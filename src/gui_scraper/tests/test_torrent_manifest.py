from __future__ import annotations

import base64
from pathlib import Path
import sys

import httpx
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from gui_scraper.app import ScraperBridge
from gui_scraper.engine import ConfigurableTableScraper
from gui_scraper.torrent_manifest import (
    MAX_TORRENT_BYTES,
    TorrentManifestError,
    TorrentManifestInspector,
    bdecode_torrent,
    normalize_info_hash,
    parse_torrent_manifest_bytes,
)

FIXTURE_TORRENT = Path(__file__).resolve().parent / "fixtures" / "sample_batch.torrent"


def _bencode(obj: object) -> bytes:
    if isinstance(obj, int) and not isinstance(obj, bool):
        return f"i{obj}e".encode("ascii")
    if isinstance(obj, bytes):
        return f"{len(obj)}:".encode("ascii") + obj
    if isinstance(obj, list):
        return b"l" + b"".join(_bencode(x) for x in obj) + b"e"
    if isinstance(obj, dict):
        items = sorted(obj.items(), key=lambda kv: kv[0])
        return b"d" + b"".join(_bencode(k) + _bencode(v) for k, v in items) + b"e"
    raise TypeError(type(obj))


def test_sample_batch_torrent_fixture_manifest_extraction() -> None:
    """Requirement 11 & 14: Valid .torrent fixture extracts paths, sizes, BTIH, and classifies video/subtitle/auxiliary."""
    raw = FIXTURE_TORRENT.read_bytes()
    result = parse_torrent_manifest_bytes(raw)

    assert result.status == "verified"
    assert len(result.info_hash) == 40
    assert result.torrent_name == "[SubsPlease] Attack on Titan - Batch (1080p)"
    assert result.total_files == 7
    assert result.video_count == 5
    assert result.subtitle_count == 1
    assert result.auxiliary_count == 1

    paths = [f.relative_path for f in result.files]
    assert "Season 1/[SubsPlease] Attack on Titan S01E01 (1080p).mkv" in paths
    assert "Season 1/[SubsPlease] Attack on Titan S01E01.eng.ass" in paths
    assert "Specials/[SubsPlease] Attack on Titan S00E01 OVA.mkv" in paths
    assert "Extras/Bonus Recap 01.mkv" in paths
    assert "Extras/NCOP1.mkv" in paths
    assert "ReadMe.txt" in paths

    sub_entry = next(f for f in result.files if f.extension == ".ass")
    assert sub_entry.file_type == "subtitle"
    aux_entry = next(f for f in result.files if f.extension == ".txt")
    assert aux_entry.file_type == "auxiliary"


def test_single_file_torrent_and_base32_btih_normalization() -> None:
    raw = _bencode(
        {
            b"info": {
                b"length": 1_500_000_000,
                b"name": b"Attack.on.Titan.Movie.1080p.mkv",
                b"piece length": 262144,
                b"pieces": b"\x11" * 20,
            }
        }
    )
    parsed = parse_torrent_manifest_bytes(raw)
    assert parsed.total_files == 1
    assert parsed.video_count == 1
    assert parsed.files[0].filename == "Attack.on.Titan.Movie.1080p.mkv"

    b32 = base64.b32encode(bytes.fromhex(parsed.info_hash)).decode("ascii")
    assert normalize_info_hash("", f"magnet:?xt=urn:btih:{b32}") == parsed.info_hash


def test_missing_manifest_magnet_only_returns_unavailable_without_peers() -> None:
    """Requirement 12 & 20: Magnet-only release produces unavailable state and makes zero HTTP/peer requests."""
    calls = 0

    def handler(_request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        return httpx.Response(500)

    client = httpx.Client(transport=httpx.MockTransport(handler))
    inspector = TorrentManifestInspector(client=client)
    res = inspector.inspect(
        torrent_url="",
        magnet_uri="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Batch",
    )
    assert calls == 0
    assert res.status == "unavailable"
    assert res.source_kind == "magnet_only"
    assert res.files == []


@pytest.mark.parametrize(
    "bad_payload",
    [
        b"",
        b"not-bencode",
        b"d4:infod1:ai01eee",  # leading zero int
        b"d4:infod4:name3:abce",  # truncated dict
        b"i42e",  # non-dict root
        b"d7:comment3:abce",  # missing info dict
        b"d4:infod4:name4:testeeEXTRA",  # trailing bytes
    ],
)
def test_malformed_torrent_bytes_raise_safe_error(bad_payload: bytes) -> None:
    """Requirement 13: Malformed torrent metadata fails safely without crashing."""
    with pytest.raises(TorrentManifestError):
        parse_torrent_manifest_bytes(bad_payload)


def test_unsafe_path_traversal_and_hash_mismatch_rejected() -> None:
    unsafe = _bencode(
        {
            b"info": {
                b"files": [{b"length": 100, b"path": [b"..", b"etc", b"passwd"]}],
                b"name": b"bad",
                b"piece length": 16384,
                b"pieces": b"\x00" * 20,
            }
        }
    )
    with pytest.raises(TorrentManifestError, match="Unsafe path traversal"):
        parse_torrent_manifest_bytes(unsafe)

    valid_raw = FIXTURE_TORRENT.read_bytes()
    with pytest.raises(TorrentManifestError, match="info hash"):
        parse_torrent_manifest_bytes(valid_raw, expected_info_hash="0" * 40)


def test_http_inspection_safety_errors_and_btih_cache_reuse() -> None:
    """Requirement 13, 17, 20: HTML errors, HTTP failures, size caps, and BTIH manifest caching."""
    fixture_bytes = FIXTURE_TORRENT.read_bytes()
    fixture_hash = parse_torrent_manifest_bytes(fixture_bytes).info_hash
    request_count = 0

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal request_count
        request_count += 1
        url = str(request.url)
        if url.endswith("/valid.torrent"):
            return httpx.Response(200, content=fixture_bytes, headers={"Content-Type": "application/x-bittorrent"})
        if url.endswith("/html.torrent"):
            return httpx.Response(200, text="<html><body>Not found</body></html>", headers={"Content-Type": "text/html"})
        if url.endswith("/oversize.torrent"):
            return httpx.Response(200, content=b"x" * (MAX_TORRENT_BYTES + 100))
        if url.endswith("/corrupt.torrent"):
            return httpx.Response(200, content=b"d4:infocorrupt")
        return httpx.Response(404)

    client = httpx.Client(transport=httpx.MockTransport(handler))
    inspector = TorrentManifestInspector(client=client)

    # First fetch hits HTTP once
    first = inspector.inspect(
        torrent_url="https://nyaa.si/download/valid.torrent",
        info_hash=fixture_hash,
    )
    assert first.status == "verified"
    assert first.cached is False
    assert request_count == 1

    # Second fetch with same BTIH reuses cache (even when no torrent_url is passed or repeat call)
    second = inspector.inspect(torrent_url="", info_hash=fixture_hash)
    assert second.status == "verified"
    assert second.cached is True
    assert second.source_kind == "cache"
    assert request_count == 1

    # Force refresh bypasses cache
    refreshed = inspector.inspect(
        torrent_url="https://nyaa.si/download/valid.torrent",
        info_hash=fixture_hash,
        force_refresh=True,
    )
    assert refreshed.status == "verified"
    assert refreshed.cached is False
    assert request_count == 2

    # Error responses produce safe error statuses
    html_res = inspector.inspect(torrent_url="https://nyaa.si/download/html.torrent")
    assert html_res.status == "error"
    assert "HTML" in (html_res.error or "")

    big_res = inspector.inspect(torrent_url="https://nyaa.si/download/oversize.torrent")
    assert big_res.status == "error"
    assert "limit" in (big_res.error or "").lower()

    corrupt_res = inspector.inspect(torrent_url="https://nyaa.si/download/corrupt.torrent")
    assert corrupt_res.status == "error"

    not_found_res = inspector.inspect(torrent_url="https://nyaa.si/download/missing.torrent")
    assert not_found_res.status == "error"
    assert "404" in (not_found_res.error or "")


def test_bridge_inspect_torrent_manifest_never_starts_streaming() -> None:
    """Requirement 20: ScraperBridge.inspect_torrent_manifest inspects metadata only cleanly."""
    fixture_bytes = FIXTURE_TORRENT.read_bytes()
    client = httpx.Client(
        transport=httpx.MockTransport(lambda _req: httpx.Response(200, content=fixture_bytes))
    )
    inspector = TorrentManifestInspector(client=client)
    bridge = ScraperBridge(
        scraper=ConfigurableTableScraper(demo_mode=True),
        manifest_inspector=inspector,
    )

    payload = bridge.inspect_torrent_manifest(torrent_url="https://nyaa.si/download/999.torrent")
    assert payload["status"] == "verified"
    assert payload["total_files"] == 7
    assert bridge.get_stream_status()["active"] is False
    assert bridge.get_stream_status()["stage"] == "idle"
