"""Link resolution, Magnet/URL validation, and live HTTP HEAD verification."""

from __future__ import annotations

import re
from urllib.parse import parse_qs, urljoin, urlparse

import httpx

_BTIH_HASH_RE = re.compile(r"^(?:[0-9a-fA-F]{40}|[A-Za-z2-7]{32})$")
_NYAA_TORRENT_PREFIX = "https://nyaa.si/download/"
_DEFAULT_HEAD_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
    ),
}


def inspect_magnet_uri(uri: str) -> tuple[bool, str]:
    """Validate whether ``uri`` is a well-formed ``magnet:?xt=urn:btih:...`` URI."""
    cleaned = (uri or "").strip()
    if not cleaned:
        return (False, "❌ Invalid Magnet (empty URI)")
    parsed = urlparse(cleaned)
    if parsed.scheme.lower() != "magnet" or parsed.path or not cleaned.lower().startswith("magnet:?"):
        return (False, "❌ Invalid Magnet URI")

    xt_values = parse_qs(parsed.query, keep_blank_values=False).get("xt", [])
    if not xt_values:
        return (False, "❌ Invalid Magnet (missing xt)")

    for xt in xt_values:
        lowered = xt.strip().lower()
        if lowered.startswith("urn:btih:"):
            info_hash = xt.strip()[len("urn:btih:") :]
            if _BTIH_HASH_RE.fullmatch(info_hash):
                kind = "40-char Hex" if len(info_hash) == 40 else "32-char Base32"
                return (True, f"✅ Valid Magnet ({kind} BTIH)")
            return (False, "❌ Invalid Magnet (malformed BTIH hash)")

    return (False, "❌ Invalid Magnet (expected urn:btih:)")


def is_valid_nyaa_torrent_link(torrent_link: str) -> bool:
    """Return True if ``torrent_link`` starts with ``https://nyaa.si/download/`` and ends with ``.torrent``."""
    cleaned = (torrent_link or "").strip()
    if not cleaned:
        return False
    return cleaned.startswith(_NYAA_TORRENT_PREFIX) and cleaned.endswith(".torrent")


def validate_nyaa_links(magnet_link: str, torrent_link: str) -> tuple[bool, str]:
    """Validate a Nyaa row's Magnet URI and/or ``.torrent`` download URL.

    Returns ``(is_valid, verification_msg)`` where ``is_valid`` is True if either
    the magnet URI passes BTIH validation or the ``.torrent`` link is valid.
    """
    magnet_ok, magnet_msg = inspect_magnet_uri(magnet_link) if (magnet_link or "").strip() else (False, "")
    torrent_ok = is_valid_nyaa_torrent_link(torrent_link)

    if magnet_ok and torrent_ok:
        return (True, "✅ Valid Magnet & .torrent")
    if magnet_ok:
        return (True, magnet_msg)
    if torrent_ok:
        return (True, "✅ Valid Nyaa .torrent URL")
    if magnet_msg:
        return (False, magnet_msg)
    return (False, "❌ Missing valid Magnet or Nyaa .torrent URL")


def resolve_and_validate_link(base_url: str, raw_href: str) -> tuple[str, bool, str]:
    """Resolve ``raw_href`` against ``base_url`` and validate HTTP(S) or Magnet URIs.

    Returns:
        ``(resolved_link, is_valid, verification_msg)``
    """
    cleaned_href = (raw_href or "").strip()
    if not cleaned_href:
        return ("", False, "❌ Invalid (Empty link)")

    if cleaned_href.startswith("#"):
        return (cleaned_href, False, "❌ Invalid (Fragment-only link)")

    if any(ch in cleaned_href for ch in ("\r", "\n", "\t")):
        return (cleaned_href, False, "❌ Invalid (Control whitespace)")

    resolved = urljoin((base_url or "").strip(), cleaned_href)
    parsed = urlparse(resolved)
    scheme = parsed.scheme.lower()

    if scheme == "magnet":
        is_valid, msg = inspect_magnet_uri(resolved)
        if is_valid:
            return (resolved, True, "✅ Valid Magnet")
        return (resolved, False, msg)

    if scheme in {"http", "https"}:
        if bool(parsed.netloc) and bool(parsed.hostname):
            return (resolved, True, "✅ Valid URL")
        return (resolved, False, "❌ Invalid URL (missing host)")

    scheme_label = scheme if scheme else "relative/unknown"
    return (resolved, False, f"❌ Invalid ({scheme_label} scheme)")


def verify_http_status(
    url: str,
    timeout: float = 5.0,
    headers: dict[str, str] | None = None,
    client: httpx.Client | None = None,
) -> tuple[bool, str]:
    """Send a fast ``HEAD`` request (5s timeout) to check if ``url`` returns HTTP 200 OK."""
    cleaned = (url or "").strip()
    if not cleaned:
        return (False, "Empty URL cannot be tested.")

    parsed = urlparse(cleaned)
    scheme = parsed.scheme.lower()
    if scheme == "magnet":
        is_valid, msg = inspect_magnet_uri(cleaned)
        return (is_valid, msg)

    if scheme not in {"http", "https"} or not parsed.hostname:
        return (False, f"Unsupported or invalid URL for HTTP HEAD: {cleaned}")

    request_headers = dict(headers) if headers is not None else dict(_DEFAULT_HEAD_HEADERS)

    try:
        if client is not None:
            response = client.head(cleaned, timeout=timeout, follow_redirects=True)
        else:
            with httpx.Client(
                timeout=timeout,
                headers=request_headers,
                follow_redirects=True,
            ) as owned_client:
                response = owned_client.head(cleaned)

        reason = response.reason_phrase or ""
        status_text = f"HTTP {response.status_code} {reason}".strip()
        return (response.status_code == 200, status_text)
    except httpx.TimeoutException:
        return (False, f"HEAD request timed out after {timeout:.1f}s")
    except httpx.HTTPError as exc:
        return (False, f"HTTP request error: {exc}")
