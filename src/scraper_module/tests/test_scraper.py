"""Unit and integration tests for the isolated scraper_module subsystem."""

from __future__ import annotations

import asyncio
import sys
from io import StringIO
from pathlib import Path
from unittest.mock import MagicMock, patch

import httpx
import pytest
from rich.console import Console

# Ensure `src/` is on sys.path so tests work regardless of working directory.
_SRC_DIR = Path(__file__).resolve().parents[2]
if str(_SRC_DIR) not in sys.path:
    sys.path.insert(0, str(_SRC_DIR))

from scraper_module import engine  # noqa: E402
from scraper_module.engine import CustomTableScraper  # noqa: E402
from scraper_module.main import parse_args  # noqa: E402
from scraper_module.models import ScrapedResult  # noqa: E402
from scraper_module.ui import (  # noqa: E402
    build_detail_panel,
    build_results_table,
    copy_to_clipboard,
    fetch_results_with_spinner,
    run_cli,
)
from scraper_module.validator import verify_link  # noqa: E402

SAMPLE_HTML = """
<html>
  <body>
    <table class="results">
      <tbody>
        <tr>
          <td class="title">Ubuntu 24.04 LTS Desktop ISO</td>
          <td class="size">5.7 GB</td>
          <td class="date">2026-11-01</td>
          <td class="stats">S: 420 / L: 12</td>
          <td class="link"><a href="/torrents/ubuntu-24.04.torrent">Download</a></td>
        </tr>
        <tr>
          <td class="title">Arch Linux 2026.11 Magnet</td>
          <td class="size">1.2 GB</td>
          <td class="date">2026-11-02</td>
          <td class="stats">S: 900 / L: 30</td>
          <td class="link"><a href="magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&amp;dn=archlinux">Magnet</a></td>
        </tr>
        <tr>
          <td class="title">Broken JS Link Entry</td>
          <td class="size">800 MB</td>
          <td class="date">2026-10-15</td>
          <td class="stats">S: 0 / L: 1</td>
          <td class="link"><a href="javascript:void(0)">Broken</a></td>
        </tr>
      </tbody>
    </table>
  </body>
</html>
"""


def test_scraped_result_dataclass_fields() -> None:
    item = ScrapedResult(
        title="Sample",
        size="1.0 GB",
        date="2026-11-14",
        stats="S: 10 / L: 2",
        download_uri="https://example.com/item.torrent",
        is_valid_link=True,
    )
    assert item.title == "Sample"
    assert item.size == "1.0 GB"
    assert item.date == "2026-11-14"
    assert item.stats == "S: 10 / L: 2"
    assert item.download_uri == "https://example.com/item.torrent"
    assert item.is_valid_link is True


@pytest.mark.parametrize(
    ("base_url", "raw_href", "expected_uri", "expected_valid"),
    [
        (
            "https://example.com/search?q=linux",
            "/download/123.torrent",
            "https://example.com/download/123.torrent",
            True,
        ),
        (
            "https://example.com/browse/",
            "files/item.torrent",
            "https://example.com/browse/files/item.torrent",
            True,
        ),
        (
            "https://example.com",
            "//cdn.example.com/file.torrent",
            "https://cdn.example.com/file.torrent",
            True,
        ),
        (
            "https://example.com",
            "http://mirror.example.org/test.iso",
            "http://mirror.example.org/test.iso",
            True,
        ),
        (
            "https://example.com",
            "magnet:?xt=urn:btih:ABCDEF1234567890&dn=Test",
            "magnet:?xt=urn:btih:ABCDEF1234567890&dn=Test",
            True,
        ),
        ("https://example.com", "", "", False),
        ("https://example.com", "   ", "", False),
        ("https://example.com", "#section", "#section", False),
        ("https://example.com", "javascript:void(0)", "javascript:void(0)", False),
        ("https://example.com", "mailto:admin@example.com", "mailto:admin@example.com", False),
        ("https://example.com", "ftp://files.example.com/a.iso", "ftp://files.example.com/a.iso", False),
        ("https://example.com", "http:///missing-host", "http:///missing-host", False),
        ("https://example.com", "magnet:?dn=missing_xt", "magnet:?dn=missing_xt", False),
        ("https://example.com", "magnet:?xt=", "magnet:?xt=", False),
    ],
)
def test_verify_link(
    base_url: str,
    raw_href: str,
    expected_uri: str,
    expected_valid: bool,
) -> None:
    uri, is_valid = verify_link(base_url, raw_href)
    assert uri == expected_uri
    assert is_valid is expected_valid


def test_engine_exposes_placeholder_constants() -> None:
    assert isinstance(engine.TARGET_BASE_URL, str) and engine.TARGET_BASE_URL
    assert isinstance(engine.ROW_SELECTOR, str) and engine.ROW_SELECTOR
    assert isinstance(engine.TITLE_SELECTOR, str) and engine.TITLE_SELECTOR
    assert isinstance(engine.LINK_SELECTOR, str) and engine.LINK_SELECTOR
    assert isinstance(engine.SIZE_SELECTOR, str) and engine.SIZE_SELECTOR


def test_scraper_parse_html() -> None:
    scraper = CustomTableScraper(base_url="https://tracker.example.org")
    results = scraper.parse_html(SAMPLE_HTML)

    assert len(results) == 3

    assert results[0].title == "Ubuntu 24.04 LTS Desktop ISO"
    assert results[0].size == "5.7 GB"
    assert results[0].date == "2026-11-01"
    assert results[0].stats == "S: 420 / L: 12"
    assert results[0].download_uri == "https://tracker.example.org/torrents/ubuntu-24.04.torrent"
    assert results[0].is_valid_link is True

    assert results[1].title == "Arch Linux 2026.11 Magnet"
    assert results[1].download_uri.startswith("magnet:?xt=urn:btih:")
    assert results[1].is_valid_link is True

    assert results[2].title == "Broken JS Link Entry"
    assert results[2].download_uri == "javascript:void(0)"
    assert results[2].is_valid_link is False


def test_scraper_async_search_with_mock_transport() -> None:
    captured_urls: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        captured_urls.append(str(request.url))
        return httpx.Response(200, text=SAMPLE_HTML)

    transport = httpx.MockTransport(handler)

    async def _run() -> list[ScrapedResult]:
        async with httpx.AsyncClient(transport=transport) as client:
            scraper = CustomTableScraper(
                base_url="https://tracker.example.org",
                client=client,
            )
            empty_res = await scraper.search("   ")
            assert empty_res == []
            return await scraper.search("ubuntu linux")

    results = asyncio.run(_run())
    assert captured_urls == ["https://tracker.example.org/search?q=ubuntu+linux"]
    assert len(results) == 3
    assert results[0].is_valid_link is True
    assert results[2].is_valid_link is False


def test_ui_table_and_detail_panel_rendering() -> None:
    items = [
        ScrapedResult(
            title="Fedora Workstation 43",
            size="2.4 GB",
            date="2026-11-10",
            stats="S: 50 / L: 5",
            download_uri="https://example.com/fedora-workstation-43-very-long-uri-path.torrent",
            is_valid_link=True,
        ),
        ScrapedResult(
            title="Bad Entry",
            size="10 MB",
            date="2026-11-01",
            stats="S: 0 / L: 0",
            download_uri="javascript:void(0)",
            is_valid_link=False,
        ),
    ]

    buffer = StringIO()
    console = Console(file=buffer, force_terminal=False, width=120)
    console.print(build_results_table(items, "fedora"))
    console.print(build_detail_panel(items[0], 1))

    output = buffer.getvalue()
    assert "Valid Link" in output
    assert "PASS" in output
    assert "FAIL" in output
    assert "Fedora Workstation 43" in output
    assert "https://example.com/fedora-workstation-43-very-long-uri-path.torrent" in output


def test_fetch_results_with_spinner() -> None:
    transport = httpx.MockTransport(lambda _: httpx.Response(200, text=SAMPLE_HTML))

    async def _run() -> list[ScrapedResult]:
        async with httpx.AsyncClient(transport=transport) as client:
            scraper = CustomTableScraper(
                base_url="https://tracker.example.org",
                client=client,
            )
            console = Console(file=StringIO(), force_terminal=False)
            return await fetch_results_with_spinner(scraper, "arch", console)

    results = asyncio.run(_run())
    assert len(results) == 3


def test_copy_to_clipboard_empty_and_unavailable() -> None:
    ok_empty, msg_empty = copy_to_clipboard("")
    assert ok_empty is False
    assert "empty" in msg_empty.lower()

    with patch("scraper_module.ui.shutil.which", return_value=None):
        ok_none, msg_none = copy_to_clipboard("https://example.com/file.torrent")
        assert ok_none is False
        assert "no working clipboard utility" in msg_none.lower()


def test_run_cli_interactive_flow_mocked() -> None:
    transport = httpx.MockTransport(lambda _: httpx.Response(200, text=SAMPLE_HTML))
    client = httpx.AsyncClient(transport=transport)
    scraper = CustomTableScraper(base_url="https://tracker.example.org", client=client)

    buffer = StringIO()
    console = Console(file=buffer, force_terminal=False, width=120)

    text_prompt = MagicMock()
    text_prompt.ask.return_value = "ubuntu"

    select_answers = iter([0, "print", "__exit__"])

    def fake_select(*_args: object, **_kwargs: object) -> MagicMock:
        mock_prompt = MagicMock()
        mock_prompt.ask.return_value = next(select_answers)
        return mock_prompt

    with (
        patch("scraper_module.ui.questionary.text", return_value=text_prompt),
        patch("scraper_module.ui.questionary.select", side_effect=fake_select),
    ):
        run_cli(scraper=scraper, console=console)

    asyncio.run(client.aclose())
    output = buffer.getvalue()
    assert "Ubuntu 24.04 LTS Desktop ISO" in output
    assert "https://tracker.example.org/torrents/ubuntu-24.04.torrent" in output


def test_main_parse_args() -> None:
    args = parse_args(["--query", "debian", "--base-url", "https://mirror.example.com"])
    assert args.query == "debian"
    assert args.base_url == "https://mirror.example.com"
