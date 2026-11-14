"""Unit and integration tests for the pywebview-based gui_scraper subsystem."""

from __future__ import annotations

import io
import sys
import time
from pathlib import Path
from unittest.mock import MagicMock, patch

import httpx
import pytest

_SRC_DIR = Path(__file__).resolve().parents[2]
if str(_SRC_DIR) not in sys.path:
    sys.path.insert(0, str(_SRC_DIR))

from gui_scraper import engine  # noqa: E402
from gui_scraper.app import UI_INDEX_PATH, ScraperBridge  # noqa: E402
from gui_scraper.engine import BASE_URL, ROW_SELECTOR, ConfigurableTableScraper  # noqa: E402
from gui_scraper.models import ScrapedItem  # noqa: E402
from gui_scraper.storage import LocalMediaStore  # noqa: E402
from gui_scraper.validator import (  # noqa: E402
    is_valid_nyaa_torrent_link,
    resolve_and_validate_link,
    validate_nyaa_links,
    verify_http_status,
)

SAMPLE_HTML = """
<html>
  <body>
    <table class="torrent-list">
      <tbody>
        <tr>
          <td><a href="/?c=1_2">Anime</a></td>
          <td colspan="2">
            <a href="/view/1894001#comments" class="comments" title="4 comments"><i class="fa fa-comments-o"></i>4</a>
            <a href="/view/1894001" title="[SubsPlease] Frieren - 01 (1080p) [A1B2C3D4].mkv">[SubsPlease] Frieren - 01</a>
          </td>
          <td class="text-center">
            <a href="/download/1894001.torrent"><i class="fa fa-fw fa-download"></i></a>
            <a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&amp;dn=frieren&amp;tr=udp%3A%2F%2Ftracker.example"><i class="fa fa-fw fa-magnet"></i></a>
          </td>
          <td>1.4 GiB</td>
          <td title="2 hours ago">2026-11-14 07:22</td>
          <td>1482</td>
          <td>94</td>
          <td>28410</td>
        </tr>
        <tr>
          <td><a href="/?c=1_2">Anime</a></td>
          <td colspan="2">
            <a href="/view/1894002">Torrent Only Release</a>
          </td>
          <td class="text-center">
            <a href="/download/1894002.torrent"><i class="fa fa-fw fa-download"></i></a>
          </td>
          <td>820.5 MiB</td>
          <td title="1 day ago">2026-11-13 10:00</td>
          <td>310</td>
          <td>12</td>
          <td>4120</td>
        </tr>
        <tr>
          <td><a href="/?c=1_2">Anime</a></td>
          <td colspan="2">
            <a href="/view/1894003" title="Base32 Magnet Only">Base32 Magnet Only</a>
          </td>
          <td class="text-center">
            <a href="magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U"><i class="fa fa-fw fa-magnet"></i></a>
          </td>
          <td>2.1 GiB</td>
          <td title="3 days ago">2026-11-11 08:15</td>
          <td>95</td>
          <td>7</td>
          <td>1024</td>
        </tr>
        <tr>
          <td><a href="/?c=1_2">Anime</a></td>
          <td colspan="2">
            <a href="/view/1894004" title="Broken Validation Row">Broken Validation Row</a>
          </td>
          <td class="text-center">
            <a href="https://other.invalid/not-nyaa.txt">Download</a>
            <a href="magnet:?xt=urn:btih:BADSHORT">Magnet</a>
          </td>
          <td>150 MiB</td>
          <td title="1 week ago">2026-11-01 01:00</td>
          <td>0</td>
          <td>2</td>
          <td>5</td>
        </tr>
        <tr>
          <td>Skipped row with too few columns</td>
          <td>Only 2 tds</td>
        </tr>
      </tbody>
    </table>
  </body>
</html>
"""


def test_scraped_item_dataclass() -> None:
    item = ScrapedItem(
        id=1,
        title="Test Item",
        size="500 MB",
        date="2026-11-14",
        stats="S: 10 / L: 1",
        raw_link="/dl/test.zip",
        resolved_link="https://example.com/dl/test.zip",
        is_valid=True,
        verification_msg="✅ Valid URL",
    )
    assert item.id == 1
    assert item.resolved_link == "https://example.com/dl/test.zip"
    assert item.is_valid is True


@pytest.mark.parametrize(
    ("base_url", "raw_href", "expected_resolved", "expected_valid"),
    [
        (
            "https://example.com/search?q=linux",
            "/dl/file.iso",
            "https://example.com/dl/file.iso",
            True,
        ),
        (
            "https://example.com/dir/",
            "sub/file.zip",
            "https://example.com/dir/sub/file.zip",
            True,
        ),
        (
            "https://example.com",
            "https://cdn.example.org/app.tar.gz",
            "https://cdn.example.org/app.tar.gz",
            True,
        ),
        (
            "https://example.com",
            "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=test",
            "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=test",
            True,
        ),
        (
            "https://example.com",
            "magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U",
            "magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U",
            True,
        ),
        ("https://example.com", "", "", False),
        ("https://example.com", "#top", "#top", False),
        ("https://example.com", "javascript:void(0)", "javascript:void(0)", False),
        ("https://example.com", "ftp://example.com/a.bin", "ftp://example.com/a.bin", False),
        ("https://example.com", "http:///missing-host", "http:///missing-host", False),
        ("https://example.com", "magnet:?xt=urn:btih:BADSHORT", "magnet:?xt=urn:btih:BADSHORT", False),
        ("https://example.com", "magnet:?dn=no-xt", "magnet:?dn=no-xt", False),
    ],
)
def test_resolve_and_validate_link(
    base_url: str,
    raw_href: str,
    expected_resolved: str,
    expected_valid: bool,
) -> None:
    resolved, is_valid, msg = resolve_and_validate_link(base_url, raw_href)
    assert resolved == expected_resolved
    assert is_valid is expected_valid
    assert msg


def test_verify_http_status_mocked() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        assert request.method == "HEAD"
        if str(request.url).endswith("/ok"):
            return httpx.Response(200)
        if str(request.url).endswith("/missing"):
            return httpx.Response(404)
        raise httpx.ReadTimeout("timed out", request=request)

    transport = httpx.MockTransport(handler)
    with httpx.Client(transport=transport) as client:
        ok, ok_msg = verify_http_status("https://example.com/ok", client=client)
        assert ok is True
        assert "200" in ok_msg

        not_found, nf_msg = verify_http_status("https://example.com/missing", client=client)
        assert not_found is False
        assert "404" in nf_msg

        timed_out, to_msg = verify_http_status("https://example.com/slow", client=client)
        assert timed_out is False
        assert "timed out" in to_msg.lower()


def test_nyaa_link_validation_rules() -> None:
    assert is_valid_nyaa_torrent_link("https://nyaa.si/download/1894001.torrent") is True
    assert is_valid_nyaa_torrent_link("https://nyaa.si/view/1894001") is False
    assert is_valid_nyaa_torrent_link("https://other.org/download/1.torrent") is False

    both_ok, _ = validate_nyaa_links(
        "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567",
        "https://nyaa.si/download/1894001.torrent",
    )
    assert both_ok is True

    torrent_only_ok, _ = validate_nyaa_links("", "https://nyaa.si/download/1894002.torrent")
    assert torrent_only_ok is True

    magnet_only_ok, _ = validate_nyaa_links(
        "magnet:?xt=urn:btih:MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U",
        "",
    )
    assert magnet_only_ok is True

    neither_ok, _ = validate_nyaa_links("magnet:?xt=urn:btih:BAD", "https://example.com/a.torrent")
    assert neither_ok is False


def test_engine_constants_urls_and_live_nyaa_parsing() -> None:
    assert engine.PROVIDER_NAME == "Nyaa"
    assert engine.BASE_URL == "https://nyaa.si"
    assert engine.ROW_SELECTOR == "table.torrent-list tbody tr"
    assert engine.DEFAULT_TIMEOUT == 10.0

    default_scraper = ConfigurableTableScraper()
    assert (
        default_scraper.build_search_url("")
        == "https://nyaa.si/?q=&f=0&c=1_2&s=leechers&o=desc"
    )
    assert (
        default_scraper.build_search_url("frieren 1080p")
        == "https://nyaa.si/?f=0&c=1_2&q=frieren+1080p&s=seeders&o=desc"
    )

    transport = httpx.MockTransport(lambda _: httpx.Response(200, text=SAMPLE_HTML))
    with httpx.Client(transport=transport) as client:
        scraper = ConfigurableTableScraper(
            base_url="https://nyaa.si",
            demo_mode=False,
            client=client,
        )
        res = scraper.search("frieren 1080p")
        assert res.error is None
        # 5th HTML row has < 8 <td> cells and must be skipped
        assert len(res.items) == 4

        first = res.items[0]
        assert first.title == "[SubsPlease] Frieren - 01 (1080p) [A1B2C3D4].mkv"
        assert first.view_url == "https://nyaa.si/view/1894001"
        assert (
            first.magnet_link
            == "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=frieren&tr=udp%3A%2F%2Ftracker.example"
        )
        assert first.torrent_link == "https://nyaa.si/download/1894001.torrent"
        assert first.size == "1.4 GiB"
        assert first.date == "2026-11-14 07:22"
        assert first.relative_date == "2 hours ago"
        assert first.seeders == 1482
        assert first.leechers == 94
        assert first.downloads == 28410
        assert first.is_valid is True

        # Row 2: valid Nyaa .torrent without magnet
        assert res.items[1].torrent_link == "https://nyaa.si/download/1894002.torrent"
        assert res.items[1].is_valid is True

        # Row 3: valid Base32 magnet without .torrent
        assert res.items[2].is_valid is True

        # Row 4: broken magnet + invalid non-Nyaa link
        assert res.items[3].is_valid is False

        # Empty query hits default browse endpoint cleanly
        browse_res = scraper.search("")
        assert browse_res.error is None
        assert browse_res.request_url == "https://nyaa.si/?q=&f=0&c=1_2&s=leechers&o=desc"
        assert len(browse_res.items) == 4


def test_bridge_search_mock_and_live(tmp_path: Path) -> None:
    transport = httpx.MockTransport(lambda _: httpx.Response(200, text=SAMPLE_HTML))
    with httpx.Client(transport=transport) as client:
        scraper = ConfigurableTableScraper(
            base_url="https://nyaa.si",
            mock_delay=0.0,
            client=client,
        )
        bridge = ScraperBridge(scraper=scraper, store=LocalMediaStore(data_dir=tmp_path))

        mock_rows = bridge.search("Frieren", "mock")
        assert len(mock_rows) == 24
        assert all(isinstance(row, dict) for row in mock_rows)
        assert "Frieren" in mock_rows[0]["title"]
        assert mock_rows[0]["torrent_link"].startswith("https://nyaa.si/download/")

        browse_mock_rows = bridge.search("", "mock")
        assert len(browse_mock_rows) == 24

        page1 = bridge.search_paginated("Frieren", "mock", page=1)
        assert page1["page"] == 1
        assert page1["total_pages"] == 5
        assert page1["has_next_page"] is True
        assert len(page1["items"]) == 24
        assert page1["items"][0]["id"] == 1

        page2 = bridge.search_paginated("Frieren", "mock", page=2)
        assert page2["page"] == 2
        assert page2["has_next_page"] is True
        assert len(page2["items"]) == 24
        assert page2["items"][0]["id"] == 25
        assert page2["items"][0]["title"] != page1["items"][0]["title"]

        page5 = bridge.search_paginated("Frieren", "mock", page=5)
        assert page5["page"] == 5
        assert page5["has_next_page"] is False
        assert len(page5["items"]) == 24

        live_rows = bridge.search("Frieren", "live")
        assert len(live_rows) == 4
        assert live_rows[0]["torrent_link"] == "https://nyaa.si/download/1894001.torrent"
        assert live_rows[0]["seeders"] == 1482

        history = bridge.get_search_history()
        assert len(history) == 2
        assert history[0]["query"] == "Frieren"
        assert history[0]["mode"] == "live"


def test_scraper_pagination_url_and_html_parsing() -> None:
    scraper = ConfigurableTableScraper(base_url="https://nyaa.si", mock_delay=0.0)
    assert scraper.build_search_url("frieren 1080p", page=1) == (
        "https://nyaa.si/?f=0&c=1_2&q=frieren+1080p&s=seeders&o=desc"
    )
    assert scraper.build_search_url("frieren 1080p", page=2) == (
        "https://nyaa.si/?f=0&c=1_2&q=frieren+1080p&s=seeders&o=desc&p=2"
    )
    assert scraper.build_search_url("", page=3) == (
        "https://nyaa.si/?q=&f=0&c=1_2&s=leechers&o=desc&p=3"
    )

    pager_html = """
    <html><body>
      <ul class="pagination">
        <li class="previous"><a href="/?p=1">«</a></li>
        <li><a href="/?p=1">1</a></li>
        <li class="active"><a href="/?p=2">2</a></li>
        <li><a href="/?p=7">7</a></li>
        <li class="next"><a href="/?p=3">»</a></li>
      </ul>
    </body></html>
    """
    has_next, total_pages = scraper.parse_pagination(pager_html, current_page=2)
    assert has_next is True
    assert total_pages == 7

    last_page_html = """
    <html><body>
      <ul class="pagination">
        <li><a href="/?p=1">1</a></li>
        <li class="active"><a href="/?p=3">3</a></li>
        <li class="next disabled"><span>»</span></li>
      </ul>
    </body></html>
    """
    has_next_last, total_last = scraper.parse_pagination(last_page_html, current_page=3)
    assert has_next_last is False
    assert total_last == 3


def test_bridge_verify_url_and_config_and_external(tmp_path: Path) -> None:
    bridge = ScraperBridge(
        ConfigurableTableScraper(mock_delay=0.0),
        store=LocalMediaStore(data_dir=tmp_path),
    )

    magnet_res = bridge.verify_url(
        "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=test"
    )
    assert magnet_res["is_valid"] is True
    assert magnet_res["status"] == "valid"

    with patch("gui_scraper.app.verify_http_status", return_value=(True, "HTTP 200 OK")):
        http_res = bridge.verify_url("https://nyaa.si/download/1894001.torrent")
        assert http_res["is_valid"] is True
        assert http_res["message"] == "HTTP 200 OK"

    cfg = bridge.get_config()
    assert cfg["base_url"] == "https://nyaa.si"

    bad_save = bridge.save_config({"base_url": "ftp://invalid", "row_selector": "tr"})
    assert bad_save["ok"] is False

    good_save = bridge.save_config(
        {
            "provider_name": "Linux Mirrors",
            "base_url": "https://mirrors.example.org",
            "search_path": "/find",
            "query_param": "term",
            "row_selector": "div.result-row",
            "title_selector": "a.name",
            "link_selector": "a.dl",
            "size_selector": "span.sz",
            "timeout": "10",
        }
    )
    assert good_save["ok"] is True
    assert good_save["config"]["base_url"] == "https://mirrors.example.org"
    assert good_save["config"]["row_selector"] == "div.result-row"
    assert good_save["config"]["timeout"] == 10.0

    reset_res = bridge.reset_config()
    assert reset_res["ok"] is True
    assert reset_res["config"]["base_url"] == BASE_URL
    assert reset_res["config"]["row_selector"] == ROW_SELECTOR

    with patch("gui_scraper.app.webbrowser.open", return_value=True) as mock_open:
        assert bridge.open_external("https://mirrors.example.org/test.iso") is True
        mock_open.assert_called_once()
        assert bridge.open_external("javascript:alert(1)") is False


def test_local_media_store_and_bridge_persistence(tmp_path: Path) -> None:
    store = LocalMediaStore(data_dir=tmp_path)
    bridge = ScraperBridge(ConfigurableTableScraper(mock_delay=0.0), store=store)

    invalid_toggle = bridge.toggle_library_item({"title": ""})
    assert invalid_toggle["ok"] is False

    saved = bridge.toggle_library_item({"key": "ubuntu-24-04", "title": "Ubuntu 24.04"})
    assert saved["ok"] is True
    assert saved["saved"] is True
    assert len(bridge.get_library()) == 1

    removed = bridge.remove_library_item("ubuntu-24-04")
    assert removed["ok"] is True
    assert bridge.get_library() == []

    watch = bridge.record_watch_launch({"key": "m1", "title": "Big Buck Bunny"})
    assert len(watch) == 1
    assert watch[0]["title"] == "Big Buck Bunny"
    assert bridge.clear_watch_history() == []

    bridge.search("Big Buck Bunny", "mock")
    assert len(bridge.get_search_history()) == 1
    assert bridge.clear_search_history() == []
    assert any("Intezam bridge initialized." in line for line in bridge.get_app_logs())


def test_bridge_intezam_engine_handoff_and_validation(tmp_path: Path) -> None:
    bridge = ScraperBridge(
        ConfigurableTableScraper(mock_delay=0.0),
        store=LocalMediaStore(data_dir=tmp_path),
    )

    non_magnet = bridge.launch_intezam_stream("https://example.com/video.mp4")
    assert non_magnet["ok"] is False
    assert "magnet" in non_magnet["error"].lower()

    bad_magnet = bridge.launch_intezam_stream("magnet:?xt=urn:btih:short")
    assert bad_magnet["ok"] is False

    valid_magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=SampleVideo"
    with patch("gui_scraper.app._find_intezam_binary", return_value=None):
        missing = bridge.launch_intezam_stream(valid_magnet)
        assert missing["ok"] is False
        assert "not found" in missing["error"].lower()
        assert bridge.get_engine_status()["intezam_binary_available"] is False

    mock_proc = MagicMock()
    mock_proc.stdin = io.StringIO()
    mock_proc.stdout = io.StringIO(
        "\n".join(
            [
                "Retrieving metadata...",
                "Torrent: Sample.Video.1080p",
                "Detected Videos: 1",
                "Selected: Sample.Video.1080p.mkv",
                "Size: 1.4 GB",
                "Playback started",
            ]
        )
        + "\n"
    )
    mock_proc.poll.return_value = None
    mock_proc.wait.return_value = 0

    with patch("gui_scraper.app._find_intezam_binary", return_value=Path("/tmp/fake-intezam")):
        with patch("gui_scraper.app.subprocess.Popen", return_value=mock_proc) as popen_mock:
            status_before = bridge.get_engine_status()
            assert status_before["intezam_binary_available"] is True

            res = bridge.launch_intezam_stream(
                valid_magnet,
                title="Sample Video",
                strict_cache=True,
                timeout_seconds=90,
            )
            assert res["ok"] is True
            cmd = popen_mock.call_args.args[0]
            assert "--timeout" in cmd and "90" in cmd
            assert "--strict-cache" in cmd

            # Give monitor thread a brief moment to parse mocked stdout lines
            deadline = time.monotonic() + 2.0
            state = bridge.get_stream_status()
            while state.get("active") and time.monotonic() < deadline:
                time.sleep(0.02)
                state = bridge.get_stream_status()

            assert state["torrent_name"] == "Sample.Video.1080p"
            assert state["detected_videos"] == 1
            assert state["selected_video"] == "Sample.Video.1080p.mkv"
            assert state["video_size"] == "1.4 GB"
            assert state["returncode"] == 0
            assert len(bridge.get_watch_history()) == 1


def test_ui_html_exists_and_contains_required_elements() -> None:
    assert UI_INDEX_PATH.is_file()
    assert UI_INDEX_PATH.parts[-3:] == ("ui", "dist", "index.html")
    html = UI_INDEX_PATH.read_text(encoding="utf-8")

    # Built locally via Vite singlefile without runtime CDN dependencies
    assert "cdn.tailwindcss.com" not in html
    assert "lucide@latest" not in html
    assert "fonts.googleapis.com" not in html
    assert "<script src=" not in html

    assert "<title>INTEZAM</title>" in html
    assert "#090B12" in html
    assert "pywebview" in html
    assert "verify_url" in html
    assert "save_config" in html
    assert "launch_intezam_stream" in html
    assert "toggle_library_item" in html
    assert "Extensions" in html
    assert "Verify .torrent HTTP HEAD" in html
    assert "Open Magnet in Client" in html
    assert "search_paginated" in html
    assert "Load Page" in html
    assert "Browse Episodes" in html
    assert "Multi-Episode Range" in html
    assert "Full-Season Batch" in html
    assert "Complete-Series Pack" in html
    assert "Advanced Torrent Browser" in html


def test_info_hash_extraction_and_deduplication() -> None:
    hash_upper = "magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567&dn=ReleaseA"
    hash_lower = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=ReleaseB"
    assert engine.extract_info_hash(hash_upper) == "0123456789abcdef0123456789abcdef01234567"
    assert engine.extract_info_hash(hash_lower) == "0123456789abcdef0123456789abcdef01234567"
    assert engine.extract_info_hash("https://nyaa.si/download/1.torrent") == ""

    items = [
        ScrapedItem(
            id=1,
            title="[SubsPlease] Frieren - 01 (1080p).mkv",
            size="1.4 GiB",
            date="2026-11-14",
            stats="120S / 10L",
            raw_link=hash_upper,
            resolved_link="https://nyaa.si/view/100",
            is_valid=True,
            verification_msg="Valid",
            view_url="https://nyaa.si/view/100",
            magnet_link=hash_upper,
            seeders=120,
            leechers=10,
        ),
        ScrapedItem(
            id=2,
            title="[SubsPlease] Frieren - 01 (1080p) [Repost].mkv",
            size="1.4 GiB",
            date="2026-11-14",
            stats="450S / 25L",
            raw_link=hash_lower,
            resolved_link="https://nyaa.si/view/101",
            is_valid=True,
            verification_msg="Valid",
            view_url="https://nyaa.si/view/101",
            magnet_link=hash_lower,
            seeders=450,
            leechers=25,
        ),
        ScrapedItem(
            id=3,
            title="[Erai-raws] Frieren - 01 [720p].mkv",
            size="700 MiB",
            date="2026-11-14",
            stats="90S / 5L",
            raw_link="magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98&dn=Frieren720",
            resolved_link="https://nyaa.si/view/102",
            is_valid=True,
            verification_msg="Valid",
            view_url="https://nyaa.si/view/102",
            magnet_link="magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98&dn=Frieren720",
            seeders=90,
            leechers=5,
        ),
    ]
    deduped = engine.deduplicate_items(items)
    assert len(deduped) == 2
    assert deduped[0].seeders == 120
    assert deduped[0].id == 1
    assert deduped[1].id == 3


def test_scraper_ttl_cache_and_retry_backoff() -> None:
    call_count = 0

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal call_count
        call_count += 1
        if call_count == 1:
            return httpx.Response(503, text="Service Unavailable", request=request)
        return httpx.Response(200, text=SAMPLE_HTML, request=request)

    transport = httpx.MockTransport(handler)
    with httpx.Client(transport=transport) as client:
        scraper = ConfigurableTableScraper(
            demo_mode=False,
            client=client,
            cache_ttl=60.0,
            max_retries=2,
            retry_backoff=0.01,
        )
        first = scraper.search("Frieren", demo_mode=False, page=1)
        assert call_count == 2
        assert len(first.items) == 4

        # Second call within TTL hits cache without another HTTP request
        second = scraper.search("Frieren", demo_mode=False, page=1)
        assert call_count == 2
        assert len(second.items) == 4

        # Force refresh bypasses cache
        third = scraper.search("Frieren", demo_mode=False, page=1, force_refresh=True)
        assert call_count == 3
        assert len(third.items) == 4


def test_mock_items_support_multi_season_batches_and_resolutions() -> None:
    scraper = ConfigurableTableScraper(mock_delay=0.0)
    browse_items = scraper.get_mock_items("", page=1)
    assert len(browse_items) == 24
    titles = [item.title for item in browse_items]
    assert any("Frieren: Beyond Journey's End" in t for t in titles)
    assert any("Delicious in Dungeon" in t for t in titles)
    assert any("The Apothecary Diaries" in t for t in titles)

    show_page_1 = scraper.get_mock_items("Sousou no Frieren", page=1)
    show_titles = [item.title for item in show_page_1]
    # Multiple resolutions of EP 01
    ep1_matches = [t for t in show_titles if "Sousou no Frieren - 01 " in t]
    assert len(ep1_matches) >= 3
    # Season 2 releases
    assert any("S02E01" in t for t in show_titles)
    # Batch subtypes & unclassified special
    assert any("01-12" in t for t in show_titles)
    assert any("Season 1 Batch" in t for t in show_titles)
    assert any("Complete Series" in t for t in show_titles)
    assert any("Original Soundtrack" in t for t in show_titles)
    # Torrent-only item without magnet
    assert any(not item.magnet_link and bool(item.torrent_link) for item in show_page_1)


def test_frontend_episode_catalog_and_grouping_scenarios(tmp_path: Path) -> None:
    import subprocess

    ui_dir = UI_INDEX_PATH.parents[1]
    esbuild_bin = ui_dir / "node_modules" / ".bin" / "esbuild"
    test_entry = ui_dir / "src" / "lib" / "catalogScenariosTest.ts"
    bundle_out = tmp_path / "catalogScenariosTest.mjs"

    build_proc = subprocess.run(
        [
            str(esbuild_bin),
            str(test_entry),
            "--bundle",
            "--platform=node",
            "--format=esm",
            f"--outfile={bundle_out}",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert build_proc.returncode == 0, build_proc.stderr

    run_proc = subprocess.run(
        ["node", str(bundle_out)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run_proc.returncode == 0, run_proc.stderr
    assert "ALL_12_CATALOG_SCENARIO_TESTS_PASSED" in run_proc.stdout


def test_canonical_metadata_models_and_providers() -> None:
    from gui_scraper.metadata import (
        AniListMetadataProvider,
        CatalogMetadataService,
        TMDBMetadataProvider,
        TVMazeMetadataProvider,
    )
    from gui_scraper.models import EpisodeMetadata, MediaIdentity, SeasonMetadata

    identity = MediaIdentity(
        canonical_id="anilist:154587",
        official_title="Frieren: Beyond Journey's End",
        original_title="葬送のフリーレン",
        romaji_title="Sousou no Frieren",
        alternate_titles=["Frieren", "Sousou no Frieren"],
        release_year=2023,
        media_type="anime",
        poster_url="https://example.com/frieren.jpg",
        seasons=[
            SeasonMetadata(
                season_number=1,
                title="Season 1",
                episode_count=28,
                episodes=[EpisodeMetadata(episode_number=1, season_number=1, title="The Journey's End")],
            )
        ],
    )
    titles = identity.all_search_titles()
    assert titles[0] == "Frieren: Beyond Journey's End"
    assert "Sousou no Frieren" in titles
    assert len(titles) == len({t.lower() for t in titles})

    # TMDB normalization
    tmdb_ident = TMDBMetadataProvider.normalize_tv_payload(
        {
            "id": 1396,
            "name": "Breaking Bad",
            "original_name": "Breaking Bad",
            "first_air_date": "2008-01-20",
            "overview": "A chemistry teacher turns to making meth.",
            "poster_path": "/bb.jpg",
            "backdrop_path": "/bb_bg.jpg",
            "genres": [{"id": 18, "name": "Drama"}],
            "external_ids": {"imdb_id": "tt0903747"},
            "seasons": [{"season_number": 1, "name": "Season 1", "episode_count": 7}],
        }
    )
    assert tmdb_ident.canonical_id == "tmdb:tv:1396"
    assert tmdb_ident.imdb_id == "tt0903747"
    assert tmdb_ident.poster_url.endswith("/bb.jpg")

    # TVMaze normalization
    tvmaze_ident = TVMazeMetadataProvider.normalize_show_payload(
        {
            "id": 169,
            "name": "Breaking Bad",
            "premiered": "2008-01-20",
            "summary": "<p>High school chemistry teacher.</p>",
            "genres": ["Drama", "Crime"],
            "externals": {"imdb": "tt0903747"},
            "image": {"original": "https://static.tvmaze.com/uploads/images/original_untouched/0/2400.jpg"},
            "_embedded": {
                "seasons": [{"number": 1, "name": "Season 1", "episodeOrder": 7}],
                "episodes": [{"season": 1, "number": 1, "name": "Pilot", "airdate": "2008-01-20"}],
            },
        }
    )
    assert tvmaze_ident.canonical_id == "tvmaze:169"
    assert tvmaze_ident.imdb_id == "tt0903747"
    assert tvmaze_ident.seasons[0].episodes[0].title == "Pilot"

    # AniList normalization
    anilist_ident = AniListMetadataProvider.normalize_media_list(
        [
            {
                "id": 154587,
                "idMal": 52991,
                "format": "TV",
                "status": "FINISHED",
                "seasonYear": 2023,
                "episodes": 28,
                "title": {
                    "english": "Frieren: Beyond Journey's End",
                    "romaji": "Sousou no Frieren",
                    "native": "葬送のフリーレン",
                },
                "synonyms": ["Frieren"],
                "coverImage": {"extraLarge": "https://s4.anilist.co/frieren.jpg"},
                "bannerImage": "https://s4.anilist.co/frieren_banner.jpg",
                "genres": ["Adventure", "Fantasy"],
            }
        ]
    )[0]
    assert anilist_ident.canonical_id == "anilist:154587"
    assert anilist_ident.romaji_title == "Sousou no Frieren"

    catalog_svc = CatalogMetadataService()
    demo_shows, _ = catalog_svc.search_shows("Frieren", demo_mode=True)
    assert len(demo_shows) >= 1
    assert demo_shows[0].official_title == "Frieren: Beyond Journey's End"


def test_title_identity_matching_and_season_episode_classification() -> None:
    from gui_scraper.matching import (
        TitleIdentityMatcher,
        match_release_to_identity,
        parse_release_title,
    )
    from gui_scraper.models import MediaIdentity, SeasonMetadata, TorrentRelease

    frieren = MediaIdentity(
        canonical_id="anilist:154587",
        official_title="Frieren: Beyond Journey's End",
        romaji_title="Sousou no Frieren",
        alternate_titles=["Frieren"],
        release_year=2023,
        seasons=[
            SeasonMetadata(
                season_number=1,
                title="Season 1",
                episode_count=28,
                season_aliases=["Sousou no Frieren"],
            ),
            SeasonMetadata(
                season_number=2,
                title="Season 2",
                episode_count=24,
                season_aliases=["Sousou no Frieren 2nd Season", "Frieren: Beyond Journey's End Season 2"],
            ),
        ],
    )

    m_romaji = match_release_to_identity(
        TorrentRelease(
            release_id="nyaa:1",
            provider_id="nyaa",
            provider_name="Nyaa",
            title="[SubsPlease] Sousou no Frieren - 01 (1080p) [F02B964D].mkv",
        ),
        frieren,
    )
    assert m_romaji.classification == "episode"
    assert m_romaji.season_number == 1
    assert m_romaji.episode_number == 1

    # Season 2 via alias
    m_s2 = match_release_to_identity(
        TorrentRelease(
            release_id="nyaa:2",
            provider_id="nyaa",
            provider_name="Nyaa",
            title="[SubsPlease] Sousou no Frieren 2nd Season - 05 (1080p) [A1B2C3D4].mkv",
        ),
        frieren,
    )
    assert m_s2.classification == "episode"
    assert m_s2.season_number == 2
    assert m_s2.episode_number == 5

    # Absolute episode 30 -> Season 2 Episode 2
    m_abs = match_release_to_identity(
        TorrentRelease(
            release_id="nyaa:3",
            provider_id="nyaa",
            provider_name="Nyaa",
            title="[SubsPlease] Sousou no Frieren - 30 (1080p) [A1B2C3D4].mkv",
        ),
        frieren,
    )
    assert m_abs.classification == "episode"
    assert m_abs.season_number == 2
    assert m_abs.episode_number == 2

    # Remake / reboot year disambiguation: Hunter x Hunter (2011) vs (1999)
    hxh_2011 = MediaIdentity(
        canonical_id="anilist:11061",
        official_title="Hunter x Hunter (2011)",
        romaji_title="Hunter x Hunter",
        release_year=2011,
    )
    score_1999, _, _ = TitleIdentityMatcher.score_release_against_identity(
        parse_release_title("[Nippon] Hunter x Hunter (1999) - 01 [480p].mkv"),
        hxh_2011,
    )
    assert score_1999 < TitleIdentityMatcher.MATCH_THRESHOLD

    # Spinoff / Movie rejection
    score_movie, _, _ = TitleIdentityMatcher.score_release_against_identity(
        parse_release_title("[Group] Sousou no Frieren Movie - 01 [1080p].mkv"),
        frieren,
    )
    assert score_movie < TitleIdentityMatcher.MATCH_THRESHOLD


def test_multi_provider_discovery_deduplication_and_failure_isolation(tmp_path: Path) -> None:
    from gui_scraper.app import ScraperBridge
    from gui_scraper.models import MediaIdentity, SourceSearchFilters, TorrentRelease
    from gui_scraper.providers import (
        EZTVSourceProvider,
        MultiProviderDiscoveryEngine,
        NyaaSourceProvider,
        SourceProvider,
        SubsPleaseSourceProvider,
        deduplicate_releases,
    )

    # 1. Deduplication across providers merges mirror_providers
    shared_magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Frieren"
    r1 = TorrentRelease(
        release_id="nyaa:1",
        provider_id="nyaa",
        provider_name="Nyaa",
        title="[SubsPlease] Sousou no Frieren - 01 (1080p).mkv",
        magnet_uri=shared_magnet,
        info_hash="0123456789abcdef0123456789abcdef01234567",
        size="1.4 GiB",
        seeders=400,
    )
    r2 = TorrentRelease(
        release_id="subsplease:1",
        provider_id="subsplease",
        provider_name="SubsPlease",
        title="[SubsPlease] Sousou no Frieren - 01 (1080p).mkv",
        magnet_uri=shared_magnet.upper(),
        info_hash="0123456789ABCDEF0123456789ABCDEF01234567",
        size="1.4 GiB",
        seeders=520,
    )
    deduped = deduplicate_releases([r1, r2])
    assert len(deduped) == 1
    assert deduped[0].seeders == 520
    assert deduped[0].mirror_providers == ["Nyaa", "SubsPlease"]

    # 2. Failure isolation when one provider raises an exception
    class FailingProvider(SourceProvider):
        provider_id = "eztv"
        provider_name = "BrokenEZTV"

        def search(self, query, page=1, filters=None, demo_mode=False, force_refresh=False):
            raise RuntimeError("Simulated provider outage")

        def get_details(self, release_id: str) -> dict:
            return {}

        def normalize_result(self, raw: object) -> TorrentRelease:
            return r1

    engine_mp = MultiProviderDiscoveryEngine(
        nyaa=NyaaSourceProvider(scraper=ConfigurableTableScraper(mock_delay=0.0)),
        subsplease=SubsPleaseSourceProvider(),
        eztv=FailingProvider(),  # type: ignore[arg-type]
    )
    identity = MediaIdentity(
        canonical_id="anilist:154587",
        official_title="Frieren: Beyond Journey's End",
        romaji_title="Sousou no Frieren",
        release_year=2023,
    )
    items, status_objs, _, _ = engine_mp.discover_for_show(
        identity,
        demo_mode=True,
        page=1,
        filters=SourceSearchFilters(category="1_0"),
    )
    assert len(items) > 0
    statuses = {s.provider_id: s for s in status_objs}
    assert statuses["nyaa"].status == "ok"
    assert statuses["subsplease"].status == "ok"
    assert statuses["eztv"].status == "error"

    # 3. ScraperBridge discover_shows and discover_show_sources end-to-end
    bridge = ScraperBridge(store=LocalMediaStore(tmp_path / "store.json"))
    shows_resp = bridge.discover_shows("Frieren", mode="mock")
    assert len(shows_resp["shows"]) >= 1
    sources_resp = bridge.discover_show_sources(
        shows_resp["shows"][0],
        mode="mock",
        page=1,
        filters={"category": "1_0"},
    )
    assert len(sources_resp["items"]) >= 5
    assert any("SubsPlease" in s["provider_name"] for s in sources_resp["provider_statuses"])



