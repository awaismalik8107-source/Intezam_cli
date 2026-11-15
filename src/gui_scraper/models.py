"""Canonical metadata, torrent release, source provider, and matching models for Intezam GUI."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any


@dataclass(slots=True)
class ScrapedItem:
    """Represents a single scraped table row and its link verification state (legacy/bridge compatible)."""

    id: int
    title: str
    size: str
    date: str
    stats: str
    raw_link: str
    resolved_link: str
    is_valid: bool
    verification_msg: str
    view_url: str = ""
    magnet_link: str = ""
    torrent_link: str = ""
    relative_date: str = ""
    seeders: int = 0
    leechers: int = 0
    downloads: int = 0
    provider_id: str = "nyaa"
    provider_name: str = "Nyaa"
    category: str = ""
    info_hash: str = ""
    resolution: str = ""
    codec: str = ""
    subtitle_lang: str = ""
    release_title: str = ""
    display_title: str = ""
    alternative_title: str = ""
    release_group: str = ""
    torrent_id: str = ""
    row_class: str = ""
    category_code: str = ""
    category_name: str = ""
    details_url: str = ""
    torrent_url: str = ""
    magnet_uri: str = ""
    file_size: str = ""
    upload_date: str = ""
    timestamp: str = ""
    magnet_tracker_count: int = 0
    trackers: list[str] = field(default_factory=list)
    classification: str = ""
    match_bucket: str = "confirmed"
    season_number: int | None = None
    episode_number: int | None = None
    first_episode: int | None = None
    last_episode: int | None = None
    batch_label: str = ""
    match_confidence: float = 0.0
    match_reason: str = ""
    mirror_providers: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.release_title and self.title:
            self.release_title = self.title
        if not self.details_url and self.view_url:
            self.details_url = self.view_url
        if not self.torrent_url and self.torrent_link:
            self.torrent_url = self.torrent_link
        if not self.magnet_uri and self.magnet_link:
            self.magnet_uri = self.magnet_link
        if not self.file_size and self.size:
            self.file_size = self.size
        if not self.upload_date and self.date:
            self.upload_date = self.date
        if not self.category_code and self.category:
            self.category_code = self.category
        if not self.category and self.category_code:
            self.category = self.category_code
        if self.trackers and self.magnet_tracker_count == 0:
            self.magnet_tracker_count = len(self.trackers)


@dataclass(slots=True)
class EpisodeMetadata:
    """Official episode metadata within a season."""

    episode_number: int
    season_number: int = 1
    title: str = ""
    air_date: str = ""
    overview: str = ""
    still_url: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class SeasonMetadata:
    """Official season metadata and episode catalog for a show."""

    season_number: int
    title: str = ""
    episode_count: int = 0
    air_year: int | None = None
    poster_url: str = ""
    anilist_id: int | None = None
    season_aliases: list[str] = field(default_factory=list)
    episodes: list[EpisodeMetadata] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class MediaIdentity:
    """Canonical show/anime/movie/TV identity separate from torrent release data."""

    canonical_id: str
    official_title: str
    english_title: str = ""
    original_title: str = ""
    romaji_title: str = ""
    alternate_titles: list[str] = field(default_factory=list)
    tmdb_id: int | None = None
    anilist_id: int | None = None
    imdb_id: str | None = None
    tvmaze_id: int | None = None
    mal_id: int | None = None
    release_year: int | None = None
    media_type: str = "anime"  # "anime" | "tv" | "movie"
    country: str = ""
    episode_count: int = 0
    synopsis: str = ""
    genres: list[str] = field(default_factory=list)
    status: str = ""
    poster_url: str = ""
    backdrop_url: str = ""
    seasons: list[SeasonMetadata] = field(default_factory=list)
    metadata_provider: str = ""
    confidence: float = 1.0
    match_score: float = 1.0
    confidence_tier: str = "high"  # "high" | "medium" | "low" | "rejected"
    match_reason: str = ""

    def all_search_titles(self, max_queries: int = 6) -> list[str]:
        """Return prioritized, deduplicated primary, Romaji, alternate, and season titles for torrent queries."""
        import re

        latin_candidates: list[str] = []
        cjk_candidates: list[str] = []
        seen_norm: set[str] = set()

        season_aliases: list[str] = []
        for s_meta in self.seasons:
            for s_alias in s_meta.season_aliases:
                if s_alias and s_alias.strip():
                    season_aliases.append(s_alias.strip())

        raw_pool = [
            self.official_title,
            self.english_title,
            self.romaji_title,
            *self.alternate_titles,
            *season_aliases,
            self.original_title,
        ]
        for raw in raw_pool:
            cleaned = (raw or "").strip()
            if not cleaned or len(cleaned) < 2:
                continue
            norm = re.sub(r"[^\w]+", " ", cleaned.lower()).strip()
            if not norm or norm in seen_norm:
                continue
            seen_norm.add(norm)
            has_latin = bool(re.search(r"[A-Za-z0-9]", cleaned))
            if has_latin:
                latin_candidates.append(cleaned)
            else:
                cjk_candidates.append(cleaned)

        combined = [*latin_candidates, *cjk_candidates]
        return combined[: max(1, max_queries)]

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class SourceSearchFilters:
    """Configurable provider and category filters for torrent discovery."""

    category: str = "all"  # "all" | "anime_all" | "anime_eng" | "anime_raw" | "anime_non_eng" | "live_action" | "tv"
    filter_mode: str = "0"  # "0" no filter, "1" no remakes, "2" trusted only
    sort_by: str = "seeders"
    sort_order: str = "desc"
    providers: list[str] = field(default_factory=list)
    resolution: str = ""
    imdb_id: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class TorrentRelease:
    """Normalized torrent release from any SourceProvider."""

    release_id: str
    provider_id: str
    provider_name: str
    title: str
    magnet_uri: str = ""
    torrent_url: str = ""
    view_url: str = ""
    info_hash: str = ""
    size: str = ""
    size_bytes: int = 0
    date: str = ""
    relative_date: str = ""
    seeders: int = 0
    leechers: int = 0
    downloads: int = 0
    category: str = ""
    resolution: str = ""
    codec: str = ""
    container: str = ""
    subtitle_lang: str = ""
    release_group: str = ""
    release_title: str = ""
    display_title: str = ""
    alternative_title: str = ""
    torrent_id: str = ""
    row_class: str = ""
    category_code: str = ""
    category_name: str = ""
    details_url: str = ""
    file_size: str = ""
    upload_date: str = ""
    timestamp: str = ""
    magnet_tracker_count: int = 0
    trackers: list[str] = field(default_factory=list)
    classification: str = ""
    match_bucket: str = "confirmed"
    season_number: int | None = None
    episode_number: int | None = None
    first_episode: int | None = None
    last_episode: int | None = None
    batch_label: str = ""
    match_confidence: float = 0.0
    match_reason: str = ""
    is_valid: bool = True
    verification_msg: str = ""
    mirror_providers: list[str] = field(default_factory=list)

    def to_scraped_item(
        self,
        numeric_id: int,
        match_result: ReleaseMatchResult | None = None,
    ) -> ScrapedItem:
        """Convert normalized TorrentRelease into legacy ScrapedItem for bridge compatibility."""
        stats = f"S:{self.seeders} L:{self.leechers} D:{self.downloads}"
        primary_link = self.magnet_uri or self.torrent_url or self.view_url
        return ScrapedItem(
            id=numeric_id,
            title=self.title,
            size=self.size or "N/A",
            date=self.date or "N/A",
            stats=stats,
            raw_link=primary_link,
            resolved_link=primary_link,
            is_valid=self.is_valid,
            verification_msg=self.verification_msg or ("Valid" if self.is_valid else "Unverified"),
            view_url=self.view_url,
            magnet_link=self.magnet_uri,
            torrent_link=self.torrent_url,
            relative_date=self.relative_date,
            seeders=self.seeders,
            leechers=self.leechers,
            downloads=self.downloads,
            provider_id=self.provider_id,
            provider_name=self.provider_name,
            category=self.category or self.category_code,
            info_hash=self.info_hash,
            resolution=self.resolution,
            codec=self.codec,
            subtitle_lang=self.subtitle_lang,
            release_title=self.release_title or self.title,
            display_title=self.display_title,
            alternative_title=self.alternative_title,
            release_group=self.release_group,
            torrent_id=self.torrent_id,
            row_class=self.row_class,
            category_code=self.category_code or self.category,
            category_name=self.category_name,
            details_url=self.view_url,
            torrent_url=self.torrent_url,
            magnet_uri=self.magnet_uri,
            file_size=self.size or "N/A",
            upload_date=self.date or "N/A",
            timestamp=self.timestamp,
            magnet_tracker_count=self.magnet_tracker_count or len(self.trackers),
            trackers=list(self.trackers),
            classification=match_result.classification if match_result else "",
            match_bucket=match_result.match_bucket if match_result else "confirmed",
            season_number=match_result.season_number if match_result else None,
            episode_number=match_result.episode_number if match_result else None,
            first_episode=match_result.first_episode if match_result else None,
            last_episode=match_result.last_episode if match_result else None,
            batch_label=match_result.batch_label if match_result else "",
            match_confidence=match_result.confidence if match_result else 1.0,
            match_reason=match_result.match_reason if match_result else "",
            mirror_providers=list(self.mirror_providers),
        )

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class ProviderRetrievalStatus:
    """Per-provider execution status so users can distinguish complete, partial, or failed searches."""

    provider_id: str
    provider_name: str
    status: str = "ok"  # "ok" | "partial" | "error" | "skipped" | "rate_limited"
    items_found: int = 0
    page: int = 1
    total_pages: int = 1
    has_next_page: bool = False
    error: str | None = None
    queried_urls: list[str] = field(default_factory=list)
    query: str = ""
    cursor_key: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class ReleaseMatchResult:
    """Result of mapping a TorrentRelease against a canonical MediaIdentity and its seasons/episodes."""

    release: TorrentRelease
    canonical_id: str
    classification: str  # "episode" | "multi_episode" | "season_pack" | "complete_series" | "movie" | "special" | "unclassified" | "ambiguous"
    match_bucket: str = "confirmed"  # "confirmed" | "possible" | "unclassified" | "rejected"
    season_number: int | None = None
    episode_number: int | None = None
    first_episode: int | None = None
    last_episode: int | None = None
    batch_label: str = ""
    confidence: float = 0.0
    match_reason: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)
