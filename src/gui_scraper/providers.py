"""Extensible SourceProvider architecture (Nyaa, SubsPlease, EZTV) and multi-provider discovery orchestrator."""

from __future__ import annotations

from abc import ABC, abstractmethod
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
import re
import threading
import time
from typing import Any
from urllib.parse import quote_plus

import httpx

from .engine import ConfigurableTableScraper, extract_info_hash
from .matching import (
    TitleIdentityMatcher,
    compact_alnum_key,
    infer_technical_metadata,
    match_release_to_identity,
    normalize_title_tokens,
    parse_release_title,
)
from .models import (
    MediaIdentity,
    ProviderRetrievalStatus,
    ReleaseMatchResult,
    ScrapedItem,
    SourceSearchFilters,
    TorrentRelease,
)

NYAA_CATEGORY_MAP: dict[str, str] = {
    "all": "0_0",
    "anime_all": "1_0",
    "anime_eng": "1_2",
    "anime_non_eng": "1_3",
    "anime_raw": "1_4",
    "live_action": "4_0",
    "live_action_eng": "4_1",
    "live_action_raw": "4_3",
    "tv": "0_0",
    # Direct Nyaa category codes
    "0_0": "0_0",
    "1_0": "1_0",
    "1_2": "1_2",
    "1_3": "1_3",
    "1_4": "1_4",
    "4_0": "4_0",
    "4_1": "4_1",
    "4_3": "4_3",
}


def parse_size_to_bytes(size_str: str) -> int:
    """Parse human-readable size strings ('1.4 GiB', '680 MiB', '450 MB') into bytes."""
    m = re.match(
        r"^\s*(\d+(?:\.\d+)?)\s*(B|KB|KiB|MB|MiB|GB|GiB|TB|TiB)?\s*$",
        (size_str or "").strip(),
        re.IGNORECASE,
    )
    if not m:
        return 0
    val = float(m.group(1))
    unit = (m.group(2) or "b").lower()
    mult = {
        "b": 1,
        "kb": 1000,
        "kib": 1024,
        "mb": 1000**2,
        "mib": 1024**2,
        "gb": 1000**3,
        "gib": 1024**3,
        "tb": 1000**4,
        "tib": 1024**4,
    }.get(unit, 1)
    return int(val * mult)


def format_bytes_label(num_bytes: int) -> str:
    if num_bytes <= 0:
        return "N/A"
    if num_bytes >= 1024**3:
        return f"{num_bytes / (1024**3):.2f} GiB"
    if num_bytes >= 1024**2:
        return f"{num_bytes / (1024**2):.1f} MiB"
    if num_bytes >= 1024:
        return f"{num_bytes / 1024:.1f} KiB"
    return f"{num_bytes} B"


@dataclass(slots=True)
class ProviderSearchPage:
    """Structured result from a single SourceProvider.search() invocation."""

    provider_id: str
    provider_name: str
    releases: list[TorrentRelease] = field(default_factory=list)
    page: int = 1
    total_pages: int = 1
    has_next_page: bool = False
    error: str | None = None
    request_url: str = ""


class SourceProvider(ABC):
    """Abstract base interface for all torrent/magnet source providers."""

    provider_id: str = "base"
    provider_name: str = "BaseProvider"
    min_request_interval: float = 0.15

    def __init__(self) -> None:
        self._rate_lock = threading.Lock()
        self._last_request_ts = 0.0

    def _respect_rate_limit(self) -> None:
        with self._rate_lock:
            now = time.monotonic()
            elapsed = now - self._last_request_ts
            if elapsed < self.min_request_interval:
                time.sleep(self.min_request_interval - elapsed)
            self._last_request_ts = time.monotonic()

    @abstractmethod
    def search(
        self,
        query: str,
        page: int = 1,
        filters: SourceSearchFilters | None = None,
        demo_mode: bool = False,
        force_refresh: bool = False,
    ) -> ProviderSearchPage:
        """Search the provider for releases matching query, page, and filters."""

    @abstractmethod
    def get_details(self, result_id: str) -> dict[str, Any]:
        """Retrieve provider-specific details for a release ID."""

    @abstractmethod
    def normalize_result(self, raw_result: Any) -> TorrentRelease:
        """Normalize a provider's raw item into a canonical TorrentRelease."""


class NyaaSourceProvider(SourceProvider):
    """Nyaa (nyaa.si) SourceProvider adapter supporting multi-category filtering and pagination."""

    provider_id = "nyaa"
    provider_name = "Nyaa"
    min_request_interval = 0.15

    def __init__(self, scraper: ConfigurableTableScraper | None = None) -> None:
        super().__init__()
        self.scraper = scraper or ConfigurableTableScraper()

    def resolve_category_code(self, filters: SourceSearchFilters | None) -> str:
        if filters is None or not filters.category:
            return "1_0"
        return NYAA_CATEGORY_MAP.get(filters.category, "1_0")

    def search(
        self,
        query: str,
        page: int = 1,
        filters: SourceSearchFilters | None = None,
        demo_mode: bool = False,
        force_refresh: bool = False,
    ) -> ProviderSearchPage:
        cat_code = self.resolve_category_code(filters)
        filter_mode = (filters.filter_mode if filters and filters.filter_mode in {"0", "1", "2"} else "0")
        sort_by = (filters.sort_by if filters and filters.sort_by in {"seeders", "size", "id", "leechers"} else "seeders")
        sort_order = (filters.sort_order if filters and filters.sort_order in {"desc", "asc"} else "desc")

        if not demo_mode:
            self._respect_rate_limit()

        resp = self.scraper.search(
            query=query,
            demo_mode=demo_mode,
            page=page,
            force_refresh=force_refresh,
            category=cat_code,
            filter_mode=filter_mode,
            sort_by=sort_by,
            sort_order=sort_order,
        )
        releases = [self.normalize_result(item) for item in resp.items]
        for rel in releases:
            if not rel.category:
                rel.category = cat_code
        return ProviderSearchPage(
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            releases=releases,
            page=resp.page,
            total_pages=resp.total_pages,
            has_next_page=resp.has_next_page,
            error=resp.error,
            request_url=resp.request_url,
        )

    def get_details(self, result_id: str) -> dict[str, Any]:
        clean_id = str(result_id).removeprefix("nyaa:")
        base = self.scraper.base_url.rstrip("/") or "https://nyaa.si"
        return {
            "provider_id": self.provider_id,
            "result_id": clean_id,
            "view_url": f"{base}/view/{clean_id}",
            "torrent_url": f"{base}/download/{clean_id}.torrent",
        }

    def normalize_result(self, raw_result: ScrapedItem) -> TorrentRelease:
        tech = infer_technical_metadata(raw_result.title)
        parsed = parse_release_title(raw_result.title)
        raw_hash = raw_result.info_hash or extract_info_hash(raw_result.magnet_link or raw_result.resolved_link)
        info_hash = raw_hash.lower() if len(raw_hash) == 40 else raw_hash
        view_id = (
            raw_result.torrent_id
            or (raw_result.view_url or "").rstrip("/").split("/")[-1]
            or str(raw_result.id)
        )
        return TorrentRelease(
            release_id=f"nyaa:{view_id}",
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            title=raw_result.title,
            release_title=raw_result.release_title or raw_result.title,
            display_title=raw_result.display_title,
            alternative_title=raw_result.alternative_title,
            torrent_id=raw_result.torrent_id or view_id,
            row_class=raw_result.row_class or "default",
            category_code=raw_result.category_code or raw_result.category or "1_2",
            category_name=raw_result.category_name,
            magnet_uri=raw_result.magnet_uri or raw_result.magnet_link or "",
            torrent_url=raw_result.torrent_url or raw_result.torrent_link or "",
            view_url=raw_result.view_url or "",
            details_url=raw_result.details_url or raw_result.view_url or "",
            info_hash=info_hash,
            magnet_tracker_count=raw_result.magnet_tracker_count,
            trackers=list(raw_result.trackers) if raw_result.trackers else [],
            size=raw_result.size,
            file_size=raw_result.file_size or raw_result.size,
            size_bytes=parse_size_to_bytes(raw_result.size),
            date=raw_result.date,
            upload_date=raw_result.upload_date or raw_result.date,
            timestamp=raw_result.timestamp,
            relative_date=raw_result.relative_date,
            seeders=raw_result.seeders,
            leechers=raw_result.leechers,
            downloads=raw_result.downloads,
            category=raw_result.category_code or raw_result.category or "1_0",
            resolution=raw_result.resolution or tech["resolution"],
            codec=raw_result.codec or tech["codec"],
            container=tech["container"],
            subtitle_lang=raw_result.subtitle_lang or tech["subtitle_lang"],
            release_group=raw_result.release_group or parsed.release_group,
            is_valid=raw_result.is_valid,
            verification_msg=raw_result.verification_msg,
            mirror_providers=[self.provider_name],
        )


class SubsPleaseSourceProvider(SourceProvider):
    """SubsPlease JSON API SourceProvider adapter for multi-resolution anime releases."""

    provider_id = "subsplease"
    provider_name = "SubsPlease"
    min_request_interval = 0.25

    def __init__(
        self,
        api_url: str = "https://subsplease.org/api/",
        timeout: float = 8.0,
        client: httpx.Client | None = None,
    ) -> None:
        super().__init__()
        self.api_url = api_url
        self.timeout = timeout
        self._client = client
        self._shared_client: httpx.Client | None = None
        self._cache_lock = threading.Lock()
        self._cache: dict[tuple[str, bool], tuple[float, list[TorrentRelease]]] = {}

    def _get_client(self) -> httpx.Client:
        if self._client is not None:
            return self._client
        if self._shared_client is None or self._shared_client.is_closed:
            self._shared_client = httpx.Client(
                timeout=self.timeout,
                follow_redirects=True,
                headers={
                    "Accept": "application/json",
                    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) Intezam/2.0",
                },
            )
        return self._shared_client

    def search(
        self,
        query: str,
        page: int = 1,
        filters: SourceSearchFilters | None = None,
        demo_mode: bool = False,
        force_refresh: bool = False,
    ) -> ProviderSearchPage:
        cleaned = (query or "").strip()
        if filters and filters.category in {"live_action", "live_action_eng", "live_action_raw", "4_0"}:
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=page,
                total_pages=1,
                has_next_page=False,
            )

        if demo_mode:
            releases = self._get_demo_releases(cleaned, page)
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=releases,
                page=page,
                total_pages=2,
                has_next_page=(page < 2),
                request_url=f"mock://subsplease?s={quote_plus(cleaned)}&p={page}",
            )

        if not cleaned:
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=page,
                total_pages=1,
                has_next_page=False,
            )

        cache_key = (cleaned.lower(), False)
        now = time.monotonic()
        if not force_refresh:
            with self._cache_lock:
                cached = self._cache.get(cache_key)
                if cached and (now - cached[0]) < 90.0:
                    all_releases = cached[1]
                    return self._slice_page(all_releases, page, cleaned)

        self._respect_rate_limit()
        req_url = f"{self.api_url}?f=search&tz=UTC&s={quote_plus(cleaned)}"
        try:
            resp = self._get_client().get(req_url, timeout=self.timeout)
            resp.raise_for_status()
            payload = resp.json()
            all_releases: list[TorrentRelease] = []
            if isinstance(payload, dict):
                for key, entry in payload.items():
                    if isinstance(entry, dict):
                        all_releases.extend(self._expand_entry(key, entry))
            with self._cache_lock:
                self._cache[cache_key] = (time.monotonic(), all_releases)
            return self._slice_page(all_releases, page, cleaned)
        except Exception as exc:
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=page,
                total_pages=1,
                has_next_page=False,
                error=f"SubsPlease request failed: {exc}",
                request_url=req_url,
            )

    def _slice_page(
        self,
        all_releases: list[TorrentRelease],
        page: int,
        query: str,
    ) -> ProviderSearchPage:
        per_page = 30
        total_pages = max(1, (len(all_releases) + per_page - 1) // per_page)
        start = (max(1, page) - 1) * per_page
        end = start + per_page
        sliced = all_releases[start:end]
        return ProviderSearchPage(
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            releases=sliced,
            page=page,
            total_pages=total_pages,
            has_next_page=(page < total_pages),
            request_url=f"{self.api_url}?f=search&tz=UTC&s={quote_plus(query)}",
        )

    def _expand_entry(self, key: str, entry: dict[str, Any]) -> list[TorrentRelease]:
        show_name = str(entry.get("show") or key).strip()
        ep_str = str(entry.get("episode") or "").strip()
        release_date = str(entry.get("release_date") or "").strip()
        page_slug = str(entry.get("page") or "").strip()
        downloads = entry.get("downloads") or []
        out: list[TorrentRelease] = []
        for dl in downloads:
            if not isinstance(dl, dict):
                continue
            res_str = str(dl.get("res") or "1080").strip()
            magnet = str(dl.get("magnet") or "").strip()
            if not magnet:
                continue
            out.append(
                self.normalize_result(
                    {
                        "show": show_name,
                        "episode": ep_str,
                        "res": f"{res_str}p" if not res_str.endswith("p") else res_str,
                        "magnet": magnet,
                        "release_date": release_date,
                        "page": page_slug,
                    }
                )
            )
        return out

    def get_details(self, result_id: str) -> dict[str, Any]:
        return {"provider_id": self.provider_id, "result_id": result_id}

    def normalize_result(self, raw_result: dict[str, Any]) -> TorrentRelease:
        show_name = str(raw_result.get("show") or "Unknown").strip()
        ep_str = str(raw_result.get("episode") or "").strip()
        res = str(raw_result.get("res") or "1080p").strip()
        magnet = str(raw_result.get("magnet") or "").strip()
        page_slug = str(raw_result.get("page") or "").strip()
        info_hash = extract_info_hash(magnet)

        is_pack = "-" in ep_str or ep_str.lower() == "batch"
        ep_label = f"{ep_str} [Batch]" if is_pack else ep_str
        title = f"[SubsPlease] {show_name} - {ep_label} ({res}) [AVC].mkv" if ep_str else f"[SubsPlease] {show_name} ({res}).mkv"
        size_label = "1.4 GiB" if "1080" in res else "700 MiB" if "720" in res else "360 MiB"
        if is_pack:
            size_label = "14.2 GiB" if "1080" in res else "6.8 GiB"

        return TorrentRelease(
            release_id=f"subsplease:{info_hash or title}",
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            title=title,
            magnet_uri=magnet,
            torrent_url="",
            view_url=f"https://subsplease.org/shows/{page_slug}/" if page_slug else "https://subsplease.org",
            info_hash=info_hash,
            size=size_label,
            size_bytes=parse_size_to_bytes(size_label),
            date=str(raw_result.get("release_date") or ""),
            seeders=180 if "1080" in res else 95,
            leechers=12,
            downloads=1200,
            category="1_2",
            resolution=res,
            codec="H.264 / x264",
            container="MKV",
            subtitle_lang="Multi-Sub",
            release_group="SubsPlease",
            is_valid=bool(info_hash),
            verification_msg="Valid SubsPlease magnet" if info_hash else "Missing BTIH",
            mirror_providers=[self.provider_name],
        )

    def _get_demo_releases(self, query: str, page: int) -> list[TorrentRelease]:
        topic = (query or "Frieren: Beyond Journey's End").strip()
        lowered = topic.lower()
        if "nonexistent" in lowered or "your name" in lowered or "kimi no na wa" in lowered:
            return []
        import hashlib

        releases: list[TorrentRelease] = []
        start_ep = 1 if page == 1 else 5
        for ep in range(start_ep, start_ep + 4):
            for res, size_lbl, seeds in [("1080p", "1.4 GiB", 620), ("720p", "680.0 MiB", 240)]:
                # Use the exact same title format for EP 01 1080p so it shares info_hash with Nyaa and demonstrates mirror merging!
                if ep <= 2 and res == "1080p":
                    title = f"[SubsPlease] {topic} - {ep:02d} (1080p) [HEVC].mkv"
                else:
                    title = f"[SubsPlease] {topic} - {ep:02d} ({res}) [Multi-Sub].mkv"
                btih = hashlib.sha1(title.encode("utf-8")).hexdigest()
                magnet = f"magnet:?xt=urn:btih:{btih}&dn={quote_plus(title)}"
                releases.append(
                    TorrentRelease(
                        release_id=f"subsplease:{btih[:12]}",
                        provider_id=self.provider_id,
                        provider_name=self.provider_name,
                        title=title,
                        magnet_uri=magnet,
                        view_url="https://subsplease.org",
                        info_hash=btih,
                        size=size_lbl,
                        size_bytes=parse_size_to_bytes(size_lbl),
                        date="2026-11-14 18:00",
                        seeders=seeds - ep * 7,
                        leechers=19,
                        downloads=3400,
                        category="1_2",
                        resolution=res,
                        codec="HEVC / x265" if "HEVC" in title else "H.264 / x264",
                        container="MKV",
                        subtitle_lang="Multi-Sub",
                        release_group="SubsPlease",
                        is_valid=True,
                        verification_msg="Valid SubsPlease magnet",
                        mirror_providers=[self.provider_name],
                    )
                )
        return releases


class EZTVSourceProvider(SourceProvider):
    """EZTV JSON API SourceProvider adapter for TV series by IMDb ID or title."""

    provider_id = "eztv"
    provider_name = "EZTV"
    min_request_interval = 0.25

    def __init__(
        self,
        api_url: str = "https://eztvx.to/api/get-torrents",
        timeout: float = 8.0,
        client: httpx.Client | None = None,
    ) -> None:
        super().__init__()
        self.api_url = api_url
        self.timeout = timeout
        self._client = client
        self._shared_client: httpx.Client | None = None

    def _get_client(self) -> httpx.Client:
        if self._client is not None:
            return self._client
        if self._shared_client is None or self._shared_client.is_closed:
            self._shared_client = httpx.Client(
                timeout=self.timeout,
                follow_redirects=True,
                headers={
                    "Accept": "application/json",
                    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) Intezam/2.0",
                },
            )
        return self._shared_client

    def search(
        self,
        query: str,
        page: int = 1,
        filters: SourceSearchFilters | None = None,
        demo_mode: bool = False,
        force_refresh: bool = False,
    ) -> ProviderSearchPage:
        imdb_raw = (filters.imdb_id if filters else None) or ""
        imdb_digits = re.sub(r"\D", "", imdb_raw)
        q_lower = (query or "").lower()
        if demo_mode:
            if "nonexistent" in q_lower:
                return ProviderSearchPage(
                    provider_id=self.provider_id,
                    provider_name=self.provider_name,
                    releases=[],
                    page=page,
                    total_pages=1,
                    has_next_page=False,
                )
            known_tv = any(k in q_lower for k in ("breaking bad", "the office", "dark", "one piece"))
            if not imdb_digits and not known_tv:
                return ProviderSearchPage(
                    provider_id=self.provider_id,
                    provider_name=self.provider_name,
                    releases=[],
                    page=page,
                    total_pages=1,
                    has_next_page=False,
                )
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=self._get_demo_releases(query or "Breaking Bad", page),
                page=page,
                total_pages=2,
                has_next_page=(page < 2),
                request_url=f"mock://eztv?imdb_id={imdb_digits or '0903747'}&page={page}",
            )

        if not imdb_digits:
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=page,
                total_pages=1,
                has_next_page=False,
            )

        self._respect_rate_limit()
        params = {"imdb_id": imdb_digits, "limit": 50, "page": max(1, page)}
        req_url = f"{self.api_url}?imdb_id={imdb_digits}&limit=50&page={max(1, page)}"
        try:
            resp = self._get_client().get(self.api_url, params=params, timeout=self.timeout)
            resp.raise_for_status()
            data = resp.json() or {}
            torrents = data.get("torrents") or []
            filtered_torrents: list[dict[str, Any]] = []
            q_norm, q_toks = normalize_title_tokens(query or "")
            for t in torrents:
                if not isinstance(t, dict):
                    continue
                t_imdb = re.sub(r"\D", "", str(t.get("imdb_id") or ""))
                t_title = str(t.get("title") or t.get("filename") or "")
                parsed_t = parse_release_title(t_title)
                rel_norm, rel_toks = normalize_title_tokens(parsed_t.show_title or t_title)
                title_sim = 0.0
                if rel_norm and q_norm:
                    title_sim, _ = TitleIdentityMatcher._compare_normalized(rel_norm, rel_toks, q_norm, q_toks)
                if (t_imdb and t_imdb == imdb_digits and title_sim >= 0.65) or title_sim >= 0.72:
                    filtered_torrents.append(t)
            total_count = int(data.get("torrents_count") or len(filtered_torrents))
            if not filtered_torrents and torrents:
                total_count = 0
            total_pages = max(1, (total_count + 49) // 50) if total_count > 0 else 1
            releases = [self.normalize_result(t) for t in filtered_torrents]
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=releases,
                page=page,
                total_pages=total_pages,
                has_next_page=(page < total_pages and len(releases) > 0),
                request_url=req_url,
            )
        except Exception as exc:
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=page,
                total_pages=1,
                has_next_page=False,
                error=f"EZTV request failed: {exc}",
                request_url=req_url,
            )

    def get_details(self, result_id: str) -> dict[str, Any]:
        return {"provider_id": self.provider_id, "result_id": result_id}

    def normalize_result(self, raw_result: dict[str, Any]) -> TorrentRelease:
        title = str(raw_result.get("title") or raw_result.get("filename") or "Untitled").strip()
        magnet = str(raw_result.get("magnet_url") or "").strip()
        torrent_url = str(raw_result.get("torrent_url") or "").strip()
        info_hash = str(raw_result.get("hash") or extract_info_hash(magnet)).strip().lower()
        size_bytes = int(raw_result.get("size_bytes") or 0)
        size_str = format_bytes_label(size_bytes)
        tech = infer_technical_metadata(title)
        parsed = parse_release_title(title)

        return TorrentRelease(
            release_id=f"eztv:{raw_result.get('id') or info_hash}",
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            title=title,
            magnet_uri=magnet,
            torrent_url=torrent_url,
            view_url=str(raw_result.get("episode_url") or "https://eztvx.to"),
            info_hash=info_hash,
            size=size_str,
            size_bytes=size_bytes,
            date=str(raw_result.get("date_released_unix") or ""),
            seeders=int(raw_result.get("seeds") or 0),
            leechers=int(raw_result.get("peers") or 0),
            downloads=0,
            category="tv",
            resolution=tech["resolution"],
            codec=tech["codec"],
            container=tech["container"],
            subtitle_lang=tech["subtitle_lang"],
            release_group=parsed.release_group,
            is_valid=bool(info_hash or torrent_url),
            verification_msg="Valid EZTV release",
            mirror_providers=[self.provider_name],
        )

    def _get_demo_releases(self, show_title: str, page: int) -> list[TorrentRelease]:
        import hashlib

        out: list[TorrentRelease] = []
        for ep in range(1, 6):
            for res, sz in [("1080p", "1.6 GiB"), ("720p", "820.0 MiB")]:
                title = f"{show_title} S01E{ep:02d} {res} WEB H264-GROUP[EZTV].mkv"
                btih = hashlib.sha1(title.encode("utf-8")).hexdigest()
                out.append(
                    TorrentRelease(
                        release_id=f"eztv:{btih[:10]}",
                        provider_id=self.provider_id,
                        provider_name=self.provider_name,
                        title=title,
                        magnet_uri=f"magnet:?xt=urn:btih:{btih}&dn={quote_plus(title)}",
                        info_hash=btih,
                        size=sz,
                        size_bytes=parse_size_to_bytes(sz),
                        date="2026-11-14",
                        seeders=310 - ep * 12,
                        leechers=18,
                        category="tv",
                        resolution=res,
                        codec="H.264 / x264",
                        container="MKV",
                        release_group="EZTV",
                        is_valid=True,
                        verification_msg="Valid EZTV magnet",
                        mirror_providers=[self.provider_name],
                    )
                )
        return out


_DEFAULT_UDP_TRACKERS: tuple[str, ...] = (
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.stealth.si:80/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://exodus.desync.com:6969/announce",
)


class APIBaySourceProvider(SourceProvider):
    """Public APIBay JSON SourceProvider adapter for Western TV Series and Feature Films."""

    provider_id = "apibay"
    provider_name = "APIBay"
    min_request_interval = 0.2

    def __init__(
        self,
        api_url: str = "https://apibay.org/q.php",
        timeout: float = 5.5,
        client: httpx.Client | None = None,
    ) -> None:
        super().__init__()
        self.api_url = api_url
        self.timeout = timeout
        self._client = client
        self._shared_client: httpx.Client | None = None

    def _get_client(self) -> httpx.Client:
        if self._client is not None:
            return self._client
        if self._shared_client is None or self._shared_client.is_closed:
            self._shared_client = httpx.Client(
                timeout=self.timeout,
                follow_redirects=True,
                headers={
                    "Accept": "application/json",
                    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) Intezam/2.0",
                },
            )
        return self._shared_client

    def search(
        self,
        query: str,
        page: int = 1,
        filters: SourceSearchFilters | None = None,
        demo_mode: bool = False,
        force_refresh: bool = False,
    ) -> ProviderSearchPage:
        if demo_mode or page > 1 or not (query or "").strip():
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=page,
                total_pages=1,
                has_next_page=False,
            )

        self._respect_rate_limit()
        cleaned = query.strip()
        params = {"q": cleaned, "cat": "200"}
        req_url = f"{self.api_url}?q={quote_plus(cleaned)}&cat=200"
        try:
            resp = self._get_client().get(self.api_url, params=params, timeout=self.timeout)
            resp.raise_for_status()
            rows = resp.json() or []
            if not isinstance(rows, list):
                rows = []
            releases: list[TorrentRelease] = []
            for row in rows:
                if not isinstance(row, dict):
                    continue
                row_id = str(row.get("id") or "0")
                ih = str(row.get("info_hash") or "").strip().lower()
                name = str(row.get("name") or "").strip()
                if row_id == "0" or not ih or ih == ("0" * 40) or not name or name.lower() == "no results returned":
                    continue
                releases.append(self.normalize_result(row))
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=releases,
                page=1,
                total_pages=1,
                has_next_page=False,
                request_url=req_url,
            )
        except Exception as exc:
            return ProviderSearchPage(
                provider_id=self.provider_id,
                provider_name=self.provider_name,
                releases=[],
                page=1,
                total_pages=1,
                has_next_page=False,
                error=f"APIBay request failed: {exc}",
                request_url=req_url,
            )

    def get_details(self, result_id: str) -> dict[str, Any]:
        return {"provider_id": self.provider_id, "result_id": result_id}

    def normalize_result(self, raw_result: dict[str, Any]) -> TorrentRelease:
        name = str(raw_result.get("name") or "Untitled").strip()
        ih = str(raw_result.get("info_hash") or "").strip().lower()
        tr_params = "".join(f"&tr={quote_plus(tr)}" for tr in _DEFAULT_UDP_TRACKERS)
        magnet = f"magnet:?xt=urn:btih:{ih}&dn={quote_plus(name)}{tr_params}"
        size_bytes = int(raw_result.get("size") or 0)
        size_str = format_bytes_label(size_bytes)
        tech = infer_technical_metadata(name)
        parsed = parse_release_title(name)
        return TorrentRelease(
            release_id=f"apibay:{raw_result.get('id') or ih[:12]}",
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            title=name,
            magnet_uri=magnet,
            view_url=f"https://thepiratebay.org/description.php?id={raw_result.get('id') or ''}",
            info_hash=ih,
            size=size_str,
            size_bytes=size_bytes,
            date="",
            seeders=int(raw_result.get("seeders") or 0),
            leechers=int(raw_result.get("leechers") or 0),
            downloads=0,
            category="tv",
            resolution=tech["resolution"],
            codec=tech["codec"],
            container=tech["container"],
            subtitle_lang=tech["subtitle_lang"],
            release_group=parsed.release_group,
            is_valid=bool(len(ih) == 40),
            verification_msg="Valid APIBay BTIH magnet",
            mirror_providers=[self.provider_name],
        )


def deduplicate_releases(releases: list[TorrentRelease]) -> list[TorrentRelease]:
    """
    Deduplicate TorrentRelease objects by BTIH info-hash (or exact magnet/torrent URL when hash is absent),
    preserving distinct torrents that have similar titles while merging mirror_providers on exact hash matches.
    """
    by_key: dict[str, TorrentRelease] = {}
    ordered_keys: list[str] = []

    for rel in releases:
        ih = (rel.info_hash or extract_info_hash(rel.magnet_uri)).strip().lower()
        if ih:
            rel.info_hash = ih
            key = f"btih:{ih}"
        elif rel.magnet_uri:
            key = f"magnet:{rel.magnet_uri.strip().lower()}"
        elif rel.torrent_url:
            key = f"torrent:{rel.torrent_url.strip().lower()}"
        else:
            key = f"id:{rel.provider_id}:{rel.release_id}:{rel.title.strip().lower()}"

        existing = by_key.get(key)
        if existing is None:
            if not rel.mirror_providers:
                rel.mirror_providers = [rel.provider_name]
            by_key[key] = rel
            ordered_keys.append(key)
        else:
            # Merge mirror providers and retain higher seeder count / magnet link
            for mp in [rel.provider_name, *rel.mirror_providers]:
                if mp and mp not in existing.mirror_providers:
                    existing.mirror_providers.append(mp)
            if rel.seeders > existing.seeders:
                existing.seeders = rel.seeders
                existing.leechers = max(existing.leechers, rel.leechers)
            if not existing.magnet_uri and rel.magnet_uri:
                existing.magnet_uri = rel.magnet_uri
            if not existing.torrent_url and rel.torrent_url:
                existing.torrent_url = rel.torrent_url

    return [by_key[k] for k in ordered_keys]


def build_show_query_plan(identity: MediaIdentity, max_queries: int = 5) -> list[str]:
    """
    Stage 5: Build a prioritized, non-redundant query plan for source discovery.
    Covers official title, English title, Romaji title, short/spacing variants,
    season aliases, and year-qualified title for movies/remakes.
    """
    raw_candidates = identity.all_search_titles(max_queries=max_queries + 3)
    plan: list[str] = []
    seen_norm: set[str] = set()

    def _add_query(candidate: str | None) -> None:
        if not candidate or len(plan) >= max_queries:
            return
        cleaned = re.sub(r"[\u2018\u2019\u02bc`]", "'", candidate.strip())
        cleaned = re.sub(r"\s+", " ", cleaned).strip()
        if len(cleaned) < 2:
            return
        # skip CJK-only queries on ASCII-oriented torrent search unless no ASCII exists
        if re.search(r"[\u3040-\u30ff\u4e00-\u9faf]", cleaned) and plan:
            return
        norm_k = normalize_title_tokens(cleaned)[0]
        if not norm_k or norm_k in seen_norm:
            return
        seen_norm.add(norm_k)
        plan.append(cleaned)

    for cand in raw_candidates:
        _add_query(cand)
        # If title ends with a trailing period (e.g. "Your Name."), also add the period-stripped form
        if cand and cand.endswith("."):
            _add_query(cand.rstrip(".").strip())
        # If title has colonized subtitle (e.g. "Frieren: Beyond Journey's End"), also consider short form if >= 6 chars
        if cand and ":" in cand:
            prefix = cand.split(":", 1)[0].strip()
            if len(prefix) >= 6 and prefix.lower() not in {"naruto", "bleach", "one piece"}:
                _add_query(prefix)

    if not plan and identity.official_title:
        plan.append(identity.official_title.strip())
    return plan[:max_queries]


class MultiProviderDiscoveryEngine:
    """
    Stage 5, 6, 7 & 9 Orchestrator:
    Queries multiple SourceProviders in bounded parallel across prioritized show title aliases,
    maintains independent per-(provider, query) pagination cursors so exhausted queries are not
    re-fetched, isolates provider failures, deduplicates by BTIH info-hash, and classifies releases
    into confirmed, possible, unclassified, and rejected buckets.
    """

    def __init__(
        self,
        nyaa: NyaaSourceProvider | None = None,
        subsplease: SubsPleaseSourceProvider | None = None,
        eztv: EZTVSourceProvider | None = None,
        apibay: APIBaySourceProvider | None = None,
    ) -> None:
        self.nyaa = nyaa or NyaaSourceProvider()
        self.subsplease = subsplease or SubsPleaseSourceProvider()
        self.eztv = eztv or EZTVSourceProvider()
        self.apibay = apibay or APIBaySourceProvider()
        self.providers: dict[str, SourceProvider] = {
            self.nyaa.provider_id: self.nyaa,
            self.subsplease.provider_id: self.subsplease,
            self.eztv.provider_id: self.eztv,
            self.apibay.provider_id: self.apibay,
        }
        self._cursor_state: dict[str, dict[str, dict[str, Any]]] = {}
        self.last_pagination_cursors: dict[str, dict[str, Any]] = {}
        self.last_rejected_items: list[ScrapedItem] = []

    def discover_for_show(
        self,
        identity: MediaIdentity,
        page: int = 1,
        demo_mode: bool = False,
        force_refresh: bool = False,
        filters: SourceSearchFilters | None = None,
        cursors: dict[str, dict[str, Any]] | None = None,
    ) -> tuple[list[ScrapedItem], list[ProviderRetrievalStatus], bool, int]:
        """
        Execute bounded parallel torrent discovery for a canonical MediaIdentity across configured providers
        and primary/alternate titles, then match and annotate every release.
        """
        eff_filters = SourceSearchFilters(
            category=(
                filters.category
                if filters and filters.category
                else ("anime_all" if identity.media_type == "anime" else "all")
            ),
            filter_mode=(filters.filter_mode if filters else "0"),
            sort_by=(filters.sort_by if filters else "seeders"),
            sort_order=(filters.sort_order if filters else "desc"),
            providers=(list(filters.providers) if filters and filters.providers else []),
            resolution=(filters.resolution if filters else ""),
            imdb_id=(filters.imdb_id if filters and filters.imdb_id else identity.imdb_id),
        )
        # Do not restrict Live-Action TV searches on Nyaa to Anime-only category "1_0"
        if identity.media_type == "tv" and eff_filters.category in {"1_0", "anime_all"}:
            eff_filters.category = "all"

        search_titles = build_show_query_plan(identity, max_queries=5)
        if demo_mode:
            search_titles = [identity.official_title]
            if identity.media_type == "movie" and identity.romaji_title and identity.romaji_title not in search_titles:
                search_titles.append(identity.romaji_title)

        active_provider_ids = (
            [p.lower() for p in eff_filters.providers if p.lower() in self.providers]
            if eff_filters.providers
            else list(self.providers.keys())
        )

        # Build candidate (provider, query_title) pairs
        candidate_pairs: list[tuple[SourceProvider, str]] = []
        for pid in active_provider_ids:
            prov = self.providers[pid]
            if pid == "eztv":
                candidate_pairs.append((prov, search_titles[0]))
            elif pid == "subsplease":
                if identity.media_type == "anime":
                    for q_title in search_titles[:2]:
                        candidate_pairs.append((prov, q_title))
            elif pid == "apibay":
                if not demo_mode and identity.media_type in {"tv", "movie"}:
                    if identity.media_type == "movie" and identity.release_year:
                        ab_q = f"{identity.official_title.rstrip('.')} {identity.release_year}"
                    elif identity.media_type == "tv" and getattr(identity, "country", "").upper() == "GB" and "uk" not in identity.official_title.lower():
                        ab_q = f"{identity.official_title} UK"
                    else:
                        ab_q = search_titles[0]
                    candidate_pairs.append((prov, ab_q))
            else:
                nyaa_limit = 1 if identity.media_type == "tv" else 2
                for q_title in search_titles[:nyaa_limit]:
                    candidate_pairs.append((prov, q_title))

        session_key = (
            f"{identity.canonical_id}:{demo_mode}:{eff_filters.category}:"
            f"{eff_filters.sort_by}:{eff_filters.resolution}"
        )
        if cursors is not None:
            session_cursors = {
                str(k): dict(v) for k, v in cursors.items() if isinstance(v, dict)
            }
        elif page <= 1 or force_refresh or session_key not in self._cursor_state:
            session_cursors = {}
        else:
            session_cursors = {
                str(k): dict(v) for k, v in self._cursor_state[session_key].items()
            }

        active_tasks: list[tuple[SourceProvider, str, int, str]] = []
        statuses: list[ProviderRetrievalStatus] = []

        for prov, q_title in candidate_pairs:
            cursor_key = f"{prov.provider_id}:{q_title}"
            prev_cursor = session_cursors.get(cursor_key)
            if page <= 1 and not cursors:
                q_page = 1
                active_tasks.append((prov, q_title, q_page, cursor_key))
            else:
                if prev_cursor is not None and not prev_cursor.get("has_more", True):
                    # Exhausted query/provider cursor: do NOT re-fetch!
                    statuses.append(
                        ProviderRetrievalStatus(
                            provider_id=prov.provider_id,
                            provider_name=prov.provider_name,
                            status="exhausted",
                            items_found=0,
                            page=int(prev_cursor.get("page", 1)),
                            total_pages=int(prev_cursor.get("total_pages", 1)),
                            has_next_page=False,
                            query=q_title,
                            cursor_key=cursor_key,
                        )
                    )
                    continue
                q_page = (int(prev_cursor.get("page", page - 1)) + 1) if prev_cursor else max(1, page)
                active_tasks.append((prov, q_title, q_page, cursor_key))

        raw_releases: list[TorrentRelease] = []

        if active_tasks:
            with ThreadPoolExecutor(max_workers=min(6, max(1, len(active_tasks)))) as pool:
                ordered_futures = [
                    (
                        pool.submit(
                            prov.search,
                            q_title,
                            q_page,
                            eff_filters,
                            demo_mode,
                            force_refresh,
                        ),
                        prov,
                        q_title,
                        q_page,
                        cursor_key,
                    )
                    for prov, q_title, q_page, cursor_key in active_tasks
                ]
                for fut, prov, q_title, q_page, cursor_key in ordered_futures:
                    st = ProviderRetrievalStatus(
                        provider_id=prov.provider_id,
                        provider_name=prov.provider_name,
                        status="ok",
                        page=q_page,
                        total_pages=1,
                        has_next_page=False,
                        query=q_title,
                        cursor_key=cursor_key,
                    )
                    try:
                        res_page = fut.result(timeout=12.0)
                        if res_page.request_url:
                            st.queried_urls.append(res_page.request_url)
                        if res_page.error:
                            st.error = res_page.error
                            st.status = "error"
                            session_cursors[cursor_key] = {
                                "provider_id": prov.provider_id,
                                "provider_name": prov.provider_name,
                                "query": q_title,
                                "page": q_page,
                                "total_pages": res_page.total_pages,
                                "has_more": False,
                                "items_found": 0,
                                "status": "error",
                                "error": res_page.error,
                            }
                        else:
                            raw_releases.extend(res_page.releases)
                            st.items_found = len(res_page.releases)
                            st.total_pages = max(1, res_page.total_pages)
                            st.has_next_page = bool(res_page.has_next_page and len(res_page.releases) > 0)
                            if not st.has_next_page and st.items_found == 0 and q_page > 1:
                                st.status = "exhausted"
                            session_cursors[cursor_key] = {
                                "provider_id": prov.provider_id,
                                "provider_name": prov.provider_name,
                                "query": q_title,
                                "page": q_page,
                                "total_pages": st.total_pages,
                                "has_more": st.has_next_page,
                                "items_found": st.items_found,
                                "status": st.status,
                            }
                    except Exception as exc:
                        st.error = str(exc)
                        st.status = "error"
                        session_cursors[cursor_key] = {
                            "provider_id": prov.provider_id,
                            "provider_name": prov.provider_name,
                            "query": q_title,
                            "page": q_page,
                            "total_pages": 1,
                            "has_more": False,
                            "items_found": 0,
                            "status": "error",
                            "error": str(exc),
                        }
                    statuses.append(st)

        self._cursor_state[session_key] = {
            str(k): dict(v) for k, v in session_cursors.items()
        }
        self.last_pagination_cursors = {
            str(k): dict(v) for k, v in session_cursors.items()
        }

        deduped = deduplicate_releases(raw_releases)

        # Stage 5: Match each release against canonical MediaIdentity & preserve 4 buckets
        items: list[ScrapedItem] = []
        rejected_items: list[ScrapedItem] = []
        idx = 0
        rej_idx = 0
        for rel in deduped:
            match_res = match_release_to_identity(rel, identity)
            if match_res.match_bucket == "rejected":
                rej_idx += 1
                rejected_items.append(rel.to_scraped_item(rej_idx, match_res))
                continue
            idx += 1
            scraped = rel.to_scraped_item(idx, match_res)
            if len(rel.mirror_providers) > 1:
                scraped.provider_name = " + ".join(rel.mirror_providers)
            items.append(scraped)

        self.last_rejected_items = rejected_items
        any_has_next = any(bool(c.get("has_more")) for c in session_cursors.values())
        max_total_pages = max((int(c.get("total_pages", 1)) for c in session_cursors.values()), default=1)
        return (items, statuses, any_has_next, max_total_pages)
