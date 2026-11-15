"""Configurable HTML table scraper engine tailored for Nyaa with Mock and Live HTTP modes."""

from __future__ import annotations

import hashlib
import re
import threading
import time
from dataclasses import dataclass, field
from urllib.parse import quote_plus, urljoin, urlsplit, urlunsplit

import httpx
from bs4 import BeautifulSoup, Tag

from .models import ScrapedItem
from .validator import validate_nyaa_links

_BTIH_RE = re.compile(r"xt=urn:btih:([A-Za-z0-9]{32,40})\b", re.IGNORECASE)
_QUERY_RES_SUFFIX_RE = re.compile(r"\s+\b(?:2160p|1080p|720p|480p|4k|hevc|x265|x264)\b.*$", re.IGNORECASE)


def extract_info_hash(uri: str) -> str:
    """Extract normalized lowercase BTIH info-hash from a magnet URI when present."""
    match = _BTIH_RE.search(uri or "")
    return match.group(1).lower() if match else ""


def deduplicate_items(items: list[ScrapedItem]) -> list[ScrapedItem]:
    """Deduplicate scraped items by BTIH info-hash (or primary link / title fallback)."""
    seen: set[str] = set()
    unique: list[ScrapedItem] = []
    for item in items:
        btih = extract_info_hash(item.magnet_link or item.resolved_link)
        key = btih or (item.magnet_link or item.torrent_link or item.view_url or item.title).strip().lower()
        if key and key in seen:
            continue
        if key:
            seen.add(key)
        unique.append(item)
    return unique

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
        cache_ttl: float = 90.0,
        max_retries: int = 2,
        retry_backoff: float = 0.25,
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
        self.cache_ttl = cache_ttl
        self.max_retries = max_retries
        self.retry_backoff = retry_backoff
        self._client = client
        self._shared_client: httpx.Client | None = None
        self._cache_lock = threading.Lock()
        self._cache: dict[tuple[str, str, int, bool], tuple[float, ScrapeResponse]] = {}
        self._inflight: dict[tuple[str, str, int, bool], threading.Event] = {}

    def clear_cache(self) -> None:
        """Clear the short-lived scrape response cache."""
        with self._cache_lock:
            self._cache.clear()

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

    def build_search_url(
        self,
        query: str = "",
        page: int = 1,
        category: str | None = None,
        filter_mode: str | None = None,
        sort_by: str | None = None,
        sort_order: str | None = None,
    ) -> str:
        """Construct the Nyaa browse or search URL with ``quote_plus`` encoding, category, filter, and optional ``&p=`` page."""
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
        eff_cat = (category or NYAA_CATEGORY).strip() or NYAA_CATEGORY
        eff_filter = (filter_mode or NYAA_FILTER).strip() or NYAA_FILTER
        eff_order = (sort_order or "desc").strip() or "desc"

        if not cleaned:
            eff_sort = (sort_by or "leechers").strip() or "leechers"
            encoded_qs = f"{param_name}=&f={eff_filter}&c={eff_cat}&s={eff_sort}&o={eff_order}"
        else:
            eff_sort = (sort_by or "seeders").strip() or "seeders"
            encoded_q = quote_plus(cleaned)
            encoded_qs = f"f={eff_filter}&c={eff_cat}&{param_name}={encoded_q}&s={eff_sort}&o={eff_order}"

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

    include_all_statuses: bool = False

    @staticmethod
    def derive_nyaa_title_fields(release_title: str) -> tuple[str, str, str]:
        """Derive ``(display_title, release_group, alternative_title)`` without mutating ``release_title``."""
        raw = (release_title or "").strip()
        if not raw:
            return ("Untitled", "", "")

        alternative_title = ""
        main_part = raw
        if "|" in raw:
            parts = [p.strip() for p in raw.split("|") if p.strip()]
            if len(parts) >= 2:
                main_part = parts[0]
                alternative_title = " | ".join(parts[1:])

        main_part = re.sub(r"\.(?:mkv|mp4|avi|cbz|zip|rar)$", "", main_part, flags=re.IGNORECASE).strip()

        release_group = ""
        leading_group = re.match(r"^\[([^\[\]]+)\]\s*(.+)$", main_part)
        if leading_group:
            release_group = leading_group.group(1).strip()
            main_part = leading_group.group(2).strip()

        main_part = re.sub(r"\[[0-9A-Fa-f]{8}\]", "", main_part).strip()

        for bracket_match in re.findall(r"\[([^\[\]]+)\]", main_part):
            candidate = bracket_match.strip()
            if not release_group and not re.search(
                r"\b(?:1080p|720p|2160p|480p|4k|bd|web|hevc|av1|x264|x265|h264|h265|aac|opus|flac|sub|subtitle|multi|dual|audio|batch)\b",
                candidate,
                re.IGNORECASE,
            ):
                release_group = candidate
        main_part = re.sub(r"\[[^\[\]]*\]", "", main_part).strip()

        def _keep_or_strip_paren(match: re.Match[str]) -> str:
            inner = match.group(1).strip()
            if re.fullmatch(r"(?:19|20)\d{2}", inner):
                return f"({inner})"
            return ""

        main_part = re.sub(r"\(([^()]*)\)", _keep_or_strip_paren, main_part).strip()
        main_part = re.sub(
            r"\s+(?:BD|BDRip|BluRay|WEB-DL|WEBRip|HDTV|2160p|1080p|720p|480p|4K|HEVC|AV1|x264|x265|H\.?264|H\.?265|AAC|Opus|FLAC|10bit|Dual[\s-]*Audio|Multi[\s-]*Audio)\b.*$",
            "",
            main_part,
            flags=re.IGNORECASE,
        ).strip()

        display_title = re.sub(r"\s{2,}", " ", main_part).strip(" -_/")
        return (display_title or raw, release_group, alternative_title)


    def parse_html(
        self,
        html: str,
        base_url: str | None = None,
        start_index: int = 1,
        include_all_statuses: bool | None = None,
    ) -> list[ScrapedItem]:
        """Parse Nyaa ``table.torrent-list`` rows into validated :class:`ScrapedItem` objects."""
        from urllib.parse import parse_qs, urlsplit

        soup = BeautifulSoup(html, "html.parser")
        effective_base = (base_url or self.base_url or BASE_URL).strip()
        selector = self.row_selector.strip() or ROW_SELECTOR
        all_statuses = (
            include_all_statuses
            if include_all_statuses is not None
            else getattr(self, "include_all_statuses", False)
        )

        status_selector = (
            "table.torrent-list tr.default, table.torrent-list tr.success, "
            "table.torrent-list tr.danger, table.torrent-list tr.warning"
        )
        if all_statuses:
            rows = [r for r in soup.select(status_selector) if r.find_parent("thead") is None]
        else:
            rows = [
                r
                for r in soup.select("table.torrent-list tr.default")
                if r.find_parent("thead") is None
            ]

        # Fallback for generic/synthetic tables that do not use Nyaa row status classes
        if not rows and not soup.select(status_selector):
            rows = [r for r in soup.select(selector) if r.find_parent("thead") is None]
            if not rows and selector != "table tbody tr":
                rows = [r for r in soup.select("table tbody tr") if r.find_parent("thead") is None]

        items: list[ScrapedItem] = []
        idx = start_index - 1

        for row in rows:
            if row.find_parent("thead") is not None:
                continue
            tds = row.find_all("td")
            if len(tds) < 8:
                continue

            # 1. Title & View Link (tds[1] / td[colspan="2"])
            title_tag = row.select_one('td[colspan="2"] a[href^="/view/"]:not(.comments)')
            if title_tag is None:
                title_tag = (
                    tds[1].select_one('a[href^="/view/"]:not(.comments)')
                    or tds[1].select_one("a:not(.comments)")
                )
            if title_tag is None:
                continue

            release_title = (
                self._attr_str(title_tag, "title")
                or title_tag.get_text(strip=True)
            )
            if not release_title:
                continue

            idx += 1
            display_title, release_group, alternative_title = self.derive_nyaa_title_fields(release_title)

            view_href = self._attr_str(title_tag, "href")
            view_url = urljoin(effective_base, view_href) if view_href else ""
            id_match = re.search(r"/view/(\d+)", view_href)
            torrent_id = id_match.group(1) if id_match else ""

            # 2. Category & Row Status (tds[0] & tr class)
            row_classes = row.get("class") or []
            if isinstance(row_classes, str):
                row_classes = row_classes.split()
            row_class = next(
                (c for c in row_classes if c in ("default", "success", "danger", "warning")),
                row_classes[0] if row_classes else "default",
            )

            cat_cell = tds[0]
            cat_anchor = cat_cell.select_one('a[href*="c="]') or cat_cell.select_one("a")
            cat_img = cat_cell.select_one("img.category-icon") or cat_cell.select_one("img")
            cat_href = self._attr_str(cat_anchor, "href")
            cat_code_match = re.search(r"[?&]c=([0-9]+_[0-9]+)", cat_href)
            if not cat_code_match:
                cat_src = self._attr_str(cat_img, "src")
                cat_code_match = re.search(r"/([0-9]+_[0-9]+)\.[a-zA-Z0-9]+$", cat_src)
            category_code = cat_code_match.group(1) if cat_code_match else ""
            category_name = self._attr_str(cat_anchor, "title") or self._attr_str(cat_img, "alt")

            # 3. Magnet & .torrent Links (tds[2] / td.text-center)
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

            raw_hash = extract_info_hash(magnet_link) if magnet_link else ""
            info_hash = raw_hash.lower() if len(raw_hash) == 40 else raw_hash
            trackers = parse_qs(urlsplit(magnet_link).query).get("tr", []) if magnet_link else []
            magnet_tracker_count = len(trackers)

            # 4. Size, Date, Timestamp & Peer Metrics (tds[3]..tds[7])
            size = tds[3].get_text(strip=True) or "N/A"
            date = tds[4].get_text(strip=True) or "N/A"
            relative_date = self._attr_str(tds[4], "title")
            raw_timestamp = self._attr_str(tds[4], "data-timestamp")
            timestamp = raw_timestamp if raw_timestamp.isdigit() else ""
            seeders = self._parse_int(tds[5].get_text(strip=True))
            leechers = self._parse_int(tds[6].get_text(strip=True))
            downloads = self._parse_int(tds[7].get_text(strip=True))

            # 5. Link verification (valid if Magnet BTIH OR Nyaa .torrent URL passes)
            is_valid, verification_msg = validate_nyaa_links(magnet_link, torrent_link)
            primary_link = magnet_link or torrent_link
            stats_summary = f"S: {seeders:,} / L: {leechers:,} / D: {downloads:,}"

            items.append(
                ScrapedItem(
                    id=idx,
                    title=release_title,
                    release_title=release_title,
                    display_title=display_title,
                    alternative_title=alternative_title,
                    release_group=release_group,
                    torrent_id=torrent_id,
                    row_class=row_class,
                    category_code=category_code,
                    category_name=category_name,
                    size=size,
                    file_size=size,
                    date=date,
                    upload_date=date,
                    timestamp=timestamp,
                    stats=stats_summary,
                    raw_link=primary_link,
                    resolved_link=primary_link,
                    is_valid=is_valid,
                    verification_msg=verification_msg,
                    view_url=view_url,
                    details_url=view_url,
                    magnet_link=magnet_link,
                    magnet_uri=magnet_link,
                    torrent_link=torrent_link,
                    torrent_url=torrent_link,
                    info_hash=info_hash,
                    magnet_tracker_count=magnet_tracker_count,
                    trackers=trackers,
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
        raw_query = (query or "").strip()
        if "nonexistent" in raw_query.lower():
            return []
        topic = _QUERY_RES_SUFFIX_RE.sub("", raw_query).strip() or raw_query
        base = self.base_url.strip().rstrip("/") or BASE_URL
        page_num = max(1, min(5, int(page or 1)))
        count = max(1, int(per_page or 24))
        start_offset = (page_num - 1) * count

        browse_shows = [
            "Frieren: Beyond Journey's End",
            "Dan Da Dan",
            "Delicious in Dungeon",
            "Solo Leveling",
            "The Apothecary Diaries",
            "Orb: On the Movements of the Earth",
        ]

        specs: list[tuple[str, str, str, str, bool]] = []
        topic_lower = topic.lower()
        is_your_name_movie = any(k in topic_lower for k in ("your name", "kimi no na wa", "君の名は"))
        if is_your_name_movie:
            movie_templates: list[tuple[str, str, str, str, bool]] = [
                (
                    "[PHTMini] Your Name. (2016) (BD 1080p AV1 Opus 5.1) (Multi-Audio) (German/Deutsch) | Kimi no Na wa.",
                    "2.1 GiB",
                    "your-name-2016-phtmini-av1",
                    "mkv",
                    True,
                ),
                (
                    "[Coalgirls] Your Name. (2016) (1920x1080 Blu-ray FLAC) [B2E8A19C].mkv",
                    "8.4 GiB",
                    "your-name-2016-coalgirls-1080p",
                    "mkv",
                    True,
                ),
                (
                    "[Judas] Your Name. (2016) [2160p 4K UHD HEVC x265 10bit Dual-Audio] | Kimi no Na wa.mkv",
                    "6.2 GiB",
                    "your-name-2016-judas-4k",
                    "mkv",
                    True,
                ),
                (
                    "[MTBB] Kimi no Na wa. (2016) [BD 1080p HEVC 10bit Dual-Audio].mkv",
                    "3.8 GiB",
                    "your-name-2016-mtbb-1080p",
                    "mkv",
                    True,
                ),
                (
                    "[Kametsu] Your Name. (2016) [BD 720p AAC] | Kimi no Na wa.mkv",
                    "1.5 GiB",
                    "your-name-2016-kametsu-720p",
                    "mkv",
                    True,
                ),
            ]
            if page_num == 1:
                specs.extend(movie_templates[: min(count, len(movie_templates))])
                count = len(specs)
            else:
                return []
        elif not topic:
            # Default browse: 6 distinct anime shows across multiple episodes, seasons, and batches
            for idx in range(count):
                show_name = browse_shows[idx % len(browse_shows)]
                ep_num = ((page_num - 1) * 4) + (idx // len(browse_shows)) + 1
                if idx % 6 == 5:
                    batch_variants = [
                        (f"[Judas] {show_name} Season 1 Batch [1080p HEVC x265 10bit].mkv", "6.8 GiB", "s1-batch-1080p"),
                        (f"[SubsPlease] {show_name} - 01-12 [1080p][Batch].mkv", "8.2 GiB", "s1-range-01-12"),
                        (f"[SubsPlease] {show_name} - 13-25 [1080p][Batch].mkv", "8.9 GiB", "s1-range-13-25"),
                        (f"[EMBER] {show_name} Complete Series [1080p Dual-Audio].mkv", "16.4 GiB", "complete-series"),
                    ]
                    b_title, b_size, b_tag = batch_variants[(idx // len(browse_shows)) % len(batch_variants)]
                    specs.append(
                        (
                            b_title,
                            b_size,
                            f"{show_name}-{b_tag}-p{page_num}",
                            "mkv",
                            True,
                        )
                    )
                elif idx % 6 == 4:
                    specs.append(
                        (
                            f"[SubsPlease] {show_name} S02E{ep_num:02d} (1080p) [HEVC].mkv",
                            "1.4 GiB",
                            f"{show_name}-s2e{ep_num:02d}",
                            "mkv",
                            True,
                        )
                    )
                elif idx % 2 == 0:
                    specs.append(
                        (
                            f"[SubsPlease] {show_name} - {ep_num:02d} (1080p) [HEVC].mkv",
                            "1.4 GiB",
                            f"{show_name}-e{ep_num:02d}-1080p",
                            "mkv",
                            True,
                        )
                    )
                else:
                    specs.append(
                        (
                            f"[Erai-raws] {show_name} - {ep_num:02d} [720p][Multiple Subtitle].mkv",
                            "680.5 MiB",
                            f"{show_name}-e{ep_num:02d}-720p",
                            "mkv",
                            True,
                        )
                    )
        elif page_num == 1:
            # Stage A / Page 1 for a specific show: rich multi-resolution episodes, Season 1 & Season 2,
            # three distinct batch kinds, one .torrent-only release, and one unclassified special.
            page1_templates: list[tuple[str, str, str, str, bool]] = [
                (f"[SubsPlease] {topic} - 01 (1080p) [HEVC].mkv", "1.4 GiB", "s1e01-1080p", "mkv", True),
                (f"[Erai-raws] {topic} - 01 [720p][AVC].mkv", "640.2 MiB", "s1e01-720p", "mkv", True),
                (f"[VARYG] {topic} - 01 [2160p 4K WEB-DL Dual-Audio].mkv", "3.2 GiB", "s1e01-2160p", "mkv", True),
                (f"[SubsPlease] {topic} - 02 (1080p) [HEVC].mkv", "1.4 GiB", "s1e02-1080p", "mkv", True),
                (f"[EMBER] {topic} - 02 [720p HEVC].mkv", "395.0 MiB", "s1e02-720p", "mkv", True),
                (f"[Toonshub] {topic} - 02 [1080p WEB-DL AAC].mp4", "1.1 GiB", "s1e02-torrent-only", "mp4", False),
                (f"[SubsPlease] {topic} - 03 (1080p) [HEVC].mkv", "1.4 GiB", "s1e03-1080p", "mkv", True),
                (f"[Judas] {topic} - 03 [1080p HEVC x265 10bit].mkv", "580.4 MiB", "s1e03-hevc", "mkv", True),
                (f"[SubsPlease] {topic} - 04 (1080p) [HEVC].mkv", "1.4 GiB", "s1e04-1080p", "mkv", True),
                (f"[Erai-raws] {topic} - 04 [720p][AVC].mkv", "650.0 MiB", "s1e04-720p", "mkv", True),
                (f"[SubsPlease] {topic} - 05 (1080p) [HEVC].mkv", "1.3 GiB", "s1e05-1080p", "mkv", True),
                (f"[ASW] {topic} - 05 [1080p HEVC Dual-Audio].mkv", "490.0 MiB", "s1e05-asw", "mkv", True),
                (f"[SubsPlease] {topic} - 06 (1080p) [HEVC].mkv", "1.4 GiB", "s1e06-1080p", "mkv", True),
                (f"[SubsPlease] {topic} - 07 (1080p) [HEVC].mkv", "1.4 GiB", "s1e07-1080p", "mkv", True),
                (f"[SubsPlease] {topic} - 08 (1080p) [HEVC].mkv", "1.5 GiB", "s1e08-1080p", "mkv", True),
                (f"[SubsPlease] {topic} S02E01 (1080p) [HEVC].mkv", "1.5 GiB", "s2e01-1080p", "mkv", True),
                (f"[Erai-raws] {topic} S02E01 [720p][AVC].mkv", "670.0 MiB", "s2e01-720p", "mkv", True),
                (f"[SubsPlease] {topic} Season 2 Episode 2 [1080p].mkv", "1.4 GiB", "s2e02-1080p", "mkv", True),
                (f"[Judas] {topic} S02E03 [1080p HEVC x265 10bit].mkv", "610.0 MiB", "s2e03-1080p", "mkv", True),
                (f"[SubsPlease] {topic} Season 2 - 04 [1080p].mkv", "1.4 GiB", "s2e04-1080p", "mkv", True),
                (f"[SubsPlease] {topic} - 01-12 [1080p][Batch].mkv", "14.8 GiB", "s1-range-01-12", "mkv", True),
                (f"[Judas] {topic} Season 1 Batch [1080p HEVC x265 10bit].mkv", "7.4 GiB", "s1-full-batch", "mkv", True),
                (f"[EMBER] {topic} Complete Series [1080p Dual-Audio].mkv", "28.6 GiB", "complete-series-pack", "mkv", True),
                (f"[SubsPlease] {topic} - Special Original Soundtrack & PV [1080p].mkv", "890.0 MiB", "special-ost-pv", "mkv", True),
            ]
            for idx in range(count):
                specs.append(page1_templates[idx % len(page1_templates)])
        else:
            # Pages 2..5: progressive later episodes across Season 1 (09..24) and Season 2 (05..16)
            groups = [
                ("SubsPlease", "1080p", "1.4 GiB", "mkv"),
                ("Erai-raws", "1080p Multiple Subtitle", "1.6 GiB", "mkv"),
                ("Judas", "1080p HEVC x265 10bit", "640.2 MiB", "mkv"),
                ("EMBER", "720p HEVC", "390.5 MiB", "mkv"),
                ("VARYG", "2160p 4K WEB-DL Dual-Audio", "3.2 GiB", "mkv"),
                ("ASW", "1080p HEVC Dual-Audio", "485.0 MiB", "mkv"),
            ]
            base_ep_s1 = 8 + (page_num - 2) * 4
            base_ep_s2 = 4 + (page_num - 2) * 3
            for offset in range(count):
                group_name, profile, size_label, ext = groups[offset % len(groups)]
                if offset == count - 2:
                    specs.append(
                        (
                            f"[{group_name}] {topic} Season {min(3, page_num)} Batch [{profile}].{ext}",
                            "8.4 GiB",
                            f"s{min(3, page_num)}-batch-p{page_num}",
                            ext,
                            True,
                        )
                    )
                elif offset % 3 == 2:
                    s2_ep = base_ep_s2 + (offset // 6) + 1
                    specs.append(
                        (
                            f"[{group_name}] {topic} S02E{s2_ep:02d} [{profile}].{ext}",
                            size_label,
                            f"s2e{s2_ep:02d}-p{page_num}-{offset}",
                            ext,
                            True,
                        )
                    )
                else:
                    s1_ep = min(28, base_ep_s1 + (offset // 3) + 1)
                    specs.append(
                        (
                            f"[{group_name}] {topic} - {s1_ep:02d} [{profile}].{ext}",
                            size_label,
                            f"s1e{s1_ep:02d}-p{page_num}-{offset}",
                            ext,
                            True,
                        )
                    )

        slug = "-".join((topic or "nyaa-browse").lower().split())
        items: list[ScrapedItem] = []

        for offset in range(count):
            global_idx = start_offset + offset + 1
            torrent_id = 1894000 + (page_num * 100) + offset + 1
            title, size_label, tag_slug, _ext, has_magnet = specs[offset]

            seeders = max(6, 1650 - global_idx * 13 + ((offset % 4) * 45))
            leechers = max(1, int(seeders * 0.08) + (offset % 9))
            downloads = seeders * 18 + (offset * 120)
            day = max(1, 14 - ((global_idx - 1) // 8))
            date = f"2026-11-{day:02d} {(23 - (offset % 20)):02d}:{(offset * 7) % 60:02d}"
            relative_date = (
                f"{global_idx} hour(s) ago"
                if global_idx < 24
                else f"{(global_idx // 24) + 1} day(s) ago"
            )

            btih_hex = hashlib.sha1(title.encode("utf-8")).hexdigest()
            magnet_link = (
                f"magnet:?xt=urn:btih:{btih_hex}&dn={quote_plus(slug + '-' + tag_slug)}"
                if has_magnet
                else ""
            )
            torrent_id = 1800000 + (int(btih_hex[:6], 16) % 900000)
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
        force_refresh: bool = False,
        category: str | None = None,
        filter_mode: str | None = None,
        sort_by: str | None = None,
        sort_order: str | None = None,
    ) -> ScrapeResponse:
        """Execute a Nyaa browse (empty query) or search for ``page`` with short-lived caching, in-flight deduplication, and bounded retry."""
        cleaned_query = (query or "").strip()
        page_num = max(1, int(page or 1))
        use_demo = self.demo_mode if demo_mode is None else demo_mode
        eff_cat = (category or NYAA_CATEGORY).strip() or NYAA_CATEGORY
        eff_filter = (filter_mode or NYAA_FILTER).strip() or NYAA_FILTER
        eff_sort = (sort_by or ("seeders" if cleaned_query else "leechers")).strip()
        eff_order = (sort_order or "desc").strip()
        cache_key = (
            f"{self.base_url.strip().rstrip('/')}:{eff_cat}:{eff_filter}:{eff_sort}:{eff_order}",
            cleaned_query.lower(),
            page_num,
            bool(use_demo),
        )

        if not force_refresh and self.cache_ttl > 0:
            with self._cache_lock:
                cached = self._cache.get(cache_key)
                if cached is not None and cached[0] > time.monotonic():
                    return cached[1]

        # Prevent duplicate concurrent requests for the same (base_url, query, page, mode)
        wait_event: threading.Event | None = None
        own_event: threading.Event | None = None
        with self._cache_lock:
            if not force_refresh and cache_key in self._inflight:
                wait_event = self._inflight[cache_key]
            else:
                own_event = threading.Event()
                self._inflight[cache_key] = own_event

        if wait_event is not None:
            wait_event.wait(timeout=max(1.0, self.timeout * 2))
            with self._cache_lock:
                cached = self._cache.get(cache_key)
                if cached is not None:
                    return cached[1]

        try:
            result = self._execute_search_uncached(
                cleaned_query,
                use_demo,
                page_num,
                category=eff_cat,
                filter_mode=eff_filter,
                sort_by=sort_by,
                sort_order=sort_order,
            )
            if result.error is None and self.cache_ttl > 0:
                with self._cache_lock:
                    self._cache[cache_key] = (time.monotonic() + self.cache_ttl, result)
            return result
        finally:
            if own_event is not None:
                with self._cache_lock:
                    if self._inflight.get(cache_key) is own_event:
                        del self._inflight[cache_key]
                    own_event.set()

    def _execute_search_uncached(
        self,
        cleaned_query: str,
        use_demo: bool,
        page_num: int,
        category: str | None = None,
        filter_mode: str | None = None,
        sort_by: str | None = None,
        sort_order: str | None = None,
    ) -> ScrapeResponse:
        eff_cat = (category or NYAA_CATEGORY).strip() or NYAA_CATEGORY
        if use_demo:
            if self.mock_delay > 0:
                time.sleep(self.mock_delay)
            mock_items = deduplicate_items(self.get_mock_items(cleaned_query, page=page_num))
            return ScrapeResponse(
                items=mock_items,
                error=None,
                request_url=f"mock://nyaa-category-{eff_cat}?p={page_num}",
                is_demo=True,
                page=page_num,
                has_next_page=page_num < 5,
                total_pages=5,
            )

        if not self.row_selector.strip():
            return ScrapeResponse(items=[], error="ROW_SELECTOR cannot be empty.", page=page_num)

        try:
            target_url = self.build_search_url(
                cleaned_query,
                page=page_num,
                category=category,
                filter_mode=filter_mode,
                sort_by=sort_by,
                sort_order=sort_order,
            )
        except ValueError as exc:
            return ScrapeResponse(items=[], error=f"Invalid URL configuration: {exc}", page=page_num)

        headers = {"User-Agent": self.user_agent}
        attempts = max(1, int(self.max_retries) + 1)

        for attempt in range(attempts):
            try:
                http_client = self._get_http_client()
                response = http_client.get(
                    target_url,
                    headers=headers,
                    timeout=self.timeout,
                    follow_redirects=True,
                )
                if response.status_code in {429, 502, 503, 504} and attempt + 1 < attempts:
                    time.sleep(self.retry_backoff * (2**attempt))
                    continue
                response.raise_for_status()
                items = deduplicate_items(self.parse_html(response.text, base_url=str(response.url)))
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
            except (httpx.TimeoutException, httpx.ConnectError, httpx.ReadError) as exc:
                if attempt + 1 < attempts:
                    time.sleep(self.retry_backoff * (2**attempt))
                    continue
                if isinstance(exc, httpx.TimeoutException):
                    return ScrapeResponse(
                        items=[],
                        error=f"Request timed out after {self.timeout:.1f}s ({target_url}).",
                        request_url=target_url,
                        page=page_num,
                    )
                return ScrapeResponse(
                    items=[],
                    error=f"Network error while fetching {target_url}: {exc}",
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
        return ScrapeResponse(items=[], error=f"Request failed for {target_url}.", request_url=target_url, page=page_num)
