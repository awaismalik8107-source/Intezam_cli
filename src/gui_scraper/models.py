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
    """Canonical show/anime identity separate from torrent release data."""

    canonical_id: str
    official_title: str
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
    synopsis: str = ""
    genres: list[str] = field(default_factory=list)
    status: str = ""
    poster_url: str = ""
    backdrop_url: str = ""
    seasons: list[SeasonMetadata] = field(default_factory=list)
    metadata_provider: str = ""
    confidence: float = 1.0

    def all_search_titles(self) -> list[str]:
        """Return deduplicated primary and alternate titles suitable for torrent queries."""
        candidates: list[str] = []
        seen: set[str] = set()
        for raw in [
            self.official_title,
            self.romaji_title,
            self.original_title,
            *self.alternate_titles,
        ]:
            cleaned = (raw or "").strip()
            if not cleaned or len(cleaned) < 2:
                continue
            # Skip purely non-Latin/CJK strings as primary query if Latin titles exist,
            # or include them after Latin titles.
            norm = cleaned.lower()
            if norm not in seen:
                seen.add(norm)
                candidates.append(cleaned)
        return candidates

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
    is_valid: bool = True
    verification_msg: str = ""
    mirror_providers: list[str] = field(default_factory=list)

    def to_scraped_item(self, numeric_id: int) -> ScrapedItem:
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
            category=self.category,
            info_hash=self.info_hash,
            resolution=self.resolution,
            codec=self.codec,
            subtitle_lang=self.subtitle_lang,
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

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class ReleaseMatchResult:
    """Result of mapping a TorrentRelease against a canonical MediaIdentity and its seasons/episodes."""

    release: TorrentRelease
    canonical_id: str
    classification: str  # "episode" | "multi_episode" | "season_pack" | "complete_series" | "special" | "unclassified" | "ambiguous"
    season_number: int | None = None
    episode_number: int | None = None
    first_episode: int | None = None
    last_episode: int | None = None
    batch_label: str = ""
    confidence: float = 0.0
    match_reason: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)
