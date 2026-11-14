"""Metadata providers (AniList GraphQL, TMDB v3 REST, and TVMaze public REST) for Intezam."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import html
import os
import re
import threading
import time
from typing import Any

import httpx

from .models import EpisodeMetadata, MediaIdentity, SeasonMetadata

ANILIST_GRAPHQL_URL = "https://graphql.anilist.co"
TMDB_API_BASE_URL = "https://api.themoviedb.org/3"
TMDB_IMAGE_BASE_URL = "https://image.tmdb.org/t/p"
TVMAZE_API_BASE_URL = "https://api.tvmaze.com"

_HTML_TAG_RE = re.compile(r"<[^>]+>")
_EP_STREAM_TITLE_RE = re.compile(
    r"^(?:Episode|Ep\.?)\s*(\d{1,4})(?:\s*[-–—:]\s*(.*))?$",
    re.IGNORECASE,
)
_SEASON_NUMBER_SUFFIX_RE = re.compile(
    r"(?:\bSeason\s*(\d{1,2})\b|\b(\d{1,2})(?:st|nd|rd|th)\s+Season\b|\bPart\s*(\d{1,2})\b|\bCour\s*(\d{1,2})\b|\bS(\d{1,2})\b)",
    re.IGNORECASE,
)


def strip_html_tags(text: str | None) -> str:
    """Remove HTML markup and unescape entities from provider descriptions."""
    if not text:
        return ""
    cleaned = _HTML_TAG_RE.sub(" ", text)
    cleaned = html.unescape(cleaned)
    return re.sub(r"\s{2,}", " ", cleaned).strip()


def _norm_key(title: str) -> str:
    cleaned = re.sub(r"[^\w\s]", " ", (title or "").lower())
    return re.sub(r"\s+", " ", cleaned).strip()


def _strip_season_suffix(title: str) -> str:
    cleaned = re.sub(
        r"\s*(?:[-–—:]\s*)?(?:Season\s*\d{1,2}|\d{1,2}(?:st|nd|rd|th)\s+Season|S\d{1,2})\s*$",
        "",
        title or "",
        flags=re.IGNORECASE,
    ).strip()
    return cleaned or (title or "").strip()


ANILIST_SEARCH_QUERY = """
query ($search: String, $perPage: Int) {
  Page(page: 1, perPage: $perPage) {
    media(search: $search, type: ANIME, sort: [SEARCH_MATCH, POPULARITY_DESC]) {
      id
      idMal
      format
      status
      seasonYear
      startDate { year }
      episodes
      description(asHtml: false)
      genres
      synonyms
      title {
        romaji
        english
        native
      }
      coverImage {
        extraLarge
        large
      }
      bannerImage
      streamingEpisodes {
        title
        thumbnail
      }
      relations {
        edges {
          relationType(version: 2)
          node {
            id
            idMal
            type
            format
            status
            seasonYear
            startDate { year }
            episodes
            synonyms
            title {
              romaji
              english
              native
            }
            coverImage {
              extraLarge
              large
            }
          }
        }
      }
    }
  }
}
"""

ANILIST_POPULAR_QUERY = """
query ($perPage: Int) {
  Page(page: 1, perPage: $perPage) {
    media(type: ANIME, sort: [TRENDING_DESC, POPULARITY_DESC], format_in: [TV, ONA]) {
      id
      idMal
      format
      status
      seasonYear
      startDate { year }
      episodes
      description(asHtml: false)
      genres
      synonyms
      title {
        romaji
        english
        native
      }
      coverImage {
        extraLarge
        large
      }
      bannerImage
      streamingEpisodes {
        title
        thumbnail
      }
      relations {
        edges {
          relationType(version: 2)
          node {
            id
            idMal
            type
            format
            status
            seasonYear
            startDate { year }
            episodes
            synonyms
            title {
              romaji
              english
              native
            }
            coverImage {
              extraLarge
              large
            }
          }
        }
      }
    }
  }
}
"""


class AniListMetadataProvider:
    """Official AniList GraphQL v2 metadata provider for anime titles, posters, and sequel chains."""

    provider_name = "AniList"

    def __init__(
        self,
        graphql_url: str = ANILIST_GRAPHQL_URL,
        timeout: float = 8.0,
        client: httpx.Client | None = None,
    ) -> None:
        self.graphql_url = graphql_url
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
                    "Content-Type": "application/json",
                    "User-Agent": "IntezamAnimeBrowser/2.0",
                },
            )
        return self._shared_client

    def search(self, query: str = "", limit: int = 8) -> list[MediaIdentity]:
        """Search AniList for anime series or fetch trending anime when query is empty."""
        cleaned = (query or "").strip()
        gql = ANILIST_SEARCH_QUERY if cleaned else ANILIST_POPULAR_QUERY
        variables: dict[str, Any] = {"perPage": max(1, min(20, limit))}
        if cleaned:
            variables["search"] = cleaned

        resp = self._get_client().post(
            self.graphql_url,
            json={"query": gql, "variables": variables},
            timeout=self.timeout,
        )
        resp.raise_for_status()
        payload = resp.json()
        media_nodes = (
            payload.get("data", {})
            .get("Page", {})
            .get("media", [])
        ) or []
        return self.normalize_media_list(media_nodes)

    @classmethod
    def normalize_media_list(cls, media_nodes: list[dict[str, Any]]) -> list[MediaIdentity]:
        """Convert raw AniList media nodes into canonical MediaIdentity records, merging sequel/prequel seasons."""
        results: list[MediaIdentity] = []
        visited_ids: set[int] = set()

        for node in media_nodes:
            if not isinstance(node, dict):
                continue
            node_id = node.get("id")
            if not isinstance(node_id, int) or node_id in visited_ids:
                continue

            chain = cls._collect_season_chain(node)
            for member in chain:
                mid = member.get("id")
                if isinstance(mid, int):
                    visited_ids.add(mid)

            identity = cls._build_identity_from_chain(node, chain)
            results.append(identity)

        return results

    @staticmethod
    def _collect_season_chain(primary: dict[str, Any]) -> list[dict[str, Any]]:
        """Assemble PREQUEL + primary + SEQUEL TV entries into a chronological season list."""
        prequels: list[dict[str, Any]] = []
        sequels: list[dict[str, Any]] = []
        edges = (primary.get("relations") or {}).get("edges") or []
        for edge in edges:
            if not isinstance(edge, dict):
                continue
            rel_type = (edge.get("relationType") or "").upper()
            rel_node = edge.get("node") or {}
            if not isinstance(rel_node, dict):
                continue
            if (rel_node.get("type") or "ANIME").upper() != "ANIME":
                continue
            fmt = (rel_node.get("format") or "").upper()
            if fmt not in {"TV", "TV_SHORT", "ONA"}:
                continue
            if rel_type == "PREQUEL":
                prequels.append(rel_node)
            elif rel_type == "SEQUEL":
                sequels.append(rel_node)

        chain = [*prequels, primary, *sequels]
        deduped: list[dict[str, Any]] = []
        seen: set[int] = set()
        for item in chain:
            iid = item.get("id")
            if isinstance(iid, int) and iid not in seen:
                seen.add(iid)
                deduped.append(item)

        def _sort_key(entry: dict[str, Any]) -> tuple[int, int]:
            year = (
                entry.get("seasonYear")
                or (entry.get("startDate") or {}).get("year")
                or 9999
            )
            return (int(year), int(entry.get("id") or 0))

        deduped.sort(key=_sort_key)
        return deduped or [primary]

    @classmethod
    def _build_identity_from_chain(
        cls,
        primary: dict[str, Any],
        chain: list[dict[str, Any]],
    ) -> MediaIdentity:
        root = chain[0] if chain else primary
        root_titles = root.get("title") or {}
        primary_titles = primary.get("title") or {}

        eng = (root_titles.get("english") or primary_titles.get("english") or "").strip()
        rom = (root_titles.get("romaji") or primary_titles.get("romaji") or "").strip()
        nat = (root_titles.get("native") or primary_titles.get("native") or "").strip()

        official_title = _strip_season_suffix(eng or rom or nat or "Unknown Anime")
        romaji_title = _strip_season_suffix(rom or eng)

        alt_titles: list[str] = []
        seen_alts: set[str] = {_norm_key(official_title)}
        for member in chain:
            t_obj = member.get("title") or {}
            for candidate in [
                t_obj.get("english"),
                t_obj.get("romaji"),
                t_obj.get("native"),
                _strip_season_suffix(t_obj.get("english") or ""),
                _strip_season_suffix(t_obj.get("romaji") or ""),
                *(member.get("synonyms") or []),
            ]:
                if not candidate or not isinstance(candidate, str):
                    continue
                cleaned = candidate.strip()
                nk = _norm_key(cleaned)
                if cleaned and nk and nk not in seen_alts:
                    seen_alts.add(nk)
                    alt_titles.append(cleaned)

        seasons: list[SeasonMetadata] = []
        for idx, member in enumerate(chain, start=1):
            m_titles = member.get("title") or {}
            m_eng = (m_titles.get("english") or "").strip()
            m_rom = (m_titles.get("romaji") or "").strip()
            ep_count = int(member.get("episodes") or 0)
            stream_eps = member.get("streamingEpisodes") or []
            if ep_count <= 0 and stream_eps:
                ep_count = len(stream_eps)
            if ep_count <= 0:
                status = (member.get("status") or "").upper()
                ep_count = 12 if status in {"RELEASING", "FINISHED"} else 0

            ep_titles_by_num: dict[int, tuple[str, str]] = {}
            for s_ep in stream_eps:
                if not isinstance(s_ep, dict):
                    continue
                raw_t = (s_ep.get("title") or "").strip()
                thumb = (s_ep.get("thumbnail") or "").strip()
                m = _EP_STREAM_TITLE_RE.match(raw_t)
                if m:
                    ep_n = int(m.group(1))
                    ep_name = (m.group(2) or f"Episode {ep_n}").strip()
                    ep_titles_by_num[ep_n] = (ep_name, thumb)

            ep_list: list[EpisodeMetadata] = []
            for ep_num in range(1, max(ep_count, len(ep_titles_by_num)) + 1):
                ep_name, thumb = ep_titles_by_num.get(ep_num, (f"Episode {ep_num}", ""))
                ep_list.append(
                    EpisodeMetadata(
                        episode_number=ep_num,
                        season_number=idx,
                        title=ep_name,
                        still_url=thumb,
                    )
                )

            s_aliases = [
                a
                for a in [m_eng, m_rom, *(member.get("synonyms") or [])]
                if isinstance(a, str) and a.strip()
            ]
            s_cover = (member.get("coverImage") or {})
            s_poster = (s_cover.get("extraLarge") or s_cover.get("large") or "").strip()
            s_year = member.get("seasonYear") or (member.get("startDate") or {}).get("year")
            seasons.append(
                SeasonMetadata(
                    season_number=idx,
                    title=f"Season {idx}" if len(chain) > 1 else (m_eng or m_rom or "Season 1"),
                    episode_count=len(ep_list),
                    air_year=int(s_year) if s_year else None,
                    poster_url=s_poster,
                    anilist_id=int(member.get("id")) if member.get("id") else None,
                    season_aliases=s_aliases,
                    episodes=ep_list,
                )
            )

        cover = (primary.get("coverImage") or root.get("coverImage") or {})
        poster_url = (cover.get("extraLarge") or cover.get("large") or "").strip()
        backdrop_url = (primary.get("bannerImage") or root.get("bannerImage") or poster_url).strip()
        release_year = (
            root.get("seasonYear")
            or (root.get("startDate") or {}).get("year")
            or primary.get("seasonYear")
        )
        root_id = int(root.get("id") or primary.get("id") or 0)

        return MediaIdentity(
            canonical_id=f"anilist:{root_id}",
            official_title=official_title,
            original_title=nat,
            romaji_title=romaji_title,
            alternate_titles=alt_titles,
            anilist_id=root_id,
            mal_id=int(root.get("idMal")) if root.get("idMal") else None,
            release_year=int(release_year) if release_year else None,
            media_type="anime",
            synopsis=strip_html_tags(primary.get("description") or root.get("description")),
            genres=[str(g) for g in (primary.get("genres") or root.get("genres") or []) if g],
            status=str(primary.get("status") or root.get("status") or ""),
            poster_url=poster_url,
            backdrop_url=backdrop_url,
            seasons=seasons,
            metadata_provider="AniList",
            confidence=1.0,
        )


class TMDBMetadataProvider:
    """Official TMDB v3 REST metadata provider for TV series and movies with optional IMDb cross-reference."""

    provider_name = "TMDB"

    def __init__(
        self,
        api_key: str | None = None,
        bearer_token: str | None = None,
        base_url: str = TMDB_API_BASE_URL,
        timeout: float = 8.0,
        client: httpx.Client | None = None,
    ) -> None:
        self.api_key = (api_key if api_key is not None else os.environ.get("TMDB_API_KEY", "")).strip()
        self.bearer_token = (
            bearer_token if bearer_token is not None else os.environ.get("TMDB_BEARER_TOKEN", "")
        ).strip()
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self._client = client
        self._shared_client: httpx.Client | None = None

    @property
    def is_configured(self) -> bool:
        return bool(self.api_key or self.bearer_token)

    def _get_client(self) -> httpx.Client:
        if self._client is not None:
            return self._client
        if self._shared_client is None or self._shared_client.is_closed:
            headers = {
                "Accept": "application/json",
                "User-Agent": "IntezamMediaBrowser/2.0",
            }
            if self.bearer_token:
                headers["Authorization"] = f"Bearer {self.bearer_token}"
            self._shared_client = httpx.Client(
                timeout=self.timeout,
                follow_redirects=True,
                headers=headers,
            )
        return self._shared_client

    def _request_json(self, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if not self.is_configured:
            raise RuntimeError("TMDB_API_KEY or TMDB_BEARER_TOKEN is not configured.")
        query_params = dict(params or {})
        if self.api_key and not self.bearer_token:
            query_params["api_key"] = self.api_key
        url = f"{self.base_url}/{path.lstrip('/')}"
        resp = self._get_client().get(url, params=query_params, timeout=self.timeout)
        resp.raise_for_status()
        return resp.json()

    def search(self, query: str = "", limit: int = 6) -> list[MediaIdentity]:
        """Search TMDB TV series and hydrate top matches with season/external_ids metadata."""
        if not self.is_configured:
            return []
        cleaned = (query or "").strip()
        if cleaned:
            data = self._request_json("/search/tv", {"query": cleaned, "include_adult": "false"})
        else:
            data = self._request_json("/trending/tv/week")

        raw_results = (data.get("results") or [])[: max(1, min(10, limit))]
        identities: list[MediaIdentity] = []
        for summary in raw_results:
            if not isinstance(summary, dict) or not summary.get("id"):
                continue
            tv_id = int(summary["id"])
            try:
                details = self._request_json(
                    f"/tv/{tv_id}",
                    {"append_to_response": "external_ids,alternative_titles"},
                )
                season_payloads: dict[int, dict[str, Any]] = {}
                for s_info in (details.get("seasons") or [])[:6]:
                    s_num = int(s_info.get("season_number") or 0)
                    if s_num <= 0:
                        continue
                    try:
                        season_payloads[s_num] = self._request_json(f"/tv/{tv_id}/season/{s_num}")
                    except Exception:
                        pass
                identities.append(self.normalize_tv_payload(details, season_payloads))
            except Exception:
                identities.append(self.normalize_tv_payload(summary, {}))
        return identities

    @classmethod
    def normalize_tv_payload(
        cls,
        details: dict[str, Any],
        season_payloads: dict[int, dict[str, Any]] | None = None,
    ) -> MediaIdentity:
        """Convert a TMDB TV payload (and optional season details) into a canonical MediaIdentity."""
        season_payloads = season_payloads or {}
        tmdb_id = int(details.get("id") or 0)
        name = (details.get("name") or details.get("original_name") or "Unknown Series").strip()
        original_name = (details.get("original_name") or "").strip()
        first_air = (details.get("first_air_date") or "").strip()
        release_year = int(first_air[:4]) if len(first_air) >= 4 and first_air[:4].isdigit() else None

        alt_titles: list[str] = []
        seen_alts: set[str] = {_norm_key(name)}
        alt_container = details.get("alternative_titles") or {}
        for item in (alt_container.get("results") or []):
            if isinstance(item, dict) and item.get("title"):
                cand = str(item["title"]).strip()
                nk = _norm_key(cand)
                if cand and nk and nk not in seen_alts:
                    seen_alts.add(nk)
                    alt_titles.append(cand)
        if original_name and _norm_key(original_name) not in seen_alts:
            alt_titles.append(original_name)

        poster_path = (details.get("poster_path") or "").strip()
        backdrop_path = (details.get("backdrop_path") or "").strip()
        poster_url = f"{TMDB_IMAGE_BASE_URL}/w500{poster_path}" if poster_path else ""
        backdrop_url = f"{TMDB_IMAGE_BASE_URL}/w1280{backdrop_path}" if backdrop_path else poster_url

        ext_ids = details.get("external_ids") or {}
        imdb_id = (ext_ids.get("imdb_id") or "").strip() or None

        genres: list[str] = []
        for g in details.get("genres") or []:
            if isinstance(g, dict) and g.get("name"):
                genres.append(str(g["name"]))

        is_anime = "Animation" in genres and (
            "JP" in (details.get("origin_country") or [])
            or details.get("original_language") == "ja"
        )

        seasons: list[SeasonMetadata] = []
        for s_raw in details.get("seasons") or []:
            if not isinstance(s_raw, dict):
                continue
            s_num = int(s_raw.get("season_number") or 0)
            if s_num <= 0:
                continue
            ep_count = int(s_raw.get("episode_count") or 0)
            s_air = (s_raw.get("air_date") or "").strip()
            s_year = int(s_air[:4]) if len(s_air) >= 4 and s_air[:4].isdigit() else None
            s_poster_path = (s_raw.get("poster_path") or "").strip()
            s_poster = f"{TMDB_IMAGE_BASE_URL}/w500{s_poster_path}" if s_poster_path else poster_url

            s_detail = season_payloads.get(s_num) or {}
            raw_eps = s_detail.get("episodes") or []
            episodes: list[EpisodeMetadata] = []
            if raw_eps:
                for ep_raw in raw_eps:
                    if not isinstance(ep_raw, dict):
                        continue
                    ep_num = int(ep_raw.get("episode_number") or 0)
                    if ep_num <= 0:
                        continue
                    still_path = (ep_raw.get("still_path") or "").strip()
                    episodes.append(
                        EpisodeMetadata(
                            episode_number=ep_num,
                            season_number=s_num,
                            title=str(ep_raw.get("name") or f"Episode {ep_num}"),
                            air_date=str(ep_raw.get("air_date") or ""),
                            overview=strip_html_tags(ep_raw.get("overview")),
                            still_url=f"{TMDB_IMAGE_BASE_URL}/w500{still_path}" if still_path else "",
                        )
                    )
            else:
                for ep_num in range(1, ep_count + 1):
                    episodes.append(
                        EpisodeMetadata(
                            episode_number=ep_num,
                            season_number=s_num,
                            title=f"Episode {ep_num}",
                        )
                    )

            seasons.append(
                SeasonMetadata(
                    season_number=s_num,
                    title=str(s_raw.get("name") or f"Season {s_num}"),
                    episode_count=len(episodes) or ep_count,
                    air_year=s_year,
                    poster_url=s_poster,
                    season_aliases=[f"{name} Season {s_num}", f"{name} S{s_num:02d}"],
                    episodes=episodes,
                )
            )

        return MediaIdentity(
            canonical_id=f"tmdb:tv:{tmdb_id}",
            official_title=name,
            original_title=original_name,
            romaji_title=original_name if is_anime else "",
            alternate_titles=alt_titles,
            tmdb_id=tmdb_id,
            imdb_id=imdb_id,
            release_year=release_year,
            media_type="anime" if is_anime else "tv",
            synopsis=strip_html_tags(details.get("overview")),
            genres=genres,
            status=str(details.get("status") or ""),
            poster_url=poster_url,
            backdrop_url=backdrop_url,
            seasons=seasons,
            metadata_provider="TMDB",
            confidence=1.0,
        )


class TVMazeMetadataProvider:
    """Authorized keyless public REST metadata adapter (api.tvmaze.com, CC-BY-SA) for TV series & IMDb IDs."""

    provider_name = "TVMaze"

    def __init__(
        self,
        base_url: str = TVMAZE_API_BASE_URL,
        timeout: float = 8.0,
        client: httpx.Client | None = None,
    ) -> None:
        self.base_url = base_url.rstrip("/")
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
                    "User-Agent": "IntezamMediaBrowser/2.0",
                },
            )
        return self._shared_client

    def search(self, query: str, limit: int = 4) -> list[MediaIdentity]:
        cleaned = (query or "").strip()
        if not cleaned:
            return []
        client = self._get_client()
        resp = client.get(
            f"{self.base_url}/search/shows",
            params={"q": cleaned},
            timeout=self.timeout,
        )
        resp.raise_for_status()
        hits = resp.json() or []
        results: list[MediaIdentity] = []
        for entry in hits[: max(1, min(6, limit))]:
            show = (entry or {}).get("show") if isinstance(entry, dict) else None
            if not isinstance(show, dict) or not show.get("id"):
                continue
            show_id = int(show["id"])
            try:
                detail_resp = client.get(
                    f"{self.base_url}/shows/{show_id}",
                    params={"embed[]": ["seasons", "episodes", "akas"]},
                    timeout=self.timeout,
                )
                detail_resp.raise_for_status()
                results.append(self.normalize_show_payload(detail_resp.json()))
            except Exception:
                results.append(self.normalize_show_payload(show))
        return results

    @classmethod
    def normalize_show_payload(cls, show: dict[str, Any]) -> MediaIdentity:
        tvmaze_id = int(show.get("id") or 0)
        name = str(show.get("name") or "Unknown Show").strip()
        premiered = str(show.get("premiered") or "").strip()
        release_year = int(premiered[:4]) if len(premiered) >= 4 and premiered[:4].isdigit() else None

        externals = show.get("externals") or {}
        imdb_id = (externals.get("imdb") or "").strip() or None

        image_obj = show.get("image") or {}
        poster_url = str(image_obj.get("original") or image_obj.get("medium") or "").strip()

        embedded = show.get("_embedded") or {}
        alt_titles: list[str] = []
        seen_alts: set[str] = {_norm_key(name)}
        for aka in embedded.get("akas") or []:
            if isinstance(aka, dict) and aka.get("name"):
                cand = str(aka["name"]).strip()
                nk = _norm_key(cand)
                if cand and nk and nk not in seen_alts:
                    seen_alts.add(nk)
                    alt_titles.append(cand)

        eps_by_season: dict[int, list[EpisodeMetadata]] = {}
        for ep in embedded.get("episodes") or []:
            if not isinstance(ep, dict):
                continue
            s_num = int(ep.get("season") or 0)
            ep_num = int(ep.get("number") or 0)
            if s_num <= 0 or ep_num <= 0:
                continue
            ep_img = (ep.get("image") or {}) if isinstance(ep.get("image"), dict) else {}
            eps_by_season.setdefault(s_num, []).append(
                EpisodeMetadata(
                    episode_number=ep_num,
                    season_number=s_num,
                    title=str(ep.get("name") or f"Episode {ep_num}"),
                    air_date=str(ep.get("airdate") or ""),
                    overview=strip_html_tags(ep.get("summary")),
                    still_url=str(ep_img.get("original") or ep_img.get("medium") or ""),
                )
            )

        seasons: list[SeasonMetadata] = []
        raw_seasons = embedded.get("seasons") or []
        if raw_seasons:
            for s_raw in raw_seasons:
                if not isinstance(s_raw, dict):
                    continue
                s_num = int(s_raw.get("number") or 0)
                if s_num <= 0:
                    continue
                s_eps = sorted(eps_by_season.get(s_num, []), key=lambda e: e.episode_number)
                ep_order = int(s_raw.get("episodeOrder") or len(s_eps) or 0)
                if not s_eps and ep_order > 0:
                    s_eps = [
                        EpisodeMetadata(episode_number=i, season_number=s_num, title=f"Episode {i}")
                        for i in range(1, ep_order + 1)
                    ]
                s_prem = str(s_raw.get("premiereDate") or "").strip()
                s_year = int(s_prem[:4]) if len(s_prem) >= 4 and s_prem[:4].isdigit() else None
                s_img = (s_raw.get("image") or {}) if isinstance(s_raw.get("image"), dict) else {}
                seasons.append(
                    SeasonMetadata(
                        season_number=s_num,
                        title=str(s_raw.get("name") or f"Season {s_num}"),
                        episode_count=len(s_eps) or ep_order,
                        air_year=s_year,
                        poster_url=str(s_img.get("original") or s_img.get("medium") or poster_url),
                        season_aliases=[f"{name} Season {s_num}", f"{name} S{s_num:02d}"],
                        episodes=s_eps,
                    )
                )
        else:
            for s_num, s_eps in sorted(eps_by_season.items()):
                seasons.append(
                    SeasonMetadata(
                        season_number=s_num,
                        title=f"Season {s_num}",
                        episode_count=len(s_eps),
                        poster_url=poster_url,
                        episodes=sorted(s_eps, key=lambda e: e.episode_number),
                    )
                )

        genres = [str(g) for g in (show.get("genres") or []) if g]
        is_anime = "Anime" in genres or str(show.get("type") or "").lower() == "animation"

        return MediaIdentity(
            canonical_id=f"tvmaze:{tvmaze_id}",
            official_title=name,
            original_title=alt_titles[0] if alt_titles else "",
            romaji_title="",
            alternate_titles=alt_titles,
            tvmaze_id=tvmaze_id,
            imdb_id=imdb_id,
            release_year=release_year,
            media_type="anime" if is_anime else "tv",
            synopsis=strip_html_tags(show.get("summary")),
            genres=genres,
            status=str(show.get("status") or ""),
            poster_url=poster_url,
            backdrop_url=poster_url,
            seasons=seasons,
            metadata_provider="TVMaze",
            confidence=0.95,
        )


def get_demo_metadata_catalog(query: str = "") -> list[MediaIdentity]:
    """Deterministic canonical metadata catalog for offline/mock testing."""
    catalog: list[MediaIdentity] = [
        MediaIdentity(
            canonical_id="anilist:154587",
            official_title="Frieren: Beyond Journey's End",
            original_title="葬送のフリーレン",
            romaji_title="Sousou no Frieren",
            alternate_titles=["Frieren", "Sousou no Frieren", "Frieren Beyond Journeys End"],
            anilist_id=154587,
            tmdb_id=209867,
            imdb_id="tt22248376",
            release_year=2023,
            media_type="anime",
            synopsis="After a ten-year quest to defeat the Demon King alongside her party, elven mage Frieren embarks on a new journey to understand humanity and the passage of time.",
            genres=["Adventure", "Drama", "Fantasy"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx154587-gHSraOSa0nBG.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/154587-ivXNJ23SM1xB.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=28,
                    air_year=2023,
                    anilist_id=154587,
                    season_aliases=["Sousou no Frieren", "Frieren: Beyond Journey's End"],
                    episodes=[
                        EpisodeMetadata(
                            episode_number=i,
                            season_number=1,
                            title=(
                                "The Journey's End"
                                if i == 1
                                else "It Didn't Have to Be Magic..."
                                if i == 2
                                else "Killing Magic"
                                if i == 3
                                else f"Episode {i}"
                            ),
                        )
                        for i in range(1, 29)
                    ],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=12,
                    air_year=2026,
                    anilist_id=182255,
                    season_aliases=[
                        "Sousou no Frieren 2nd Season",
                        "Sousou no Frieren S2",
                        "Frieren: Beyond Journey's End Season 2",
                    ],
                    episodes=[
                        EpisodeMetadata(
                            episode_number=i,
                            season_number=2,
                            title=f"Season 2 Episode {i}",
                        )
                        for i in range(1, 13)
                    ],
                ),
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:171018",
            official_title="DAN DA DAN",
            original_title="ダンダダン",
            romaji_title="Dandadan",
            alternate_titles=["Dan Da Dan", "Dandadan"],
            anilist_id=171018,
            tmdb_id=240411,
            imdb_id="tt30217403",
            release_year=2024,
            media_type="anime",
            synopsis="Momo Ayase and Okarun test their conflicting beliefs in ghosts and aliens, triggering a chaotic supernatural battle.",
            genres=["Action", "Comedy", "Sci-Fi", "Supernatural"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx171018-2ldCj6QywuOa.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/171018-M04be34Qq2TW.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=12,
                    air_year=2024,
                    anilist_id=171018,
                    season_aliases=["Dandadan", "Dan Da Dan"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 13)
                    ],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=12,
                    air_year=2025,
                    anilist_id=185660,
                    season_aliases=["Dandadan 2nd Season", "Dan Da Dan Season 2"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}")
                        for i in range(1, 13)
                    ],
                ),
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:153518",
            official_title="Delicious in Dungeon",
            original_title="ダンジョン飯",
            romaji_title="Dungeon Meshi",
            alternate_titles=["Dungeon Meshi", "Delicious in Dungeon"],
            anilist_id=153518,
            tmdb_id=206586,
            imdb_id="tt21621494",
            release_year=2024,
            media_type="anime",
            synopsis="Adventurer Laios and his party cook and eat dungeon monsters to survive a rescue mission in the depths of a cursed kingdom.",
            genres=["Adventure", "Comedy", "Fantasy"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx153518-7FNR7zCx22X3.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/153518-7u3V2Y5m3rR2.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=24,
                    air_year=2024,
                    anilist_id=153518,
                    season_aliases=["Dungeon Meshi", "Delicious in Dungeon"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 25)
                    ],
                )
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:151807",
            official_title="Solo Leveling",
            original_title="俺だけレベルアップな件",
            romaji_title="Ore dake Level Up na Ken",
            alternate_titles=["Ore dake Level Up na Ken", "Solo Leveling"],
            anilist_id=151807,
            tmdb_id=127532,
            imdb_id="tt21209876",
            release_year=2024,
            media_type="anime",
            synopsis="In a world where hunters battle deadly monsters inside dimensional gates, E-rank hunter Sung Jinwoo awakens a unique system that lets only him level up.",
            genres=["Action", "Adventure", "Fantasy"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx151807-m1gX3iwfIsLu.png",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/151807-37yfQA3ym8PA.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=12,
                    air_year=2024,
                    anilist_id=151807,
                    season_aliases=["Solo Leveling", "Ore dake Level Up na Ken"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 13)
                    ],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2 — Arise from the Shadow",
                    episode_count=13,
                    air_year=2025,
                    anilist_id=176496,
                    season_aliases=["Solo Leveling Season 2", "Ore dake Level Up na Ken Season 2"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}")
                        for i in range(1, 14)
                    ],
                ),
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:161645",
            official_title="The Apothecary Diaries",
            original_title="薬屋のひとりごと",
            romaji_title="Kusuriya no Hitorigoto",
            alternate_titles=["Kusuriya no Hitorigoto", "The Apothecary Diaries"],
            anilist_id=161645,
            tmdb_id=220542,
            imdb_id="tt26743760",
            release_year=2023,
            media_type="anime",
            synopsis="Maomao, a sharp-witted apothecary's daughter, uses her knowledge of poisons and medicine to solve mysteries inside the imperial palace.",
            genres=["Drama", "Mystery"],
            status="RELEASING",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx161645-S9T3L2XzH7x9.jpg",
            backdrop_url="",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=24,
                    air_year=2023,
                    anilist_id=161645,
                    season_aliases=["Kusuriya no Hitorigoto", "The Apothecary Diaries"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 25)
                    ],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=24,
                    air_year=2025,
                    anilist_id=176301,
                    season_aliases=["Kusuriya no Hitorigoto Season 2", "The Apothecary Diaries Season 2"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}")
                        for i in range(1, 25)
                    ],
                ),
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:151342",
            official_title="Orb: On the Movements of the Earth",
            original_title="チ。―地球の運動について―",
            romaji_title="Chi.: Chikyuu no Undou ni Tsuite",
            alternate_titles=["Chi.: Chikyuu no Undou ni Tsuite", "Orb: On the Movements of the Earth"],
            anilist_id=151342,
            release_year=2024,
            media_type="anime",
            synopsis="In 15th-century Europe, scholars risk heresy and persecution to prove the heliocentric model of the cosmos.",
            genres=["Drama", "Historical"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx151342-cI6N14r642P7.jpg",
            backdrop_url="",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=25,
                    air_year=2024,
                    anilist_id=151342,
                    season_aliases=["Chi.: Chikyuu no Undou ni Tsuite", "Orb: On the Movements of the Earth"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 26)
                    ],
                )
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="tmdb:tv:1396",
            official_title="Breaking Bad",
            original_title="Breaking Bad",
            romaji_title="",
            alternate_titles=["Breaking Bad"],
            tmdb_id=1396,
            tvmaze_id=169,
            imdb_id="tt0903747",
            release_year=2008,
            media_type="tv",
            synopsis="High school chemistry teacher Walter White partners with former student Jesse Pinkman to manufacture methamphetamine after a terminal diagnosis.",
            genres=["Crime", "Drama", "Thriller"],
            status="Ended",
            poster_url="https://static.tvmaze.com/uploads/images/original_untouched/0/2400.jpg",
            backdrop_url="https://static.tvmaze.com/uploads/images/original_untouched/0/2400.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=7,
                    air_year=2008,
                    season_aliases=["Breaking Bad Season 1", "Breaking Bad S01"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 8)
                    ],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=13,
                    air_year=2009,
                    season_aliases=["Breaking Bad Season 2", "Breaking Bad S02"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}")
                        for i in range(1, 14)
                    ],
                ),
            ],
            metadata_provider="TMDB / TVMaze",
        ),
    ]

    cleaned = (query or "").strip().lower()
    if not cleaned:
        return catalog

    matched: list[MediaIdentity] = []
    for item in catalog:
        haystack = " ".join(
            [item.official_title, item.romaji_title, item.original_title, *item.alternate_titles]
        ).lower()
        if cleaned in haystack or any(tok in haystack for tok in cleaned.split()):
            matched.append(item)
    if matched:
        return matched

    # Dynamic fallback identity for arbitrary mock query
    return [
        MediaIdentity(
            canonical_id=f"mock:{_norm_key(query).replace(' ', '-')}",
            official_title=query.strip(),
            romaji_title=query.strip(),
            alternate_titles=[query.strip()],
            release_year=2025,
            media_type="anime",
            synopsis=f"Canonical metadata entry for {query.strip()}.",
            genres=["Action", "Fantasy"],
            status="RELEASING",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=12,
                    air_year=2025,
                    season_aliases=[query.strip()],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}")
                        for i in range(1, 13)
                    ],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=12,
                    air_year=2026,
                    season_aliases=[f"{query.strip()} Season 2", f"{query.strip()} S02"],
                    episodes=[
                        EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}")
                        for i in range(1, 13)
                    ],
                ),
            ],
            metadata_provider="MockCatalog",
        )
    ]


class CatalogMetadataService:
    """Unified metadata discovery orchestrating AniList, TMDB, and TVMaze with caching."""

    def __init__(
        self,
        anilist: AniListMetadataProvider | None = None,
        tmdb: TMDBMetadataProvider | None = None,
        tvmaze: TVMazeMetadataProvider | None = None,
        cache_ttl: float = 300.0,
    ) -> None:
        self.anilist = anilist or AniListMetadataProvider()
        self.tmdb = tmdb or TMDBMetadataProvider()
        self.tvmaze = tvmaze or TVMazeMetadataProvider()
        self.cache_ttl = cache_ttl
        self._lock = threading.Lock()
        self._cache: dict[tuple[str, bool], tuple[float, list[MediaIdentity], list[str]]] = {}

    def clear_cache(self) -> None:
        with self._lock:
            self._cache.clear()

    def search_shows(
        self,
        query: str = "",
        demo_mode: bool = False,
        limit: int = 8,
    ) -> tuple[list[MediaIdentity], list[str]]:
        """Discover candidate shows from metadata providers (or deterministic demo catalog in mock mode)."""
        cleaned = (query or "").strip()
        cache_key = (cleaned.lower(), bool(demo_mode))
        now = time.monotonic()
        with self._lock:
            cached = self._cache.get(cache_key)
            if cached and (now - cached[0]) < self.cache_ttl:
                return (list(cached[1]), list(cached[2]))

        if demo_mode:
            shows = get_demo_metadata_catalog(cleaned)
            notes = ["Mock metadata catalog active (AniList + TMDB/TVMaze schema)."]
            with self._lock:
                self._cache[cache_key] = (now, shows, notes)
            return (shows, notes)

        notes: list[str] = []
        ani_results: list[MediaIdentity] = []
        tmdb_results: list[MediaIdentity] = []
        tvmaze_results: list[MediaIdentity] = []

        with ThreadPoolExecutor(max_workers=3) as pool:
            fut_ani = pool.submit(self.anilist.search, cleaned, limit)
            fut_tmdb = (
                pool.submit(self.tmdb.search, cleaned, max(2, limit // 2))
                if self.tmdb.is_configured
                else None
            )
            fut_tvmaze = (
                pool.submit(self.tvmaze.search, cleaned, 4)
                if cleaned and not self.tmdb.is_configured
                else None
            )

            try:
                ani_results = fut_ani.result(timeout=10.0)
                notes.append(f"AniList: {len(ani_results)} show(s) found.")
            except Exception as exc:
                notes.append(f"AniList error: {exc}")

            if fut_tmdb is not None:
                try:
                    tmdb_results = fut_tmdb.result(timeout=10.0)
                    notes.append(f"TMDB: {len(tmdb_results)} show(s) found.")
                except Exception as exc:
                    notes.append(f"TMDB error: {exc}")
            else:
                notes.append("TMDB: API key not configured; using AniList + TVMaze public metadata.")

            if fut_tvmaze is not None:
                try:
                    tvmaze_results = fut_tvmaze.result(timeout=10.0)
                    if tvmaze_results:
                        notes.append(f"TVMaze: {len(tvmaze_results)} show(s) found.")
                except Exception as exc:
                    notes.append(f"TVMaze error: {exc}")

        merged = self._merge_identities(ani_results, [*tmdb_results, *tvmaze_results])
        if not merged:
            merged = get_demo_metadata_catalog(cleaned)
            notes.append("Fallback metadata catalog used due to empty live response.")

        with self._lock:
            self._cache[cache_key] = (now, merged, notes)
        return (merged, notes)

    @staticmethod
    def _merge_identities(
        primary_list: list[MediaIdentity],
        secondary_list: list[MediaIdentity],
    ) -> list[MediaIdentity]:
        """Merge overlapping identities across providers (e.g. enriching AniList with IMDb/TMDB IDs)."""
        merged: list[MediaIdentity] = list(primary_list)
        by_title: dict[str, MediaIdentity] = {}
        for item in merged:
            for t in [item.official_title, item.romaji_title, *item.alternate_titles]:
                nk = _norm_key(t)
                if nk:
                    by_title[nk] = item

        for sec in secondary_list:
            matched: MediaIdentity | None = None
            for t in [sec.official_title, sec.original_title, *sec.alternate_titles]:
                nk = _norm_key(t)
                if nk and nk in by_title:
                    matched = by_title[nk]
                    break
            if matched is not None:
                if not matched.tmdb_id and sec.tmdb_id:
                    matched.tmdb_id = sec.tmdb_id
                if not matched.imdb_id and sec.imdb_id:
                    matched.imdb_id = sec.imdb_id
                if not matched.tvmaze_id and sec.tvmaze_id:
                    matched.tvmaze_id = sec.tvmaze_id
                if not matched.poster_url and sec.poster_url:
                    matched.poster_url = sec.poster_url
                if not matched.backdrop_url and sec.backdrop_url:
                    matched.backdrop_url = sec.backdrop_url
                for alt in [sec.official_title, *sec.alternate_titles]:
                    if alt and alt not in matched.alternate_titles and alt != matched.official_title:
                        matched.alternate_titles.append(alt)
            else:
                merged.append(sec)
                for t in [sec.official_title, *sec.alternate_titles]:
                    nk = _norm_key(t)
                    if nk:
                        by_title[nk] = sec

        return merged
