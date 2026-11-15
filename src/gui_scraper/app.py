"""Standalone pywebview Desktop GUI application and Python <-> JS bridge."""

from __future__ import annotations

import shutil
import subprocess
import sys
import threading
import webbrowser
from collections import deque
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import pyperclip
import webview

if __package__ in (None, ""):
    # Allow running directly as `python src/gui_scraper/app.py`.
    _SRC_PARENT = Path(__file__).resolve().parents[2]
    if str(_SRC_PARENT) not in sys.path:
        sys.path.insert(0, str(_SRC_PARENT))
    from gui_scraper.engine import (
        BASE_URL,
        DATE_SELECTOR,
        DEFAULT_TIMEOUT,
        DEFAULT_USER_AGENT,
        LINK_SELECTOR,
        PROVIDER_NAME,
        QUERY_PARAM,
        ROW_SELECTOR,
        SEARCH_PATH,
        SIZE_SELECTOR,
        STATS_SELECTOR,
        TITLE_SELECTOR,
        ConfigurableTableScraper,
    )
    from gui_scraper.episode_resolver import EpisodeResolver, make_intezam_magnet_fetcher
    from gui_scraper.metadata import CatalogMetadataService
    from gui_scraper.models import EpisodeMetadata, MediaIdentity, SeasonMetadata, SourceSearchFilters
    from gui_scraper.providers import MultiProviderDiscoveryEngine, NyaaSourceProvider
    from gui_scraper.storage import LocalMediaStore
    from gui_scraper.torrent_manifest import TorrentManifestInspector
    from gui_scraper.validator import inspect_magnet_uri, verify_http_status
else:
    from .engine import (
        BASE_URL,
        DATE_SELECTOR,
        DEFAULT_TIMEOUT,
        DEFAULT_USER_AGENT,
        LINK_SELECTOR,
        PROVIDER_NAME,
        QUERY_PARAM,
        ROW_SELECTOR,
        SEARCH_PATH,
        SIZE_SELECTOR,
        STATS_SELECTOR,
        TITLE_SELECTOR,
        ConfigurableTableScraper,
    )
    from .episode_resolver import EpisodeResolver, make_intezam_magnet_fetcher
    from .metadata import CatalogMetadataService
    from .models import EpisodeMetadata, MediaIdentity, SeasonMetadata, SourceSearchFilters
    from .providers import MultiProviderDiscoveryEngine, NyaaSourceProvider
    from .storage import LocalMediaStore
    from .torrent_manifest import TorrentManifestInspector
    from .validator import inspect_magnet_uri, verify_http_status

UI_INDEX_PATH = Path(__file__).resolve().parent / "ui" / "dist" / "index.html"
_REPO_ROOT = Path(__file__).resolve().parents[2]


def _find_intezam_binary() -> Path | None:
    """Locate a compiled Intezam C++ binary without altering files outside ``src/gui_scraper``."""
    candidates = [
        _REPO_ROOT / "build" / "intezam",
        _REPO_ROOT / "build-cli" / "intezam",
        _REPO_ROOT / "intezam",
    ]
    for candidate in candidates:
        if candidate.is_file() and candidate.stat().st_mode & 0o111:
            return candidate
    which_path = shutil.which("intezam")
    return Path(which_path) if which_path else None


class ScraperBridge:
    """Thread-safe Python API bridge exposed to the pywebview frontend via ``js_api``."""

    def __init__(
        self,
        scraper: ConfigurableTableScraper | None = None,
        store: LocalMediaStore | None = None,
        metadata_service: CatalogMetadataService | None = None,
        discovery_engine: MultiProviderDiscoveryEngine | None = None,
        manifest_inspector: TorrentManifestInspector | None = None,
        episode_resolver: EpisodeResolver | None = None,
    ) -> None:
        self.scraper = scraper or ConfigurableTableScraper(demo_mode=True)
        self.store = store or LocalMediaStore()
        self.metadata_service = metadata_service or CatalogMetadataService()
        self.discovery_engine = discovery_engine or MultiProviderDiscoveryEngine(
            nyaa=NyaaSourceProvider(self.scraper)
        )
        self.manifest_inspector = manifest_inspector or TorrentManifestInspector(
            user_agent=self.scraper.user_agent,
            magnet_metadata_fetcher=make_intezam_magnet_fetcher(_find_intezam_binary),
        )
        self.episode_resolver = episode_resolver or EpisodeResolver(
            self.discovery_engine,
            self.manifest_inspector,
        )
        self._lock = threading.Lock()
        self._stream_lock = threading.Lock()
        self._logs: deque[str] = deque(maxlen=150)
        self._stream_proc: subprocess.Popen[str] | None = None
        self._stream_state: dict[str, Any] = {
            "active": False,
            "stage": "idle",
            "message": "Idle",
            "url": "",
            "torrent_name": "",
            "selected_video": "",
            "video_size": "",
            "detected_videos": 0,
            "strict_cache": False,
            "returncode": None,
            "lines": [],
        }
        self._log("Intezam bridge initialized.")

    def _log(self, message: str) -> None:
        stamp = datetime.now(timezone.utc).strftime("%H:%M:%S")
        self._logs.append(f"[{stamp}] {message}")

    def discover_shows(
        self,
        query: str = "",
        mode: str = "mock",
    ) -> dict[str, Any]:
        """Stage 9: Discover candidate shows from metadata providers (AniList + TMDB/TVMaze) without scraping torrents for every card."""
        normalized_mode = (mode or "mock").strip().lower()
        use_demo = normalized_mode in {"mock", "mock data", "demo"}
        mode_key = "mock" if use_demo else "live"
        cleaned = (query or "").strip()

        shows, notes, search_status = self.metadata_service.search_shows_detailed(
            cleaned,
            demo_mode=use_demo,
            limit=8,
        )
        if cleaned:
            try:
                self.store.record_search(cleaned, mode_key, len(shows))
            except Exception as exc:
                self._log(f"Non-fatal search history write warning: {exc}")
        self._log(
            f"Metadata discovery ({mode_key}, status={search_status}) for {cleaned!r} returned {len(shows)} show(s)."
        )
        return {
            "shows": [s.to_dict() for s in shows],
            "query": cleaned,
            "mode": mode_key,
            "search_status": search_status,
            "error": None,
            "provider_notes": notes,
        }

    def discover_show_sources(
        self,
        show: dict[str, Any] | str,
        mode: str = "mock",
        page: int = 1,
        force_refresh: bool = False,
        filters: dict[str, Any] | None = None,
        cursors: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Stage 5, 6 & 9: On-demand multi-provider torrent discovery, 4-bucket classification, and independent per-query pagination."""
        normalized_mode = (mode or "mock").strip().lower()
        use_demo = normalized_mode in {"mock", "mock data", "demo"}
        mode_key = "mock" if use_demo else "live"
        page_num = max(1, int(page or 1))

        identity = self._coerce_media_identity(show)
        search_filters = self._coerce_filters(filters, identity)

        items, statuses, has_next, total_pages = self.discovery_engine.discover_for_show(
            identity,
            page=page_num,
            demo_mode=use_demo,
            force_refresh=bool(force_refresh),
            filters=search_filters,
            cursors=cursors if isinstance(cursors, dict) else None,
        )
        serialized = [asdict(item) for item in items]
        pagination_cursors = dict(self.discovery_engine.last_pagination_cursors)
        rejected_count = len(self.discovery_engine.last_rejected_items)
        self._log(
            f"Show source discovery ({mode_key}, p={page_num}/{total_pages}) for {identity.official_title!r} "
            f"returned {len(serialized)} source(s) (rejected={rejected_count})."
        )
        return {
            "canonical_id": identity.canonical_id,
            "items": serialized,
            "page": page_num,
            "has_next_page": has_next,
            "total_pages": total_pages,
            "provider_statuses": [s.to_dict() for s in statuses],
            "pagination_cursors": pagination_cursors,
            "rejected_count": rejected_count,
            "error": None,
        }

    def resolve_episode(
        self,
        show: dict[str, Any] | str,
        season: int,
        episode: int,
        options: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Resolve an official episode to exact verified torrent file candidates."""
        identity = self._coerce_media_identity(show)
        s_num = int(season)
        ep_num = max(1, int(episode))
        result = self.episode_resolver.resolve_episode(
            identity,
            season=s_num,
            episode=ep_num,
            options=options if isinstance(options, dict) else None,
        )
        self._log(
            f"Episode resolve S{s_num:02d}E{ep_num:02d} for {identity.official_title!r} -> "
            f"{len(result.candidates)} verified candidate(s), {len(result.possible_matches)} possible, "
            f"{len(result.unresolved_candidates)} unresolved ({result.metrics.get('total_ms', 0):.0f}ms)."
        )
        return result.to_dict()

    def cancel_episode_resolution(self, request_id: str = "") -> dict[str, Any]:
        """Cancel an in-flight episode resolution request cooperatively."""
        cleaned_id = (request_id or "").strip()
        cancelled = self.episode_resolver.cancel(cleaned_id)
        return {
            "ok": True,
            "cancelled": cancelled,
            "request_id": cleaned_id,
        }

    def inspect_torrent_manifest(
        self,
        torrent_url: str = "",
        magnet_uri: str = "",
        info_hash: str = "",
        force_refresh: bool = False,
    ) -> dict[str, Any]:
        """Metadata-only .torrent manifest inspection without initiating downloads or peer connections."""
        result = self.manifest_inspector.inspect(
            torrent_url=torrent_url,
            magnet_uri=magnet_uri,
            info_hash=info_hash,
            force_refresh=bool(force_refresh),
        )
        self._log(
            f"Torrent manifest inspect (status={result.status}, cached={result.cached}, files={result.total_files}) "
            f"hash={result.info_hash[:12] or 'none'}."
        )
        return result.to_dict()

    @staticmethod
    def _coerce_media_identity(show: dict[str, Any] | str) -> MediaIdentity:
        if isinstance(show, str):
            return MediaIdentity(
                canonical_id=f"query:{show.strip().lower()}",
                official_title=show.strip(),
                romaji_title=show.strip(),
            )
        raw_seasons = show.get("seasons") or []
        seasons: list[SeasonMetadata] = []
        for s_raw in raw_seasons:
            if not isinstance(s_raw, dict):
                continue
            raw_s_num = s_raw.get("season_number")
            s_num = int(raw_s_num) if raw_s_num is not None else 1
            eps = [
                EpisodeMetadata(
                    episode_number=int(ep.get("episode_number") if ep.get("episode_number") is not None else 1),
                    season_number=int(ep.get("season_number") if ep.get("season_number") is not None else s_num),
                    title=str(ep.get("title") or ""),
                    air_date=str(ep.get("air_date") or ""),
                    overview=str(ep.get("overview") or ""),
                    still_url=str(ep.get("still_url") or ""),
                    absolute_episode=int(ep["absolute_episode"]) if ep.get("absolute_episode") is not None else None,
                    is_special=bool(ep.get("is_special", False)),
                )
                for ep in (s_raw.get("episodes") or [])
                if isinstance(ep, dict)
            ]
            seasons.append(
                SeasonMetadata(
                    season_number=s_num,
                    title=str(s_raw.get("title") or ""),
                    episode_count=int(s_raw.get("episode_count") if s_raw.get("episode_count") is not None else len(eps)),
                    air_year=int(s_raw["air_year"]) if s_raw.get("air_year") else None,
                    poster_url=str(s_raw.get("poster_url") or ""),
                    anilist_id=int(s_raw["anilist_id"]) if s_raw.get("anilist_id") else None,
                    season_aliases=[str(a) for a in (s_raw.get("season_aliases") or []) if a],
                    episodes=eps,
                )
            )
        official = str(show.get("official_title") or show.get("title") or "Unknown").strip()
        return MediaIdentity(
            canonical_id=str(show.get("canonical_id") or show.get("key") or f"show:{official.lower()}"),
            official_title=official,
            english_title=str(show.get("english_title") or ""),
            original_title=str(show.get("original_title") or ""),
            romaji_title=str(show.get("romaji_title") or ""),
            alternate_titles=[str(a) for a in (show.get("alternate_titles") or []) if a],
            tmdb_id=int(show["tmdb_id"]) if show.get("tmdb_id") else None,
            anilist_id=int(show["anilist_id"]) if show.get("anilist_id") else None,
            imdb_id=str(show["imdb_id"]) if show.get("imdb_id") else None,
            tvmaze_id=int(show["tvmaze_id"]) if show.get("tvmaze_id") else None,
            release_year=int(show["release_year"]) if show.get("release_year") else None,
            media_type=str(show.get("media_type") or "anime"),
            country=str(show.get("country") or ""),
            episode_count=int(show["episode_count"]) if show.get("episode_count") else None,
            synopsis=str(show.get("synopsis") or ""),
            genres=[str(g) for g in (show.get("genres") or []) if g],
            status=str(show.get("status") or ""),
            poster_url=str(show.get("poster_url") or ""),
            backdrop_url=str(show.get("backdrop_url") or ""),
            seasons=seasons,
            metadata_provider=str(show.get("metadata_provider") or ""),
            match_score=float(show.get("match_score") or 0.0),
            confidence_tier=str(show.get("confidence_tier") or "high"),
            match_reason=str(show.get("match_reason") or ""),
        )

    @staticmethod
    def _coerce_filters(
        filters: dict[str, Any] | None,
        identity: MediaIdentity | None = None,
    ) -> SourceSearchFilters:
        if not isinstance(filters, dict):
            return SourceSearchFilters(
                category="anime_all" if (identity is None or identity.media_type == "anime") else "all",
                imdb_id=identity.imdb_id if identity else None,
            )
        return SourceSearchFilters(
            category=str(filters.get("category") or ("anime_all" if (identity is None or identity.media_type == "anime") else "all")),
            filter_mode=str(filters.get("filter_mode") or "0"),
            sort_by=str(filters.get("sort_by") or "seeders"),
            sort_order=str(filters.get("sort_order") or "desc"),
            providers=[str(p) for p in (filters.get("providers") or []) if p],
            resolution=str(filters.get("resolution") or ""),
            imdb_id=str(filters.get("imdb_id") or (identity.imdb_id if identity else "") or "") or None,
        )

    def search_paginated(
        self,
        query: str = "",
        mode: str = "mock",
        page: int = 1,
        force_refresh: bool = False,
        filters: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Run a paginated Nyaa browse or search and return items + pagination metadata."""
        normalized_mode = (mode or "mock").strip().lower()
        use_demo = normalized_mode in {"mock", "mock data", "demo"}
        mode_key = "mock" if use_demo else "live"
        page_num = max(1, int(page or 1))
        cat_arg: str | None = None
        flt_arg: str | None = None
        if isinstance(filters, dict):
            from .providers import NYAA_CATEGORY_MAP

            raw_cat = str(filters.get("category") or "").strip()
            if raw_cat:
                cat_arg = NYAA_CATEGORY_MAP.get(raw_cat, raw_cat)
            raw_flt = str(filters.get("filter_mode") or "").strip()
            if raw_flt in {"0", "1", "2"}:
                flt_arg = raw_flt

        response = self.scraper.search(
            query,
            demo_mode=use_demo,
            page=page_num,
            force_refresh=bool(force_refresh),
            category=cat_arg,
            filter_mode=flt_arg,
        )

        if response.error:
            self._log(f"Search error ({mode_key}, p={page_num}) for {query!r}: {response.error}")
            raise RuntimeError(response.error)

        items = [asdict(item) for item in response.items]
        if page_num == 1:
            self.store.record_search(query, mode_key, len(items))
        self._log(
            f"Search ({mode_key}, p={page_num}/{response.total_pages}) for {query!r} returned {len(items)} source(s)."
        )
        return {
            "items": items,
            "page": response.page,
            "has_next_page": response.has_next_page,
            "total_pages": response.total_pages,
            "request_url": response.request_url,
        }

    def search(self, query: str = "", mode: str = "mock", page: int = 1) -> list[dict[str, Any]]:
        """Run a mock or live table scrape (or default browse when query is empty) and return serialized ``ScrapedItem`` dicts."""
        return self.search_paginated(query=query, mode=mode, page=page)["items"]

    def verify_url(self, url: str) -> dict[str, Any]:
        """Verify a Magnet URI's BTIH hash or send an HTTP HEAD check with a 5s timeout."""
        cleaned = (url or "").strip()
        if not cleaned:
            return {
                "url": "",
                "is_valid": False,
                "status": "invalid",
                "message": "Empty URI cannot be verified.",
            }

        if cleaned.lower().startswith("magnet:"):
            is_valid, message = inspect_magnet_uri(cleaned)
            self._log(f"Magnet verify -> {is_valid}: {message}")
            return {
                "url": cleaned,
                "is_valid": is_valid,
                "status": "valid" if is_valid else "invalid",
                "message": message,
            }

        with self._lock:
            headers = {"User-Agent": self.scraper.user_agent}

        is_valid, message = verify_http_status(cleaned, timeout=5.0, headers=headers)
        self._log(f"HTTP HEAD {cleaned} -> {is_valid}: {message}")
        return {
            "url": cleaned,
            "is_valid": is_valid,
            "status": "valid" if is_valid else "invalid",
            "message": message,
        }

    def get_config(self) -> dict[str, Any]:
        """Return the current scraper URL, selector, and HTTP configuration."""
        with self._lock:
            return {
                "provider_name": self.scraper.provider_name,
                "base_url": self.scraper.base_url,
                "search_path": self.scraper.search_path,
                "query_param": self.scraper.query_param,
                "row_selector": self.scraper.row_selector,
                "title_selector": self.scraper.title_selector,
                "link_selector": self.scraper.link_selector,
                "size_selector": self.scraper.size_selector,
                "date_selector": self.scraper.date_selector,
                "stats_selector": self.scraper.stats_selector,
                "timeout": self.scraper.timeout,
                "user_agent": self.scraper.user_agent,
            }

    def save_config(self, new_config: dict[str, Any]) -> dict[str, Any]:
        """Validate and apply updated target URL and CSS selector settings."""
        if not isinstance(new_config, dict):
            return {"ok": False, "error": "Configuration payload must be a dictionary."}

        base_url = str(new_config.get("base_url", self.scraper.base_url)).strip()
        row_selector = str(new_config.get("row_selector", self.scraper.row_selector)).strip()
        timeout_raw = new_config.get("timeout", self.scraper.timeout)

        if not base_url.startswith(("http://", "https://")):
            return {"ok": False, "error": "BASE_URL must start with http:// or https://"}
        if not row_selector:
            return {"ok": False, "error": "ROW_SELECTOR cannot be empty."}

        try:
            timeout_val = float(timeout_raw)
            if timeout_val <= 0:
                raise ValueError("Timeout must be greater than zero.")
        except (TypeError, ValueError):
            return {"ok": False, "error": "Timeout must be a positive number."}

        with self._lock:
            self.scraper.provider_name = (
                str(new_config.get("provider_name", self.scraper.provider_name)).strip()
                or PROVIDER_NAME
            )
            self.scraper.base_url = base_url
            self.scraper.search_path = (
                str(new_config.get("search_path", self.scraper.search_path)).strip() or "/"
            )
            self.scraper.query_param = str(
                new_config.get("query_param", self.scraper.query_param)
            ).strip()
            self.scraper.row_selector = row_selector
            self.scraper.title_selector = str(
                new_config.get("title_selector", self.scraper.title_selector)
            ).strip()
            self.scraper.link_selector = str(
                new_config.get("link_selector", self.scraper.link_selector)
            ).strip()
            self.scraper.size_selector = str(
                new_config.get("size_selector", self.scraper.size_selector)
            ).strip()
            self.scraper.date_selector = str(
                new_config.get("date_selector", self.scraper.date_selector)
            ).strip()
            self.scraper.stats_selector = str(
                new_config.get("stats_selector", self.scraper.stats_selector)
            ).strip()
            self.scraper.timeout = timeout_val
            self.scraper.user_agent = (
                str(new_config.get("user_agent", self.scraper.user_agent)).strip()
                or DEFAULT_USER_AGENT
            )
            self.scraper.clear_cache()

        self._log(f"Updated scraper config for {base_url}.")
        return {"ok": True, "config": self.get_config()}

    def reset_config(self) -> dict[str, Any]:
        """Restore the scraper configuration to engine defaults."""
        with self._lock:
            self.scraper.provider_name = PROVIDER_NAME
            self.scraper.base_url = BASE_URL
            self.scraper.search_path = SEARCH_PATH
            self.scraper.query_param = QUERY_PARAM
            self.scraper.row_selector = ROW_SELECTOR
            self.scraper.title_selector = TITLE_SELECTOR
            self.scraper.link_selector = LINK_SELECTOR
            self.scraper.size_selector = SIZE_SELECTOR
            self.scraper.date_selector = DATE_SELECTOR
            self.scraper.stats_selector = STATS_SELECTOR
            self.scraper.timeout = DEFAULT_TIMEOUT
            self.scraper.user_agent = DEFAULT_USER_AGENT
            self.scraper.clear_cache()

        self._log("Restored default scraper configuration.")
        return {"ok": True, "config": self.get_config()}

    def open_external(self, url: str) -> bool:
        """Open a validated HTTP(S) or Magnet URI in the OS default handler."""
        cleaned = (url or "").strip()
        if not cleaned.lower().startswith(("http://", "https://", "magnet:")):
            return False
        try:
            ok = bool(webbrowser.open(cleaned))
            self._log(f"External open ({ok}): {cleaned[:64]}")
            return ok
        except Exception as exc:
            self._log(f"External open failed: {exc}")
            return False

    def copy_to_clipboard(self, text: str) -> dict[str, Any]:
        """Copy text to the system clipboard via pyperclip or native Linux helpers."""
        if not text:
            return {"ok": False, "error": "Nothing to copy."}

        try:
            pyperclip.copy(text)
            return {"ok": True}
        except Exception:
            for cmd in (["wl-copy"], ["xclip", "-selection", "clipboard"], ["xsel", "-b", "-i"]):
                exe = shutil.which(cmd[0])
                if not exe:
                    continue
                try:
                    subprocess.run(
                        [exe, *cmd[1:]],
                        input=text,
                        text=True,
                        check=True,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE,
                    )
                    return {"ok": True}
                except subprocess.SubprocessError:
                    continue
            return {"ok": False, "error": "No system clipboard mechanism available."}

    # ------------------------------------------------------------------
    # Intezam Cinema: Local Library, History, Diagnostics & Engine Handoff
    # ------------------------------------------------------------------

    def get_library(self) -> list[dict[str, Any]]:
        return self.store.get_library()

    def toggle_library_item(self, item: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(item, dict):
            return {"ok": False, "error": "Invalid library item payload."}
        return self.store.toggle_library_item(item)

    def remove_library_item(self, key: str) -> dict[str, Any]:
        return self.store.remove_library_item(key)

    def get_search_history(self) -> list[dict[str, Any]]:
        return self.store.get_search_history()

    def clear_search_history(self) -> list[dict[str, Any]]:
        return self.store.clear_search_history()

    def get_watch_history(self) -> list[dict[str, Any]]:
        return self.store.get_watch_history()

    def record_watch_launch(self, entry: dict[str, Any]) -> list[dict[str, Any]]:
        if not isinstance(entry, dict):
            return self.store.get_watch_history()
        return self.store.record_watch_launch(entry)

    def clear_watch_history(self) -> list[dict[str, Any]]:
        return self.store.clear_watch_history()

    def get_engine_status(self) -> dict[str, Any]:
        binary = _find_intezam_binary()
        with self._stream_lock:
            active = bool(self._stream_state.get("active"))
        return {
            "intezam_binary_available": binary is not None,
            "intezam_binary_path": str(binary) if binary else "",
            "vlc_available": shutil.which("vlc") is not None,
            "mpv_available": shutil.which("mpv") is not None,
            "rolling_cache_policy": "2.5% target / 25% adaptive ceiling",
            "stream_active": active,
        }

    def launch_intezam_stream(
        self,
        url: str,
        title: str = "",
        strict_cache: bool = False,
        timeout_seconds: int = 120,
        torrent_file_index: int | None = None,
        expected_info_hash: str = "",
        expected_file_path: str = "",
        expected_file_size: int | None = None,
    ) -> dict[str, Any]:
        """Launch the standalone Intezam C++ + LibVLC player safely for a Magnet URI and explicit file target."""
        cleaned = (url or "").strip()
        if not cleaned.lower().startswith("magnet:"):
            return {
                "ok": False,
                "error": "Intezam C++ rolling-cache engine requires a valid magnet:? URI.",
            }

        is_valid, msg = inspect_magnet_uri(cleaned)
        if not is_valid:
            return {"ok": False, "error": msg}

        if torrent_file_index is not None:
            try:
                file_index_val = int(torrent_file_index)
            except (TypeError, ValueError):
                return {"ok": False, "error": "Invalid torrent file index."}
            if file_index_val < 0:
                return {"ok": False, "error": "Torrent file index must be non-negative."}
        else:
            file_index_val = None

        norm_hash = (expected_info_hash or "").strip().lower()
        if norm_hash and not all(c in "0123456789abcdef" for c in norm_hash):
            return {"ok": False, "error": "Invalid expected info hash format."}

        if expected_file_size is not None:
            try:
                expected_size_val = int(expected_file_size)
            except (TypeError, ValueError):
                return {"ok": False, "error": "Invalid expected file size."}
            if expected_size_val < 0:
                return {"ok": False, "error": "Expected file size must be non-negative."}
        else:
            expected_size_val = None

        binary = _find_intezam_binary()
        if binary is None:
            return {
                "ok": False,
                "error": "Compiled Intezam engine binary not found. Use Open External instead.",
            }

        self.stop_stream()

        timeout_clamped = max(10, min(600, int(timeout_seconds)))
        cmd = [str(binary), "--timeout", str(timeout_clamped)]
        if strict_cache:
            cmd.append("--strict-cache")
        if file_index_val is not None:
            cmd.extend(["--file-index", str(file_index_val)])
            if norm_hash:
                cmd.extend(["--expected-info-hash", norm_hash])
            cleaned_path = (expected_file_path or "").replace("\\", "/").strip()
            if cleaned_path:
                cmd.extend(["--expected-file-path", cleaned_path])
            if expected_size_val is not None and expected_size_val > 0:
                cmd.extend(["--expected-file-size", str(expected_size_val)])
        else:
            cmd.append("--select-single")

        try:
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                cwd=str(_REPO_ROOT),
            )
        except OSError as exc:
            self._log(f"Failed to start Intezam engine: {exc}")
            return {"ok": False, "error": f"Failed to start Intezam engine: {exc}"}

        if proc.stdin is not None:
            try:
                # Pass only the Magnet URI via stdin; exact file targeting is supplied explicitly via CLI flags.
                proc.stdin.write(f"{cleaned}\n")
                proc.stdin.flush()
                proc.stdin.close()
            except OSError:
                pass

        with self._stream_lock:
            self._stream_proc = proc
            self._stream_state = {
                "active": True,
                "stage": "connecting",
                "message": "Connecting to peers and retrieving torrent metadata...",
                "url": cleaned,
                "torrent_name": title,
                "selected_video": "",
                "video_size": "",
                "detected_videos": 0,
                "strict_cache": strict_cache,
                "returncode": None,
                "lines": [],
            }

        self.store.record_watch_launch(
            {
                "key": cleaned,
                "title": title or "Magnet Stream",
                "resolved_link": cleaned,
                "engine": "intezam-cpp",
            }
        )
        self._log(f"Launched Intezam engine ({binary.name}) for magnet stream.")

        def _monitor_process() -> None:
            if proc.stdout is not None:
                for raw_line in proc.stdout:
                    line = raw_line.strip()
                    if not line:
                        continue
                    with self._stream_lock:
                        lines = list(self._stream_state.get("lines", []))
                        lines.append(line)
                        self._stream_state["lines"] = lines[-30:]

                        if line.startswith("Retrieving metadata"):
                            self._stream_state["stage"] = "metadata"
                            self._stream_state["message"] = line
                        elif line.startswith("Torrent:"):
                            self._stream_state["torrent_name"] = line.split(":", 1)[1].strip()
                            self._stream_state["stage"] = "selecting"
                            self._stream_state["message"] = "Inspecting playable video streams..."
                        elif line.startswith("Detected Videos:"):
                            try:
                                self._stream_state["detected_videos"] = int(
                                    line.split(":", 1)[1].strip()
                                )
                            except ValueError:
                                pass
                        elif line.startswith("Selected:"):
                            self._stream_state["selected_video"] = line.split(":", 1)[1].strip()
                            self._stream_state["stage"] = "buffering"
                            self._stream_state["message"] = (
                                "Preparing 2.5% rolling cache & LibVLC playback..."
                            )
                        elif line.startswith("Size:"):
                            self._stream_state["video_size"] = line.split(":", 1)[1].strip()
                        elif "playing" in line.lower() or "playback" in line.lower():
                            self._stream_state["stage"] = "playing"
                            self._stream_state["message"] = line
                        elif line.startswith("Error:"):
                            self._stream_state["stage"] = "error"
                            self._stream_state["message"] = line

            rc = proc.wait()
            with self._stream_lock:
                self._stream_state["active"] = False
                self._stream_state["returncode"] = rc
                if rc != 0 and self._stream_state["stage"] != "error":
                    self._stream_state["stage"] = "error"
                    last_lines = self._stream_state.get("lines") or []
                    self._stream_state["message"] = (
                        last_lines[-1] if last_lines else f"Engine exited with code {rc}."
                    )
                elif rc == 0 and self._stream_state["stage"] not in {"error", "stopped"}:
                    self._stream_state["stage"] = "completed"
                    self._stream_state["message"] = "Playback session ended."
                if self._stream_proc is proc:
                    self._stream_proc = None

        threading.Thread(target=_monitor_process, daemon=True).start()
        return {"ok": True, "state": self.get_stream_status()}

    def get_stream_status(self) -> dict[str, Any]:
        with self._stream_lock:
            return dict(self._stream_state)

    def stop_stream(self) -> dict[str, Any]:
        with self._stream_lock:
            proc = self._stream_proc
            if proc is not None and proc.poll() is None:
                try:
                    proc.terminate()
                except OSError:
                    pass
                self._stream_state["active"] = False
                self._stream_state["stage"] = "stopped"
                self._stream_state["message"] = "Playback session stopped."
                self._stream_proc = None
            return dict(self._stream_state)

    def get_app_logs(self) -> list[str]:
        return list(self._logs)


def main() -> None:
    """Create the pywebview desktop window and start the GUI event loop."""
    if not UI_INDEX_PATH.is_file():
        raise FileNotFoundError(f"Frontend entrypoint not found: {UI_INDEX_PATH}")

    bridge = ScraperBridge()
    webview.create_window(
        title="INTEZAM",
        url=UI_INDEX_PATH.as_uri(),
        js_api=bridge,
        width=1440,
        height=900,
        min_size=(1000, 650),
        background_color="#090B12",
    )
    webview.start(debug=False)


if __name__ == "__main__":
    main()
