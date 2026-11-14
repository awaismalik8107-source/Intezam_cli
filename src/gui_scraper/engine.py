"""Configurable HTML table scraper engine tailored for Nyaa with Mock and Live HTTP modes."""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from urllib.parse import quote_plus, urljoin, urlsplit, urlunsplit

import httpx
from bs4 import BeautifulSoup, Tag

from .models import ScrapedItem
from .validator import validate_nyaa_links

# ---------------------------------------------------------------------------
# Nyaa URL & Exact Table Selector Constants
# ---------------------------------------------------------------------------
PROVIDER_NAME = "Nyaa"
BASE_URL = "https://nyaa.si"
SEARCH_PATH = "/"
QUERY_PARAM = "q"
NYAA_FILTER = "0"
NYAA_CATEGORY = "1_2"

ROW_SELECTOR = "table.torrent-list tbody tr"
TITLE_SELECTOR = 'td[colspan="2"] a[href^="/view/"]:not(.comments)'
LINK_SELECTOR = 'td.text-center a[href^="magnet:?"]'
TORRENT_SELECTOR = 'td.text-center a[href^="/download/"]'
SIZE_SELECTOR = "td:nth-of-type(4)"
DATE_SELECTOR = "td:nth-of-type(5)"
STATS_SELECTOR = "td:nth-of-type(6)"

DEFAULT_TIMEOUT = 10.0
DEFAULT_USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
)


@dataclass(slots=True)
class ScrapeResponse:
    """Structured result returned to the GUI by :class:`ConfigurableTableScraper`."""

    items: list[ScrapedItem] = field(default_factory=list)
    error: str | None = None
    request_url: str = ""
    is_demo: bool = False
    page: int = 1
    has_next_page: bool = False
    total_pages: int = 1


class ConfigurableTableScraper:
    """HTML table scraper supporting Nyaa browse/search endpoints, pagination, and connection pooling."""

    def __init__(
        self,
        provider_name: str = PROVIDER_NAME,
        base_url: str = BASE_URL,
        search_path: str = SEARCH_PATH,
        query_param: str = QUERY_PARAM,
        row_selector: str = ROW_SELECTOR,
        title_selector: str = TITLE_SELECTOR,
        link_selector: str = LINK_SELECTOR,
        size_selector: str = SIZE_SELECTOR,
        date_selector: str = DATE_SELECTOR,
        stats_selector: str = STATS_SELECTOR,
        timeout: float = DEFAULT_TIMEOUT,
        user_agent: str = DEFAULT_USER_AGENT,
        demo_mode: bool = False,
        mock_delay: float = 0.0,
        client: httpx.Client | None = None,
    ) -> None:
        self.provider_name = provider_name
        self.base_url = base_url
        self.search_path = search_path
        self.query_param = query_param
        self.row_selector = row_selector
        self.title_selector = title_selector
        self.link_selector = link_selector
        self.size_selector = size_selector
        self.date_selector = date_selector
        self.stats_selector = stats_selector
        self.timeout = timeout
        self.user_agent = user_agent
        self.demo_mode = demo_mode
        self.mock_delay = mock_delay
        self._client = client
        self._shared_client: httpx.Client | None = None

    def _get_http_client(self) -> httpx.Client:
        if self._client is not None:
            return self._client
        if self._shared_client is None or self._shared_client.is_closed:
            self._shared_client = httpx.Client(
                timeout=self.timeout,
                follow_redirects=True,
                limits=httpx.Limits(max_keepalive_connections=8, max_connections=16),
            )
        return self._shared_client

    def close(self) -> None:
        """Close the internal pooled HTTP client if initialized."""
        if self._shared_client is not None and not self._shared_client.is_closed:
            self._shared_client.close()
            self._shared_client = None

    def build_search_url(self, query: str = "", page: int = 1) -> str:
        """Construct the Nyaa browse or search URL with ``quote_plus`` encoding and optional ``&p=`` page."""
        base = self.base_url.strip().rstrip("/") or BASE_URL
        path = self.search_path.strip() or "/"
        joined = urljoin(base + "/", path.lstrip("/")) if base else path
        parts = urlsplit(joined)
        if not parts.scheme or not parts.netloc:
            raise ValueError("BASE_URL must include scheme and host.")

        cleaned = (query or "").strip()
        param_name = self.query_param.strip() or QUERY_PARAM
        normalized_path = parts.path or "/"
        page_num = max(1, int(page or 1))

        if not cleaned:
            encoded_qs = f"{param_name}=&f={NYAA_FILTER}&c={NYAA_CATEGORY}&s=leechers&o=desc"
        else:
            encoded_q = quote_plus(cleaned)
            encoded_qs = f"f={NYAA_FILTER}&c={NYAA_CATEGORY}&{param_name}={encoded_q}&s=seeders&o=desc"

        if page_num > 1:
            encoded_qs = f"{encoded_qs}&p={page_num}"

        return urlunsplit((parts.scheme, parts.netloc, normalized_path, encoded_qs, ""))

    @staticmethod
    def _attr_str(tag: Tag | None, attr: str) -> str:
        if tag is None:
            return ""
        raw = tag.get(attr)
        if isinstance(raw, list):
            return str(raw[0]).strip() if raw else ""
        return str(raw).strip() if raw is not None else ""

    @staticmethod
    def _parse_int(value: str) -> int:
        cleaned = "".join(ch for ch in (value or "") if ch.isdigit())
        return int(cleaned) if cleaned else 0

    @classmethod
    def _find_anchor_with_icon(cls, cell: Tag, icon_class: str) -> Tag | None:
        for anchor in cell.find_all("a"):
            if anchor.select_one(f"i.{icon_class}") is not None:
                return anchor
        return None

    @staticmethod
    def parse_pagination(html: str, current_page: int = 1) -> tuple[bool, int]:
        """Inspect Nyaa ``ul.pagination`` markup for next-page availability and total pages."""
        soup = BeautifulSoup(html, "html.parser")
        pager = soup.select_one("ul.pagination")
        if pager is None:
            return (False, max(1, current_page))

        page_numbers: list[int] = [max(1, current_page)]
        for link in pager.select("a"):
            text = link.get_text(strip=True)
            if text.isdigit():
                page_numbers.append(int(text))

        next_disabled = pager.select_one("li.next.disabled") is not None
        next_link = pager.select_one('li.next:not(.disabled) a, a[rel="next"]')
        max_page = max(page_numbers) if page_numbers else max(1, current_page)
        has_next = (next_link is not None and not next_disabled) or (current_page < max_page)
        return (has_next, max_page)

    def parse_html(
        self,
        html: str,
        base_url: str | None = None,
        start_index: int = 1,
    ) -> list[ScrapedItem]:
        """Parse Nyaa ``table.torrent-list tbody tr`` rows into validated :class:`ScrapedItem` objects."""
        soup = BeautifulSoup(html, "html.parser")
        effective_base = (base_url or self.base_url or BASE_URL).strip()
        selector = self.row_selector.strip() or ROW_SELECTOR
        rows = soup.select(selector)
        if not rows and selector != "table tbody tr":
            rows = soup.select("table tbody tr")

        items: list[ScrapedItem] = []
        idx = start_index - 1

        for row in rows:
            tds = row.find_all("td")
            if len(tds) < 8:
                continue

            idx += 1

            # 1. Title & View Link (tds[1] / td[colspan="2"])
            title_tag = row.select_one('td[colspan="2"] a[href^="/view/"]:not(.comments)')
            if title_tag is None:
                title_tag = tds[1].select_one('a[href^="/view/"]:not(.comments)') or tds[1].select_one("a")
            title = (
                self._attr_str(title_tag, "title")
                or (title_tag.get_text(strip=True) if title_tag is not None else "")
                or "Untitled"
            )
            view_href = self._attr_str(title_tag, "href")
            view_url = urljoin(effective_base, view_href) if view_href else ""

            # 2. Magnet & .torrent Links (tds[2] / td.text-center)
            links_cell = tds[2]
            magnet_tag = links_cell.select_one('a[href^="magnet:?"]') or self._find_anchor_with_icon(
                links_cell, "fa-magnet"
            )
            magnet_link = self._attr_str(magnet_tag, "href")

            torrent_tag = links_cell.select_one('a[href^="/download/"]') or self._find_anchor_with_icon(
                links_cell, "fa-download"
            )
            torrent_href = self._attr_str(torrent_tag, "href")
            torrent_link = urljoin(effective_base, torrent_href) if torrent_href else ""

            # 3. Size, Date & Peer Metrics (tds[3]..tds[7])
            size = tds[3].get_text(strip=True) or "N/A"
            date = tds[4].get_text(strip=True) or "N/A"
            relative_date = self._attr_str(tds[4], "title")
            seeders = self._parse_int(tds[5].get_text(strip=True))
            leechers = self._parse_int(tds[6].get_text(strip=True))
            downloads = self._parse_int(tds[7].get_text(strip=True))

            # 4. Link verification (valid if Magnet BTIH OR Nyaa .torrent URL passes)
            is_valid, verification_msg = validate_nyaa_links(magnet_link, torrent_link)
            primary_link = magnet_link or torrent_link
            stats_summary = f"S: {seeders:,} / L: {leechers:,} / D: {downloads:,}"

            items.append(
                ScrapedItem(
                    id=idx,
                    title=title,
                    size=size,
                    date=date,
                    stats=stats_summary,
                    raw_link=primary_link,
                    resolved_link=primary_link,
                    is_valid=is_valid,
                    verification_msg=verification_msg,
                    view_url=view_url,
                    magnet_link=magnet_link,
                    torrent_link=torrent_link,
                    relative_date=relative_date,
                    seeders=seeders,
                    leechers=leechers,
                    downloads=downloads,
                )
            )

        return items

    def get_mock_items(
        self,
        query: str = "",
        page: int = 1,
        per_page: int = 24,
    ) -> list[ScrapedItem]:
        """Generate multi-page Nyaa-structured rows (24 links/page across 5 pages) for previews."""
        topic = query.strip() or "Anime Browse (Category 1_2)"
        slug = "-".join(topic.lower().split()) or "nyaa-sample"
        base = self.base_url.strip().rstrip("/") or BASE_URL
        page_num = max(1, min(5, int(page or 1)))
        count = max(1, int(per_page or 24))

        groups = [
            ("SubsPlease", "1080p", "1.4 GiB", "MKV"),
            ("Erai-raws", "1080p Multiple Subtitle", "1.6 GiB", "MKV"),
            ("Judas", "1080p HEVC x265 10bit", "640.2 MiB", "MKV"),
            ("EMBER", "720p HEVC", "390.5 MiB", "MKV"),
            ("VARYG", "2160p 4K WEB-DL Dual-Audio", "3.2 GiB", "MKV"),
            ("ASW", "1080p HEVC Dual-Audio", "485.0 MiB", "MKV"),
            ("DKB", "1080p HEVC 10bit", "510.8 MiB", "MKV"),
            ("Toonshub", "1080p WEB-DL AAC", "1.1 GiB", "MP4"),
        ]

        items: list[ScrapedItem] = []
        start_offset = (page_num - 1) * count

        for offset in range(count):
            global_idx = start_offset + offset + 1
            group_name, profile, size, ext = groups[offset % len(groups)]
            episode_num = ((global_idx - 1) % 24) + 1
            torrent_id = 1894000 + (page_num * 100) + offset + 1
            is_batch = offset % 7 == 6

            if is_batch:
                title = f"[{group_name}] {topic} (Batch 01-24) [{profile}].{ext.lower()}"
                size_label = f"{4.2 + (offset % 5) * 1.3:.1f} GiB"
            else:
                title = f"[{group_name}] {topic} - {episode_num:02d} [{profile}] [{torrent_id:08X}].{ext.lower()}"
                size_label = size

            seeders = max(6, 1650 - global_idx * 13 + ((offset % 4) * 45))
            leechers = max(1, int(seeders * 0.08) + (offset % 9))
            downloads = seeders * 18 + (offset * 120)
            day = max(1, 14 - ((global_idx - 1) // 8))
            date = f"2026-11-{day:02d} {(23 - (offset % 20)):02d}:{(offset * 7) % 60:02d}"
            relative_date = f"{global_idx} hour(s) ago" if global_idx < 24 else f"{(global_idx // 24) + 1} day(s) ago"

            btih_hex = f"{torrent_id:08x}" * 5
            magnet_link = f"magnet:?xt=urn:btih:{btih_hex}&dn={quote_plus(slug + f'-p{page_num}-{offset + 1}')}"
            torrent_link = f"{base}/download/{torrent_id}.torrent"
            view_url = f"{base}/view/{torrent_id}"

            # Keep last item of page 5 as a validation edge case; all other items valid
            if page_num == 5 and offset == count - 1:
                magnet_link = "magnet:?xt=urn:btih:SHORT_BAD_HASH"
                torrent_link = ""

            is_valid, verification_msg = validate_nyaa_links(magnet_link, torrent_link)
            primary_link = magnet_link or torrent_link
            items.append(
                ScrapedItem(
                    id=global_idx,
                    title=title,
                    size=size_label,
                    date=date,
                    stats=f"S: {seeders:,} / L: {leechers:,} / D: {downloads:,}",
                    raw_link=primary_link,
                    resolved_link=primary_link,
                    is_valid=is_valid,
                    verification_msg=verification_msg,
                    view_url=view_url,
                    magnet_link=magnet_link,
                    torrent_link=torrent_link,
                    relative_date=relative_date,
                    seeders=seeders,
                    leechers=leechers,
                    downloads=downloads,
                )
            )
        return items

    def search(
        self,
        query: str = "",
        demo_mode: bool | None = None,
        page: int = 1,
    ) -> ScrapeResponse:
        """Execute a Nyaa browse (empty query) or search for ``page`` in Mock or Live HTTP mode."""
        cleaned_query = (query or "").strip()
        page_num = max(1, int(page or 1))

        use_demo = self.demo_mode if demo_mode is None else demo_mode
        if use_demo:
            if self.mock_delay > 0:
                time.sleep(self.mock_delay)
            mock_items = self.get_mock_items(cleaned_query, page=page_num)
            return ScrapeResponse(
                items=mock_items,
                error=None,
                request_url=f"mock://nyaa-category-1_2?p={page_num}",
                is_demo=True,
                page=page_num,
                has_next_page=page_num < 5,
                total_pages=5,
            )

        if not self.row_selector.strip():
            return ScrapeResponse(items=[], error="ROW_SELECTOR cannot be empty.", page=page_num)

        try:
            target_url = self.build_search_url(cleaned_query, page=page_num)
        except ValueError as exc:
            return ScrapeResponse(items=[], error=f"Invalid URL configuration: {exc}", page=page_num)

        headers = {"User-Agent": self.user_agent}

        try:
            http_client = self._get_http_client()
            response = http_client.get(
                target_url,
                headers=headers,
                timeout=self.timeout,
                follow_redirects=True,
            )
            response.raise_for_status()
            items = self.parse_html(response.text, base_url=str(response.url))
            has_next, total_pages = self.parse_pagination(response.text, current_page=page_num)
            return ScrapeResponse(
                items=items,
                error=None,
                request_url=str(response.url),
                is_demo=False,
                page=page_num,
                has_next_page=has_next,
                total_pages=total_pages,
            )
        except httpx.TimeoutException:
            return ScrapeResponse(
                items=[],
                error=f"Request timed out after {self.timeout:.1f}s ({target_url}).",
                request_url=target_url,
                page=page_num,
            )
        except httpx.HTTPStatusError as exc:
            return ScrapeResponse(
                items=[],
                error=f"HTTP {exc.response.status_code} {exc.response.reason_phrase} from {exc.request.url}",
                request_url=str(exc.request.url),
                page=page_num,
            )
        except httpx.HTTPError as exc:
            return ScrapeResponse(
                items=[],
                error=f"Network error while fetching {target_url}: {exc}",
                request_url=target_url,
                page=page_num,
            )
        except Exception as exc:
            return ScrapeResponse(
                items=[],
                error=f"Failed to parse HTML with current selectors: {exc}",
                request_url=target_url,
                page=page_num,
            )
