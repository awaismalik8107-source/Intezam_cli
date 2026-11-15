"""Metadata-only .torrent inspection, safe bencode decoding, manifest extraction, and BTIH caching."""

from __future__ import annotations

import base64
from dataclasses import asdict, dataclass, field, replace
import hashlib
import ipaddress
from pathlib import PurePosixPath
import re
import socket
import threading
import time
from typing import Any, Callable
from urllib.parse import parse_qs, urljoin, urlparse

import httpx

MAX_TORRENT_BYTES = 5 * 1024 * 1024  # 5 MiB safety cap for .torrent metadata files
DEFAULT_TORRENT_TIMEOUT = 8.0
DEFAULT_CACHE_TTL_SECONDS = 30 * 60.0
_MAX_BENCODE_DEPTH = 32
_MAX_BENCODE_ITEMS = 100_000

_VIDEO_EXTS = frozenset({
    ".mkv", ".mp4", ".avi", ".webm", ".m4v", ".ts", ".m2ts",
    ".mov", ".mpg", ".mpeg", ".wmv", ".flv", ".ogm", ".vob",
})
_SUBTITLE_EXTS = frozenset({
    ".ass", ".ssa", ".srt", ".vtt", ".sub", ".idx", ".sup", ".pgs", ".smi",
})

_HEX_BTIH_RE = re.compile(r"^[0-9a-f]{40}$", re.IGNORECASE)
_BASE32_BTIH_RE = re.compile(r"^[A-Z2-7]{32}$", re.IGNORECASE)
_WINDOWS_DRIVE_RE = re.compile(r"^[A-Za-z]:")


class TorrentManifestError(ValueError):
    """Raised when a .torrent payload is malformed, oversized, or fails path safety checks."""


@dataclass(slots=True)
class TorrentManifestFile:
    """Single validated file entry inside a .torrent metadata manifest."""

    relative_path: str
    filename: str
    size_bytes: int
    size_label: str
    file_type: str  # "video" | "subtitle" | "auxiliary"
    extension: str = ""
    torrent_file_index: int = 0

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class TorrentManifestResult:
    """Normalized metadata-only inspection result returned to the pywebview frontend."""

    status: str  # "verified" | "unavailable" | "error"
    info_hash: str = ""
    source_kind: str = "magnet_only"  # "torrent_url" | "magnet_only" | "cache"
    cached: bool = False
    torrent_name: str = ""
    total_files: int = 0
    total_size_bytes: int = 0
    total_size_label: str = "0 B"
    video_count: int = 0
    subtitle_count: int = 0
    auxiliary_count: int = 0
    error: str | None = None
    inspection_note: str = ""
    files: list[TorrentManifestFile] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "status": self.status,
            "info_hash": self.info_hash,
            "source_kind": self.source_kind,
            "cached": self.cached,
            "torrent_name": self.torrent_name,
            "total_files": self.total_files,
            "total_size_bytes": self.total_size_bytes,
            "total_size_label": self.total_size_label,
            "video_count": self.video_count,
            "subtitle_count": self.subtitle_count,
            "auxiliary_count": self.auxiliary_count,
            "error": self.error,
            "inspection_note": self.inspection_note,
            "files": [f.to_dict() for f in self.files],
        }


def format_byte_size(num_bytes: int) -> str:
    """Format byte counts into readable IEC units (KiB/MiB/GiB)."""
    value = float(max(0, num_bytes))
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            if unit == "B":
                return f"{int(value)} B"
            return f"{value:.2f} {unit}".rstrip("0").rstrip(".") if value < 10 else f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{int(num_bytes)} B"


def normalize_info_hash(raw_hash: str = "", magnet_uri: str = "") -> str:
    """Normalize a 40-char hex or 32-char base32 BTIH into lowercase 40-char hex."""
    candidates: list[str] = []
    if raw_hash and raw_hash.strip():
        candidates.append(raw_hash.strip())
    if magnet_uri and magnet_uri.strip().lower().startswith("magnet:?"):
        parsed = urlparse(magnet_uri.strip())
        qs = parse_qs(parsed.query)
        for xt in qs.get("xt", []):
            if xt.lower().startswith("urn:btih:"):
                candidates.append(xt[9:].strip())

    for cand in candidates:
        if _HEX_BTIH_RE.match(cand):
            return cand.lower()
        if _BASE32_BTIH_RE.match(cand):
            try:
                decoded = base64.b32decode(cand.upper(), casefold=True)
                if len(decoded) == 20:
                    return decoded.hex().lower()
            except Exception:
                continue
    return ""


def classify_file_type(filename: str) -> tuple[str, str]:
    """Classify a filename into ('video' | 'subtitle' | 'auxiliary', lowercase extension)."""
    ext = PurePosixPath(filename).suffix.lower()
    if ext in _VIDEO_EXTS:
        return ("video", ext)
    if ext in _SUBTITLE_EXTS:
        return ("subtitle", ext)
    return ("auxiliary", ext)


def _decode_utf8_text(raw: Any) -> str:
    if isinstance(raw, bytes):
        return raw.decode("utf-8", errors="replace")
    if isinstance(raw, str):
        return raw
    return ""


def _validate_path_segments(segments: list[str]) -> str:
    """Validate and join relative torrent manifest segments without permitting traversal."""
    if not segments:
        raise TorrentManifestError("Torrent manifest contains an empty file path.")
    cleaned: list[str] = []
    for seg in segments:
        if "\x00" in seg:
            raise TorrentManifestError("Torrent manifest contains null bytes in file path.")
        part = seg.replace("\\", "/").strip()
        if not part or part in {".", ".."} or "/" in part:
            # Split nested slash-bearing segments only if benign; reject traversal or absolute paths.
            if part.startswith("/") or _WINDOWS_DRIVE_RE.match(part) or ".." in part.split("/"):
                raise TorrentManifestError(f"Unsafe path traversal detected in torrent manifest: {seg!r}")
            subparts = [p for p in part.split("/") if p and p != "."]
            if not subparts:
                raise TorrentManifestError("Torrent manifest contains an empty path component.")
            for sp in subparts:
                if sp == ".." or _WINDOWS_DRIVE_RE.match(sp):
                    raise TorrentManifestError(f"Unsafe path traversal detected in torrent manifest: {seg!r}")
                cleaned.append(sp)
            continue
        if _WINDOWS_DRIVE_RE.match(part):
            raise TorrentManifestError(f"Unsafe drive path in torrent manifest: {seg!r}")
        cleaned.append(part)
    return "/".join(cleaned)


def bdecode_torrent(data: bytes) -> tuple[dict[bytes, Any], bytes | None]:
    """Safely decode a bencoded .torrent payload and return (root_dict, raw_info_bytes)."""
    if not isinstance(data, (bytes, bytearray)) or len(data) == 0:
        raise TorrentManifestError("Torrent payload is empty.")
    buf = bytes(data)
    if len(buf) > MAX_TORRENT_BYTES:
        raise TorrentManifestError(f"Torrent metadata exceeds {MAX_TORRENT_BYTES // (1024 * 1024)} MiB safety limit.")

    n = len(buf)
    pos = 0
    items_seen = 0
    info_slice: list[bytes | None] = [None]

    def _parse(depth: int, capture_info_key: bool = False) -> Any:
        nonlocal pos, items_seen
        if depth > _MAX_BENCODE_DEPTH:
            raise TorrentManifestError("Torrent metadata exceeds maximum nesting depth.")
        items_seen += 1
        if items_seen > _MAX_BENCODE_ITEMS:
            raise TorrentManifestError("Torrent metadata exceeds maximum item count.")
        if pos >= n:
            raise TorrentManifestError("Unexpected end of torrent metadata.")

        ch = buf[pos : pos + 1]
        if ch == b"i":
            end = buf.find(b"e", pos + 1)
            if end == -1:
                raise TorrentManifestError("Unterminated bencode integer.")
            num_bytes = buf[pos + 1 : end]
            if not num_bytes:
                raise TorrentManifestError("Empty bencode integer.")
            if num_bytes == b"-0" or (
                len(num_bytes) > 1 and (num_bytes.startswith(b"0") or num_bytes.startswith(b"-0"))
            ):
                raise TorrentManifestError("Invalid leading zero in bencode integer.")
            try:
                val = int(num_bytes.decode("ascii"))
            except (UnicodeDecodeError, ValueError) as exc:
                raise TorrentManifestError("Malformed bencode integer.") from exc
            pos = end + 1
            return val

        if ch == b"l":
            pos += 1
            out_list: list[Any] = []
            while True:
                if pos >= n:
                    raise TorrentManifestError("Unterminated bencode list.")
                if buf[pos : pos + 1] == b"e":
                    pos += 1
                    return out_list
                out_list.append(_parse(depth + 1))

        if ch == b"d":
            pos += 1
            out_dict: dict[bytes, Any] = {}
            while True:
                if pos >= n:
                    raise TorrentManifestError("Unterminated bencode dictionary.")
                if buf[pos : pos + 1] == b"e":
                    pos += 1
                    return out_dict
                key = _parse(depth + 1)
                if not isinstance(key, bytes):
                    raise TorrentManifestError("Bencode dictionary keys must be byte strings.")
                val_start = pos
                val = _parse(depth + 1)
                val_end = pos
                if capture_info_key and key == b"info" and info_slice[0] is None:
                    info_slice[0] = buf[val_start:val_end]
                out_dict[key] = val

        if b"0" <= ch <= b"9":
            colon = buf.find(b":", pos)
            if colon == -1:
                raise TorrentManifestError("Malformed bencode byte-string length.")
            len_bytes = buf[pos:colon]
            if len(len_bytes) > 1 and len_bytes.startswith(b"0"):
                raise TorrentManifestError("Invalid leading zero in bencode byte-string length.")
            try:
                length = int(len_bytes.decode("ascii"))
            except (UnicodeDecodeError, ValueError) as exc:
                raise TorrentManifestError("Invalid bencode byte-string length.") from exc
            start = colon + 1
            end = start + length
            if length < 0 or end > n:
                raise TorrentManifestError("Bencode byte-string extends past end of file.")
            pos = end
            return buf[start:end]

        raise TorrentManifestError(f"Invalid bencode token at byte offset {pos}.")

    root = _parse(0, capture_info_key=True)
    if pos != n:
        raise TorrentManifestError("Trailing unexpected bytes after bencode root dictionary.")
    if not isinstance(root, dict):
        raise TorrentManifestError("Torrent root structure must be a bencode dictionary.")
    return (root, info_slice[0])


def parse_torrent_manifest_bytes(
    data: bytes,
    expected_info_hash: str = "",
) -> TorrentManifestResult:
    """Extract a validated file manifest directly from raw .torrent metadata bytes."""
    root, raw_info = bdecode_torrent(data)
    info = root.get(b"info")
    if not isinstance(info, dict) or raw_info is None:
        raise TorrentManifestError("Torrent file does not contain a valid 'info' dictionary.")

    computed_hash = hashlib.sha1(raw_info).hexdigest().lower()
    expected = normalize_info_hash(expected_info_hash)
    if expected and expected != computed_hash:
        raise TorrentManifestError(
            f"Torrent metadata info hash ({computed_hash[:12]}...) does not match release BTIH ({expected[:12]}...)."
        )

    raw_name = info.get(b"name.utf-8") or info.get(b"name")
    torrent_name = _decode_utf8_text(raw_name).strip()
    files: list[TorrentManifestFile] = []

    if b"files" in info:
        raw_files = info.get(b"files")
        if not isinstance(raw_files, list) or len(raw_files) == 0:
            raise TorrentManifestError("Multi-file torrent contains an empty or invalid 'files' list.")
        for idx, entry in enumerate(raw_files):
            if not isinstance(entry, dict):
                raise TorrentManifestError("Malformed file entry inside torrent manifest.")
            attr_raw = entry.get(b"attr")
            if isinstance(attr_raw, bytes) and (b"p" in attr_raw or b"l" in attr_raw):
                # Skip BEP 47 padding or symlink entries while preserving libtorrent's 0-based file_index_t.
                continue
            length = entry.get(b"length")
            if not isinstance(length, int) or isinstance(length, bool) or length < 0:
                raise TorrentManifestError("Invalid negative or non-integer file length in torrent manifest.")
            raw_path = entry.get(b"path.utf-8") or entry.get(b"path")
            if not isinstance(raw_path, list) or not raw_path:
                raise TorrentManifestError("Missing path list in multi-file torrent entry.")
            seg_strs = [_decode_utf8_text(seg) for seg in raw_path]
            if seg_strs[0] == ".pad":
                continue
            rel_path = _validate_path_segments(seg_strs)
            filename = rel_path.rsplit("/", 1)[-1]
            ftype, ext = classify_file_type(filename)
            files.append(
                TorrentManifestFile(
                    relative_path=rel_path,
                    filename=filename,
                    size_bytes=length,
                    size_label=format_byte_size(length),
                    file_type=ftype,
                    extension=ext,
                    torrent_file_index=idx,
                )
            )
    elif b"length" in info:
        length = info.get(b"length")
        if not isinstance(length, int) or isinstance(length, bool) or length < 0:
            raise TorrentManifestError("Invalid file length in single-file torrent manifest.")
        safe_name = _validate_path_segments([torrent_name or "unnamed.bin"])
        filename = safe_name.rsplit("/", 1)[-1]
        ftype, ext = classify_file_type(filename)
        files.append(
            TorrentManifestFile(
                relative_path=safe_name,
                filename=filename,
                size_bytes=length,
                size_label=format_byte_size(length),
                file_type=ftype,
                extension=ext,
                torrent_file_index=0,
            )
        )
    else:
        raise TorrentManifestError("Torrent 'info' dictionary is missing both 'files' and 'length'.")

    total_size = sum(f.size_bytes for f in files)
    video_count = sum(1 for f in files if f.file_type == "video")
    subtitle_count = sum(1 for f in files if f.file_type == "subtitle")
    aux_count = sum(1 for f in files if f.file_type == "auxiliary")

    return TorrentManifestResult(
        status="verified",
        info_hash=computed_hash,
        source_kind="torrent_url",
        cached=False,
        torrent_name=torrent_name,
        total_files=len(files),
        total_size_bytes=total_size,
        total_size_label=format_byte_size(total_size),
        video_count=video_count,
        subtitle_count=subtitle_count,
        auxiliary_count=aux_count,
        error=None,
        inspection_note=(
            f"Verified .torrent manifest metadata only ({len(files)} files, {format_byte_size(total_size)} total; "
            "no video payload downloaded)."
        ),
        files=files,
    )


def _is_disallowed_ip(ip_obj: ipaddress.IPv4Address | ipaddress.IPv6Address) -> bool:
    return (
        ip_obj.is_private
        or ip_obj.is_loopback
        or ip_obj.is_link_local
        or ip_obj.is_multicast
        or ip_obj.is_reserved
        or ip_obj.is_unspecified
    )


def validate_remote_torrent_url(
    torrent_url: str,
    *,
    allow_private_urls: bool = False,
    resolve_dns: bool = True,
) -> str:
    """Validate HTTP(S) torrent URL and reject loopback, private, link-local, or local hosts."""
    parsed = urlparse((torrent_url or "").strip())
    scheme = parsed.scheme.lower()
    if scheme not in {"http", "https"} or not parsed.netloc or not parsed.hostname:
        raise TorrentManifestError("Torrent URL must be a valid http:// or https:// URL.")
    if parsed.username or parsed.password:
        raise TorrentManifestError("Torrent URLs with embedded credentials are not allowed.")

    host = parsed.hostname.strip().lower().rstrip(".")
    if not host:
        raise TorrentManifestError("Torrent URL is missing a valid hostname.")

    if not allow_private_urls:
        if host in {"localhost", "ip6-localhost"} or host.endswith((".localhost", ".local", ".internal")):
            raise TorrentManifestError("Blocked unsafe local/private host in torrent URL.")
        try:
            literal_ip = ipaddress.ip_address(host)
        except ValueError:
            literal_ip = None
        if literal_ip is not None:
            if _is_disallowed_ip(literal_ip):
                raise TorrentManifestError("Blocked loopback/private/link-local IP in torrent URL.")
        elif resolve_dns:
            try:
                port = parsed.port or (443 if scheme == "https" else 80)
                addr_info = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
            except OSError as exc:
                raise TorrentManifestError(f"Unable to resolve torrent host {host!r}: {exc}") from exc
            resolved_any = False
            for info_entry in addr_info:
                sockaddr = info_entry[4]
                if not sockaddr:
                    continue
                ip_text = str(sockaddr[0]).split("%", 1)[0]
                try:
                    addr_ip = ipaddress.ip_address(ip_text)
                except ValueError:
                    continue
                resolved_any = True
                if _is_disallowed_ip(addr_ip):
                    raise TorrentManifestError(
                        f"Blocked resolved private/loopback address ({ip_text}) for torrent URL."
                    )
            if not resolved_any:
                raise TorrentManifestError(f"Torrent host {host!r} did not resolve to any valid IP address.")

    return parsed.geturl()


class TorrentManifestInspector:
    """Thread-safe metadata-only .torrent inspector with BTIH/URL manifest caching and SSRF guards."""

    def __init__(
        self,
        *,
        client: httpx.Client | None = None,
        timeout: float = DEFAULT_TORRENT_TIMEOUT,
        user_agent: str = "Intezam-Scraper/2.0",
        cache_ttl: float = DEFAULT_CACHE_TTL_SECONDS,
        max_bytes: int = MAX_TORRENT_BYTES,
        allow_private_urls: bool = False,
        magnet_metadata_fetcher: Callable[[str, float, str], TorrentManifestResult] | None = None,
    ) -> None:
        self._client = client
        self.timeout = float(timeout)
        self.user_agent = user_agent
        self.cache_ttl = float(cache_ttl)
        self.max_bytes = int(max_bytes)
        self.allow_private_urls = bool(allow_private_urls)
        self.magnet_metadata_fetcher = magnet_metadata_fetcher
        self._lock = threading.Lock()
        self._cache_by_hash: dict[str, tuple[float, TorrentManifestResult]] = {}
        self._cache_by_url: dict[str, tuple[float, TorrentManifestResult]] = {}

    def _get_cached(self, info_hash: str, torrent_url: str) -> TorrentManifestResult | None:
        now = time.monotonic()
        with self._lock:
            if info_hash and info_hash in self._cache_by_hash:
                ts, entry = self._cache_by_hash[info_hash]
                if now - ts <= self.cache_ttl:
                    return replace(entry, cached=True, source_kind="cache", files=list(entry.files))
                del self._cache_by_hash[info_hash]
            if torrent_url and torrent_url in self._cache_by_url:
                ts, entry = self._cache_by_url[torrent_url]
                if now - ts <= self.cache_ttl:
                    return replace(entry, cached=True, source_kind="cache", files=list(entry.files))
                del self._cache_by_url[torrent_url]
        return None

    def _store_cache(self, result: TorrentManifestResult, torrent_url: str) -> None:
        if result.status != "verified":
            return
        now = time.monotonic()
        with self._lock:
            if result.info_hash:
                self._cache_by_hash[result.info_hash] = (now, result)
            if torrent_url:
                self._cache_by_url[torrent_url] = (now, result)

    def _fetch_torrent_bytes(self, torrent_url: str) -> bytes:
        resolve_dns = self._client is None
        current_url = validate_remote_torrent_url(
            torrent_url,
            allow_private_urls=self.allow_private_urls,
            resolve_dns=resolve_dns,
        )

        headers = {
            "User-Agent": self.user_agent,
            "Accept": "application/x-bittorrent, application/octet-stream, */*;q=0.5",
        }
        own_client = self._client is None
        client = self._client or httpx.Client(
            timeout=self.timeout,
            follow_redirects=False,
            headers=headers,
        )
        max_redirects = 5
        try:
            for _hop in range(max_redirects + 1):
                with client.stream("GET", current_url, headers=headers) as resp:
                    if resp.status_code in {301, 302, 303, 307, 308}:
                        location = resp.headers.get("Location") or resp.headers.get("location")
                        if not location:
                            raise TorrentManifestError("Redirect response missing Location header.")
                        next_url = urljoin(current_url, location.strip())
                        current_url = validate_remote_torrent_url(
                            next_url,
                            allow_private_urls=self.allow_private_urls,
                            resolve_dns=resolve_dns,
                        )
                        continue
                    resp.raise_for_status()
                    content_len = resp.headers.get("Content-Length") or resp.headers.get("content-length")
                    if content_len and content_len.isdigit() and int(content_len) > self.max_bytes:
                        raise TorrentManifestError(
                            f"Remote .torrent file exceeds {self.max_bytes // (1024 * 1024)} MiB safety limit."
                        )
                    content_type = (resp.headers.get("Content-Type") or "").lower()
                    chunks: list[bytes] = []
                    total = 0
                    for chunk in resp.iter_bytes(chunk_size=16384):
                        if not chunk:
                            continue
                        total += len(chunk)
                        if total > self.max_bytes:
                            raise TorrentManifestError(
                                f"Remote .torrent response exceeded {self.max_bytes // (1024 * 1024)} MiB safety limit."
                            )
                        chunks.append(chunk)
                    payload = b"".join(chunks)
                    if "text/html" in content_type or payload.lstrip().startswith((b"<!DOCTYPE", b"<html", b"<HTML")):
                        raise TorrentManifestError(
                            "Provider returned an HTML page instead of a .torrent metadata file."
                        )
                    return payload
            raise TorrentManifestError("Too many redirects while fetching .torrent metadata file.")
        finally:
            if own_client:
                client.close()

    def inspect(
        self,
        *,
        torrent_url: str = "",
        magnet_uri: str = "",
        info_hash: str = "",
        force_refresh: bool = False,
        allow_magnet_peers: bool = False,
        magnet_timeout: float = 15.0,
    ) -> TorrentManifestResult:
        """Inspect a release's metadata manifest without downloading video payloads."""
        norm_url = (torrent_url or "").strip()
        norm_magnet = (magnet_uri or "").strip()
        norm_hash = normalize_info_hash(info_hash, norm_magnet)

        if not force_refresh:
            cached = self._get_cached(norm_hash, norm_url)
            if cached is not None:
                return cached

        if norm_url:
            try:
                raw_bytes = self._fetch_torrent_bytes(norm_url)
                result = parse_torrent_manifest_bytes(raw_bytes, expected_info_hash=norm_hash)
                self._store_cache(result, norm_url)
                return result
            except httpx.TimeoutException:
                url_error = TorrentManifestResult(
                    status="error",
                    info_hash=norm_hash,
                    source_kind="torrent_url",
                    error="Timed out while fetching .torrent metadata file.",
                    inspection_note="Torrent file inspection timed out without downloading payload.",
                )
            except httpx.HTTPStatusError as exc:
                status_code = exc.response.status_code if exc.response is not None else "HTTP"
                url_error = TorrentManifestResult(
                    status="error",
                    info_hash=norm_hash,
                    source_kind="torrent_url",
                    error=f"Provider returned status {status_code} for .torrent metadata URL.",
                    inspection_note="Torrent file inspection failed due to HTTP error.",
                )
            except TorrentManifestError as exc:
                url_error = TorrentManifestResult(
                    status="error",
                    info_hash=norm_hash,
                    source_kind="torrent_url",
                    error=str(exc),
                    inspection_note="Torrent metadata failed format or safety validation.",
                )
            except Exception as exc:
                url_error = TorrentManifestResult(
                    status="error",
                    info_hash=norm_hash,
                    source_kind="torrent_url",
                    error=f"Unable to inspect torrent metadata: {exc}",
                    inspection_note="Unexpected error during metadata-only torrent inspection.",
                )

            if not (allow_magnet_peers and norm_magnet and self.magnet_metadata_fetcher is not None):
                return url_error

        if allow_magnet_peers and norm_magnet and self.magnet_metadata_fetcher is not None:
            try:
                mag_result = self.magnet_metadata_fetcher(norm_magnet, float(magnet_timeout), norm_hash)
                if mag_result.status == "verified":
                    self._store_cache(mag_result, norm_url)
                return mag_result
            except Exception as exc:
                return TorrentManifestResult(
                    status="error",
                    info_hash=norm_hash,
                    source_kind="libtorrent_magnet",
                    error=f"Magnet metadata retrieval failed: {exc}",
                    inspection_note="BitTorrent metadata-only retrieval failed without payload download.",
                )

        return TorrentManifestResult(
            status="unavailable",
            info_hash=norm_hash,
            source_kind="magnet_only",
            cached=False,
            error=None,
            inspection_note=(
                "Unavailable: this release only has a magnet URI with no .torrent metadata URL. "
                "Automatic BitTorrent peer discovery is disabled."
            ),
        )
