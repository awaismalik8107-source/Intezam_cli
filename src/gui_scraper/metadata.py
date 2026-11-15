"""Metadata providers (AniList GraphQL, TMDB v3 REST, and TVMaze public REST) for Intezam."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import html
import os
import re
import threading
import time
from typing import Any
from urllib.parse import quote

import httpx

from .matching import (
    compact_alnum_key,
    normalize_title_tokens,
    preprocess_search_query,
    rank_and_filter_candidates,
)
from .models import EpisodeMetadata, MediaIdentity, SeasonMetadata

ANILIST_GRAPHQL_URL = "https://graphql.anilist.co"
TMDB_API_BASE_URL = "https://api.themoviedb.org/3"
TMDB_IMAGE_BASE_URL = "https://image.tmdb.org/t/p"
TVMAZE_API_BASE_URL = "https://api.tvmaze.com"
IMDB_SUGGEST_BASE_URL = "https://v3.sg.media-imdb.com/suggestion"

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
    cleaned = (title or "").strip()
    previous = None
    while cleaned != previous:
        previous = cleaned
        cleaned = re.sub(
            r"\s*(?:[-–—:]\s*)?(?:"
            r"Season\s*\d{1,2}(?:\s*Part\s*\d{1,2})?"
            r"|\d{1,2}(?:st|nd|rd|th)\s+Season(?:\s*Part\s*\d{1,2})?"
            r"|S\d{1,2}"
            r"|Part\s*\d{1,2}"
            r"|Cour\s*\d{1,2}"
            r"|The\s+Final\s+Season(?:\s*(?:[-–—:]?\s*)?Part\s*\d{1,2}|\s*[-–—:]?\s*Kanketsu-hen)?"
            r"|Final\s+Season(?:\s*Part\s*\d{1,2})?"
            r"|Kanketsu-hen"
            r"|The\s+Separation"
            r"|The\s+Conflict"
            r"|Soukoku-hen"
            r"|Ketsubetsu-tan"
            r"|Arise\s+from\s+the\s+Shadow"
            r")\s*$",
            "",
            cleaned,
            flags=re.IGNORECASE,
        ).strip()
    return cleaned or (title or "").strip()


def _same_franchise_season_base(node_a: dict[str, Any], node_b: dict[str, Any]) -> bool:
    """
    Return True only if node_a and node_b are numbered/cour seasons of the SAME show
    (e.g. 'Sousou no Frieren' + 'Sousou no Frieren 2nd Season', or 'BLEACH: Thousand-Year Blood War' + '... - The Separation'),
    and False when node_b is a distinct named sequel series (e.g. 'Naruto' vs 'Naruto: Shippuden', 'Bleach' vs 'BLEACH: Thousand-Year Blood War').
    """
    ta = node_a.get("title") or {}
    tb = node_b.get("title") or {}
    keys_a = {
        compact_alnum_key(_strip_season_suffix(ta.get("english") or "")),
        compact_alnum_key(_strip_season_suffix(ta.get("romaji") or "")),
        compact_alnum_key(_strip_season_suffix(ta.get("native") or "")),
    } - {""}
    keys_b = {
        compact_alnum_key(_strip_season_suffix(tb.get("english") or "")),
        compact_alnum_key(_strip_season_suffix(tb.get("romaji") or "")),
        compact_alnum_key(_strip_season_suffix(tb.get("native") or "")),
    } - {""}
    return bool(keys_a & keys_b)


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
        """Convert raw AniList media nodes into canonical MediaIdentity records, merging numbered seasons while keeping standalone sequels distinct."""
        results: list[MediaIdentity] = []
        visited_ids: set[int] = set()

        valid_nodes = [n for n in media_nodes if isinstance(n, dict) and isinstance(n.get("id"), int)]

        for node in valid_nodes:
            node_id = int(node["id"])
            if node_id in visited_ids:
                continue

            fmt = (node.get("format") or "").upper()
            if fmt == "MOVIE":
                visited_ids.add(node_id)
                results.append(cls._build_identity_from_chain(node, [node]))
                continue

            chain = cls._collect_season_chain(node)
            # Also absorb any other returned search nodes that are numbered seasons of this exact show
            for peer in valid_nodes:
                pid = int(peer["id"])
                peer_fmt = (peer.get("format") or "").upper()
                if pid not in visited_ids and peer_fmt in {"TV", "TV_SHORT", "ONA", ""} and _same_franchise_season_base(node, peer):
                    if all(int(m.get("id") or 0) != pid for m in chain):
                        chain.append(peer)

            chain.sort(
                key=lambda entry: (
                    int(entry.get("seasonYear") or (entry.get("startDate") or {}).get("year") or 9999),
                    int(entry.get("id") or 0),
                )
            )

            for member in chain:
                mid = member.get("id")
                if isinstance(mid, int):
                    visited_ids.add(mid)

            identity = cls._build_identity_from_chain(node, chain)
            results.append(identity)

            # Also surface major standalone TV sequels (e.g. Naruto -> Naruto: Shippuden, Bleach -> BLEACH: Thousand-Year Blood War)
            # that were intentionally kept separate from the root show's season chain
            edges = (node.get("relations") or {}).get("edges") or []
            for edge in edges:
                if not isinstance(edge, dict):
                    continue
                if (edge.get("relationType") or "").upper() != "SEQUEL":
                    continue
                rel_node = edge.get("node") or {}
                rel_id = rel_node.get("id")
                rel_fmt = (rel_node.get("format") or "").upper()
                if (
                    isinstance(rel_id, int)
                    and rel_id not in visited_ids
                    and all(int( vn.get("id") or 0) != rel_id for vn in valid_nodes)
                    and rel_fmt in {"TV", "ONA"}
                    and not _same_franchise_season_base(node, rel_node)
                ):
                    visited_ids.add(rel_id)
                    results.append(cls._build_identity_from_chain(rel_node, [rel_node]))

        return results

    @staticmethod
    def _collect_season_chain(primary: dict[str, Any]) -> list[dict[str, Any]]:
        """Assemble PREQUEL + primary + SEQUEL TV entries that share the same base series title into a chronological season list."""
        primary_fmt = (primary.get("format") or "").upper()
        if primary_fmt == "MOVIE":
            return [primary]

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
            if fmt not in {"TV", "TV_SHORT", "ONA", ""}:
                continue
            if not _same_franchise_season_base(primary, rel_node):
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

        is_movie = (root.get("format") or primary.get("format") or "").upper() == "MOVIE"
        official_title = (eng or rom or nat or "Unknown Anime") if is_movie else _strip_season_suffix(eng or rom or nat or "Unknown Anime")
        english_title = eng if is_movie else _strip_season_suffix(eng)
        romaji_title = (rom or eng) if is_movie else _strip_season_suffix(rom or eng)

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
        total_ep_count = 1 if is_movie else 0
        if not is_movie:
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
                total_ep_count += len(ep_list)
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

        cover = (root.get("coverImage") or primary.get("coverImage") or {})
        poster_url = (cover.get("extraLarge") or cover.get("large") or "").strip()
        backdrop_url = (root.get("bannerImage") or primary.get("bannerImage") or poster_url).strip()
        release_year = (
            root.get("seasonYear")
            or (root.get("startDate") or {}).get("year")
            or primary.get("seasonYear")
        )
        root_id = int(root.get("id") or primary.get("id") or 0)

        return MediaIdentity(
            canonical_id=f"anilist:{root_id}",
            official_title=official_title,
            english_title=english_title,
            original_title=nat,
            romaji_title=romaji_title,
            alternate_titles=alt_titles,
            anilist_id=root_id,
            mal_id=int(root.get("idMal")) if root.get("idMal") else None,
            release_year=int(release_year) if release_year else None,
            media_type="movie" if is_movie else "anime",
            country="JP",
            episode_count=total_ep_count,
            synopsis=strip_html_tags(root.get("description") or primary.get("description")),
            genres=[str(g) for g in (root.get("genres") or primary.get("genres") or []) if g],
            status=str(root.get("status") or primary.get("status") or ""),
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
        candidate_shows: list[dict[str, Any]] = []
        for entry in hits[: max(1, min(4, limit))]:
            show = (entry or {}).get("show") if isinstance(entry, dict) else None
            if not isinstance(show, dict) or not show.get("id"):
                continue
            # Filter out totally dateless, summary-less stub entries on TVMaze when possible
            if not show.get("premiered") and not show.get("summary") and not (show.get("externals") or {}).get("imdb"):
                continue
            candidate_shows.append(show)

        def _fetch_detail(show: dict[str, Any]) -> MediaIdentity:
            show_id = int(show["id"])
            try:
                detail_resp = client.get(
                    f"{self.base_url}/shows/{show_id}",
                    params={"embed[]": ["seasons", "episodes", "akas"]},
                    timeout=self.timeout,
                )
                detail_resp.raise_for_status()
                return self.normalize_show_payload(detail_resp.json())
            except Exception:
                return self.normalize_show_payload(show)

        if not candidate_shows:
            return []
        if len(candidate_shows) == 1:
            return [_fetch_detail(candidate_shows[0])]

        with ThreadPoolExecutor(max_workers=min(4, len(candidate_shows))) as pool:
            return list(pool.map(_fetch_detail, candidate_shows))

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
        is_anime = "Anime" in genres or (
            str(show.get("type") or "").lower() == "animation"
            and str(show.get("language") or "").lower() == "japanese"
        )
        net_country = ((show.get("network") or {}).get("country") or {}).get("code")
        web_country = ((show.get("webChannel") or {}).get("country") or {}).get("code")
        country_code = str(net_country or web_country or "").strip().upper()

        if country_code in {"US", "GB"}:
            c_label = "UK" if country_code == "GB" else country_code
            for qual in [f"{name} ({c_label})", f"{name} {c_label}"]:
                nk = _norm_key(qual)
                if nk not in seen_alts:
                    seen_alts.add(nk)
                    alt_titles.append(qual)

        total_eps = sum(s.episode_count for s in seasons)

        return MediaIdentity(
            canonical_id=f"tvmaze:{tvmaze_id}",
            official_title=name,
            english_title=name,
            original_title=alt_titles[0] if alt_titles else "",
            romaji_title="",
            alternate_titles=alt_titles,
            tvmaze_id=tvmaze_id,
            imdb_id=imdb_id,
            release_year=release_year,
            media_type="anime" if is_anime else "tv",
            country=country_code,
            episode_count=total_eps,
            synopsis=strip_html_tags(show.get("summary")),
            genres=genres,
            status=str(show.get("status") or ""),
            poster_url=poster_url,
            backdrop_url=poster_url,
            seasons=seasons,
            metadata_provider="TVMaze",
            confidence=0.95,
        )


class IMDbSuggestMetadataProvider:
    """Public IMDb JSON suggestion client (keyless) covering Movies and TV Series."""

    def __init__(
        self,
        base_url: str = IMDB_SUGGEST_BASE_URL,
        timeout: float = 5.0,
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
                headers={
                    "Accept": "application/json",
                    "User-Agent": "IntezamMediaBrowser/2.0",
                },
            )
        return self._shared_client

    def search(self, query: str, limit: int = 6) -> list[MediaIdentity]:
        cleaned = (query or "").strip()
        if not cleaned:
            return []
        alnum_chars = [ch.lower() for ch in cleaned if ch.isalnum()]
        first_char = alnum_chars[0] if alnum_chars else "x"
        url = f"{self.base_url}/{first_char}/{quote(cleaned.lower())}.json"
        resp = self._get_client().get(url, timeout=self.timeout)
        resp.raise_for_status()
        payload = resp.json() or {}
        entries = payload.get("d") or []
        results: list[MediaIdentity] = []
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            item_id = str(entry.get("id") or "").strip()
            if not item_id.startswith("tt"):
                continue
            qid = str(entry.get("qid") or "").strip()
            qtype = str(entry.get("q") or "").strip().lower()
            if qid in {"movie", "tvMovie"} or qtype in {"feature", "tv movie"}:
                mtype = "movie"
            elif qid in {"tvSeries", "tvMiniSeries"} or qtype in {"tv series", "tv mini-series"}:
                mtype = "tv"
            else:
                continue

            title = str(entry.get("l") or "").strip()
            if not title:
                continue
            year = entry.get("y")
            release_year = int(year) if isinstance(year, int) else None
            img_info = entry.get("i") or {}
            poster_url = (
                str(img_info.get("imageUrl") or "").strip()
                if isinstance(img_info, dict)
                else ""
            )
            results.append(
                MediaIdentity(
                    canonical_id=f"imdb:{item_id}",
                    official_title=title,
                    english_title=title,
                    original_title=title,
                    alternate_titles=[title],
                    imdb_id=item_id,
                    release_year=release_year,
                    media_type=mtype,
                    episode_count=1 if mtype == "movie" else 0,
                    poster_url=poster_url,
                    backdrop_url=poster_url,
                    seasons=[],
                    metadata_provider="IMDb",
                    confidence=0.9,
                )
            )
            if len(results) >= limit:
                break
        return results


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
        MediaIdentity(
            canonical_id="anilist:21519",
            official_title="Your Name.",
            english_title="Your Name.",
            original_title="君の名は。",
            romaji_title="Kimi no Na wa.",
            alternate_titles=["Kimi no Na wa.", "Your Name", "君の名は。"],
            anilist_id=21519,
            tmdb_id=372058,
            imdb_id="tt5311514",
            release_year=2016,
            media_type="movie",
            country="JP",
            episode_count=1,
            synopsis="Two strangers—high school girl Mitsuha in rural Itomori and Tokyo boy Taki—find themselves mysteriously swapping bodies across time.",
            genres=["Drama", "Romance", "Supernatural"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx21519-fPhvy69vnQqS.png",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/21519-1ayMXgNlmByb.jpg",
            seasons=[],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:16498",
            official_title="Attack on Titan",
            english_title="Attack on Titan",
            original_title="進撃の巨人",
            romaji_title="Shingeki no Kyojin",
            alternate_titles=["Shingeki no Kyojin", "Attack on Titan", "AoT", "進撃の巨人"],
            anilist_id=16498,
            imdb_id="tt2560140",
            release_year=2013,
            media_type="anime",
            country="JP",
            episode_count=89,
            synopsis="Several hundred years ago, humans were nearly exterminated by giant Titans; Eren Yeager vows to reclaim the world beyond the walls.",
            genres=["Action", "Drama", "Fantasy", "Mystery"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx16498-73IhOXpJZiMF.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/16498-8jpFCOcDmneX.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=25,
                    air_year=2013,
                    anilist_id=16498,
                    season_aliases=["Attack on Titan", "Shingeki no Kyojin"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 26)],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=12,
                    air_year=2017,
                    anilist_id=20958,
                    season_aliases=["Attack on Titan Season 2", "Shingeki no Kyojin Season 2", "Shingeki no Kyojin 2nd Season"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}") for i in range(1, 13)],
                ),
                SeasonMetadata(
                    season_number=3,
                    title="Season 3",
                    episode_count=22,
                    air_year=2018,
                    anilist_id=99147,
                    season_aliases=["Attack on Titan Season 3", "Shingeki no Kyojin Season 3"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=3, title=f"Episode {i}") for i in range(1, 23)],
                ),
                SeasonMetadata(
                    season_number=4,
                    title="Season 4 — The Final Season",
                    episode_count=30,
                    air_year=2020,
                    anilist_id=110277,
                    season_aliases=[
                        "Attack on Titan: The Final Season",
                        "Shingeki no Kyojin: The Final Season",
                        "Attack on Titan Season 4",
                        "Shingeki no Kyojin Season 4",
                    ],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=4, title=f"Episode {i}") for i in range(1, 31)],
                ),
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:21",
            official_title="ONE PIECE",
            english_title="ONE PIECE",
            original_title="ONE PIECE",
            romaji_title="ONE PIECE",
            alternate_titles=["One Piece", "ワンピース"],
            anilist_id=21,
            imdb_id="tt0388629",
            release_year=1999,
            media_type="anime",
            country="JP",
            episode_count=24,
            synopsis="Monkey D. Luffy sets off with his Straw Hat Pirates crew across the Grand Line in search of the legendary treasure One Piece.",
            genres=["Action", "Adventure", "Comedy", "Fantasy"],
            status="RELEASING",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx21-YCDoj1EkAxFn.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/21-wf37VakJmZqs.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=24,
                    air_year=1999,
                    anilist_id=21,
                    season_aliases=["One Piece"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 25)],
                )
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="tvmaze:50147",
            official_title="One Piece",
            english_title="One Piece",
            original_title="One Piece",
            romaji_title="",
            alternate_titles=["One Piece (Live Action)", "One Piece (US)", "One Piece 2023"],
            tvmaze_id=50147,
            imdb_id="tt11737520",
            release_year=2023,
            media_type="tv",
            country="US",
            episode_count=8,
            synopsis="Live-action seafaring adventure following Monkey D. Luffy and his pirate crew on their quest for the legendary One Piece.",
            genres=["Action", "Adventure", "Fantasy"],
            status="Running",
            poster_url="https://static.tvmaze.com/uploads/images/original_untouched/466/1166492.jpg",
            backdrop_url="https://static.tvmaze.com/uploads/images/original_untouched/466/1166492.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=8,
                    air_year=2023,
                    season_aliases=["One Piece Season 1", "One Piece S01"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 9)],
                )
            ],
            metadata_provider="TVMaze",
        ),
        MediaIdentity(
            canonical_id="anilist:20",
            official_title="Naruto",
            english_title="Naruto",
            original_title="NARUTO -ナルト-",
            romaji_title="NARUTO",
            alternate_titles=["Naruto", "NARUTO -ナルト-"],
            anilist_id=20,
            imdb_id="tt0409591",
            release_year=2002,
            media_type="anime",
            country="JP",
            episode_count=24,
            synopsis="Naruto Uzumaki, a mischievous adolescent ninja of Konohagakure, struggles as he searches for recognition and dreams of becoming the Hokage.",
            genres=["Action", "Adventure", "Comedy", "Fantasy"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx20-dE6UHbFFg1A5.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/20-HHxhPj5JD13a.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=24,
                    air_year=2002,
                    anilist_id=20,
                    season_aliases=["Naruto"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 25)],
                )
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:1735",
            official_title="Naruto: Shippuden",
            english_title="Naruto: Shippuden",
            original_title="NARUTO -ナルト- 疾風伝",
            romaji_title="NARUTO: Shippuuden",
            alternate_titles=["Naruto Shippuden", "NARUTO: Shippuuden"],
            anilist_id=1735,
            imdb_id="tt0988824",
            release_year=2007,
            media_type="anime",
            country="JP",
            episode_count=24,
            synopsis="Two and a half years after leaving Konohagakure for intense training with Jiraiya, Naruto returns to confront the Akatsuki organization.",
            genres=["Action", "Adventure", "Comedy", "Drama", "Fantasy"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx1735-RXM2s4q7i7kX.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/1735-p78CEkC4U16x.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=24,
                    air_year=2007,
                    anilist_id=1735,
                    season_aliases=["Naruto: Shippuden", "Naruto Shippuuden"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 25)],
                )
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:269",
            official_title="Bleach",
            english_title="Bleach",
            original_title="BLEACH",
            romaji_title="BLEACH",
            alternate_titles=["Bleach"],
            anilist_id=269,
            imdb_id="tt0434665",
            release_year=2004,
            media_type="anime",
            country="JP",
            episode_count=24,
            synopsis="Ichigo Kurosaki obtains the powers of a Soul Reaper from Rukia Kuchiki, defending humans from Hollows and guiding souls to Soul Society.",
            genres=["Action", "Adventure", "Supernatural"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx269-d2GmRk3167oX.png",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/269-08ar08quI9VT.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=24,
                    air_year=2004,
                    anilist_id=269,
                    season_aliases=["Bleach"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 25)],
                )
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="anilist:116674",
            official_title="BLEACH: Thousand-Year Blood War",
            english_title="BLEACH: Thousand-Year Blood War",
            original_title="BLEACH 千年血戦篇",
            romaji_title="BLEACH: Sennen Kessen-hen",
            alternate_titles=[
                "Bleach: Thousand-Year Blood War",
                "BLEACH: Sennen Kessen-hen",
                "Bleach TYBW",
            ],
            anilist_id=116674,
            imdb_id="tt14986406",
            release_year=2022,
            media_type="anime",
            country="JP",
            episode_count=39,
            synopsis="Ichigo Kurosaki returns to battle Yhwach and the Wandenreich Quincy army in the final arc of Bleach.",
            genres=["Action", "Adventure", "Supernatural"],
            status="FINISHED",
            poster_url="https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/bx116674-p3zK4PU52A9F.jpg",
            backdrop_url="https://s4.anilist.co/file/anilistcdn/media/anime/banner/116674-5v3B9K7m0Z2X.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1 (Cour 1)",
                    episode_count=13,
                    air_year=2022,
                    anilist_id=116674,
                    season_aliases=["BLEACH: Thousand-Year Blood War", "BLEACH: Sennen Kessen-hen"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 14)],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2 — The Separation",
                    episode_count=13,
                    air_year=2023,
                    anilist_id=159322,
                    season_aliases=[
                        "BLEACH: Thousand-Year Blood War - The Separation",
                        "BLEACH: Sennen Kessen-hen - Ketsubetsu-tan",
                    ],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}") for i in range(1, 14)],
                ),
                SeasonMetadata(
                    season_number=3,
                    title="Season 3 — The Conflict",
                    episode_count=13,
                    air_year=2024,
                    anilist_id=169755,
                    season_aliases=[
                        "BLEACH: Thousand-Year Blood War - The Conflict",
                        "BLEACH: Sennen Kessen-hen - Soukoku-hen",
                    ],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=3, title=f"Episode {i}") for i in range(1, 14)],
                ),
            ],
            metadata_provider="AniList",
        ),
        MediaIdentity(
            canonical_id="tvmaze:526",
            official_title="The Office",
            english_title="The Office",
            original_title="The Office",
            romaji_title="",
            alternate_titles=["The Office (US)", "The Office US", "The Office 2005"],
            tvmaze_id=526,
            imdb_id="tt0386676",
            release_year=2005,
            media_type="tv",
            country="US",
            episode_count=18,
            synopsis="Mockumentary sitcom depicting the everyday work lives of office employees at the Scranton, Pennsylvania branch of Dunder Mifflin Paper Company.",
            genres=["Comedy"],
            status="Ended",
            poster_url="https://static.tvmaze.com/uploads/images/original_untouched/85/213184.jpg",
            backdrop_url="https://static.tvmaze.com/uploads/images/original_untouched/85/213184.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=6,
                    air_year=2005,
                    season_aliases=["The Office Season 1", "The Office S01"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 7)],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=12,
                    air_year=2005,
                    season_aliases=["The Office Season 2", "The Office S02"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}") for i in range(1, 13)],
                ),
            ],
            metadata_provider="TVMaze",
        ),
        MediaIdentity(
            canonical_id="tvmaze:525",
            official_title="The Office",
            english_title="The Office",
            original_title="The Office",
            romaji_title="",
            alternate_titles=["The Office (UK)", "The Office UK", "The Office 2001"],
            tvmaze_id=525,
            imdb_id="tt0290978",
            release_year=2001,
            media_type="tv",
            country="GB",
            episode_count=12,
            synopsis="British mockumentary sitcom created by Ricky Gervais and Stephen Merchant following David Brent at Wernham Hogg paper merchants in Slough.",
            genres=["Comedy"],
            status="Ended",
            poster_url="https://static.tvmaze.com/uploads/images/original_untouched/93/234802.jpg",
            backdrop_url="https://static.tvmaze.com/uploads/images/original_untouched/93/234802.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=6,
                    air_year=2001,
                    season_aliases=["The Office UK Season 1", "The Office UK S01"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 7)],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=6,
                    air_year=2002,
                    season_aliases=["The Office UK Season 2", "The Office UK S02"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}") for i in range(1, 7)],
                ),
            ],
            metadata_provider="TVMaze",
        ),
        MediaIdentity(
            canonical_id="tvmaze:17861",
            official_title="Dark",
            english_title="Dark",
            original_title="Dark",
            romaji_title="",
            alternate_titles=["Dark (2017)"],
            tvmaze_id=17861,
            imdb_id="tt5753856",
            release_year=2017,
            media_type="tv",
            country="DE",
            episode_count=26,
            synopsis="A missing child sets four families on a frantic hunt for answers as they unearth a mind-bending mystery that spans three generations in Winden.",
            genres=["Drama", "Mystery", "Science-Fiction", "Supernatural"],
            status="Ended",
            poster_url="https://static.tvmaze.com/uploads/images/original_untouched/504/1262352.jpg",
            backdrop_url="https://static.tvmaze.com/uploads/images/original_untouched/504/1262352.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=10,
                    air_year=2017,
                    season_aliases=["Dark Season 1", "Dark S01"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 11)],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=8,
                    air_year=2019,
                    season_aliases=["Dark Season 2", "Dark S02"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}") for i in range(1, 9)],
                ),
                SeasonMetadata(
                    season_number=3,
                    title="Season 3",
                    episode_count=8,
                    air_year=2020,
                    season_aliases=["Dark Season 3", "Dark S03"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=3, title=f"Episode {i}") for i in range(1, 9)],
                ),
            ],
            metadata_provider="TVMaze",
        ),
        MediaIdentity(
            canonical_id="tvmaze:34218",
            official_title="You",
            english_title="You",
            original_title="You",
            romaji_title="",
            alternate_titles=["You (2018)", "You US"],
            tvmaze_id=34218,
            imdb_id="tt7335184",
            release_year=2018,
            media_type="tv",
            country="US",
            episode_count=20,
            synopsis="A dangerously charming, intensely obsessive young man goes to extreme measures to insert himself into the lives of those he is transfixed by.",
            genres=["Crime", "Drama", "Thriller"],
            status="Ended",
            poster_url="https://static.tvmaze.com/uploads/images/original_untouched/452/1130315.jpg",
            backdrop_url="https://static.tvmaze.com/uploads/images/original_untouched/452/1130315.jpg",
            seasons=[
                SeasonMetadata(
                    season_number=1,
                    title="Season 1",
                    episode_count=10,
                    air_year=2018,
                    season_aliases=["You Season 1", "You S01"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=1, title=f"Episode {i}") for i in range(1, 11)],
                ),
                SeasonMetadata(
                    season_number=2,
                    title="Season 2",
                    episode_count=10,
                    air_year=2019,
                    season_aliases=["You Season 2", "You S02"],
                    episodes=[EpisodeMetadata(episode_number=i, season_number=2, title=f"Episode {i}") for i in range(1, 11)],
                ),
            ],
            metadata_provider="TVMaze",
        ),
        MediaIdentity(
            canonical_id="tmdb:346364",
            official_title="It",
            english_title="It",
            original_title="It",
            romaji_title="",
            alternate_titles=["IT", "It (2017)", "IT Chapter One"],
            tmdb_id=346364,
            imdb_id="tt1396484",
            release_year=2017,
            media_type="movie",
            country="US",
            episode_count=1,
            synopsis="In the summer of 1989, a group of bullied kids band together to destroy a shape-shifting monster, which disguises itself as a clown.",
            genres=["Horror", "Thriller"],
            status="Released",
            poster_url="https://image.tmdb.org/t/p/w500/9E2y5Q7WlCVNEhP5GiVTjhEhx1o.jpg",
            backdrop_url="https://image.tmdb.org/t/p/w780/tcheoA2nPATCm2vvXw2hVQoaEFD.jpg",
            seasons=[],
            metadata_provider="TMDB",
        ),
    ]

    cleaned = (query or "").strip()
    if not cleaned:
        return catalog[:8]

    return rank_and_filter_candidates(cleaned, catalog)


class CatalogMetadataService:
    """Unified metadata discovery orchestrating AniList, TMDB, TVMaze, and IMDb with caching and explainable ranking."""

    def __init__(
        self,
        anilist: AniListMetadataProvider | None = None,
        tmdb: TMDBMetadataProvider | None = None,
        tvmaze: TVMazeMetadataProvider | None = None,
        imdb_suggest: IMDbSuggestMetadataProvider | None = None,
        cache_ttl: float = 300.0,
    ) -> None:
        self.anilist = anilist or AniListMetadataProvider()
        self.tmdb = tmdb or TMDBMetadataProvider()
        self.tvmaze = tvmaze or TVMazeMetadataProvider()
        self.imdb_suggest = imdb_suggest or IMDbSuggestMetadataProvider()
        self.cache_ttl = cache_ttl
        self._lock = threading.Lock()
        self._cache: dict[tuple[str, bool], tuple[float, list[MediaIdentity], list[str], str]] = {}
        self.last_search_status: str = "ok"

    def clear_cache(self) -> None:
        with self._lock:
            self._cache.clear()

    def search_shows_detailed(
        self,
        query: str = "",
        demo_mode: bool = False,
        limit: int = 8,
    ) -> tuple[list[MediaIdentity], list[str], str]:
        """
        Discover candidate shows from metadata providers (or deterministic demo catalog ONLY in mock mode).
        Returns (shows, provider_notes, search_status).
        search_status is one of: 'ok' | 'no_matches' | 'provider_unavailable' | 'partial_failure' | 'mock'.
        """
        cleaned = (query or "").strip()
        cache_key = (cleaned.lower(), bool(demo_mode))
        now = time.monotonic()
        with self._lock:
            cached = self._cache.get(cache_key)
            if cached and (now - cached[0]) < self.cache_ttl:
                self.last_search_status = cached[3]
                return (list(cached[1]), list(cached[2]), cached[3])

        if demo_mode:
            shows = get_demo_metadata_catalog(cleaned)[: max(1, limit)]
            status = "mock" if shows else "no_matches"
            notes = [
                f"Mock metadata catalog active ({len(shows)} show(s) matched)."
                if shows
                else f"Mock metadata catalog: 0 matches for {cleaned!r}."
            ]
            with self._lock:
                self._cache[cache_key] = (now, shows, notes, status)
                self.last_search_status = status
            return (shows, notes, status)

        nq = preprocess_search_query(cleaned)
        provider_query = nq.core_query if cleaned else ""

        notes: list[str] = []
        ani_results: list[MediaIdentity] = []
        tmdb_results: list[MediaIdentity] = []
        tvmaze_results: list[MediaIdentity] = []
        imdb_results: list[MediaIdentity] = []
        provider_errors = 0
        providers_attempted = 0

        # If the user entered a compact/alias query like "Dandadan" or "Kimi no Na wa",
        # also pick an English/spaced alias for TVMaze/AniList if needed.
        alt_provider_query = ""
        for exp in nq.expanded_aliases:
            if exp.lower() != provider_query.lower() and any("a" <= c <= "z" for c in exp.lower()):
                alt_provider_query = exp
                break

        with ThreadPoolExecutor(max_workers=5) as pool:
            providers_attempted += 1
            fut_ani = pool.submit(self.anilist.search, provider_query, max(8, limit))
            fut_ani_alt = (
                pool.submit(self.anilist.search, alt_provider_query, 6)
                if alt_provider_query and compact_alnum_key(alt_provider_query) != compact_alnum_key(provider_query)
                else None
            )
            fut_tmdb = None
            if self.tmdb.is_configured:
                providers_attempted += 1
                fut_tmdb = pool.submit(self.tmdb.search, provider_query, max(4, limit // 2))

            fut_tvmaze = None
            fut_imdb = None
            if provider_query and not self.tmdb.is_configured:
                providers_attempted += 1
                fut_tvmaze = pool.submit(self.tvmaze.search, provider_query, 4)
                providers_attempted += 1
                fut_imdb = pool.submit(self.imdb_suggest.search, provider_query, 6)

            try:
                ani_results = fut_ani.result(timeout=10.0)
                if fut_ani_alt is not None:
                    try:
                        extra_ani = fut_ani_alt.result(timeout=8.0)
                        seen_cids = {a.canonical_id for a in ani_results}
                        for ea in extra_ani:
                            if ea.canonical_id not in seen_cids:
                                seen_cids.add(ea.canonical_id)
                                ani_results.append(ea)
                    except Exception:
                        pass
                notes.append(f"AniList: {len(ani_results)} candidate(s) retrieved.")
            except Exception as exc:
                provider_errors += 1
                notes.append(f"AniList error: {exc}")

            if fut_tmdb is not None:
                try:
                    tmdb_results = fut_tmdb.result(timeout=10.0)
                    notes.append(f"TMDB: {len(tmdb_results)} candidate(s) retrieved.")
                except Exception as exc:
                    provider_errors += 1
                    notes.append(f"TMDB error: {exc}")
            else:
                notes.append("TMDB: API key not configured; using AniList + TVMaze + IMDb public metadata.")

            if fut_tvmaze is not None:
                try:
                    tvmaze_results = fut_tvmaze.result(timeout=10.0)
                    notes.append(f"TVMaze: {len(tvmaze_results)} candidate(s) retrieved.")
                except Exception as exc:
                    provider_errors += 1
                    notes.append(f"TVMaze error: {exc}")

            if fut_imdb is not None:
                try:
                    imdb_results = fut_imdb.result(timeout=6.0)
                    notes.append(f"IMDb: {len(imdb_results)} candidate(s) retrieved.")
                except Exception as exc:
                    # Non-fatal enrichment error if AniList/TVMaze already ran
                    notes.append(f"IMDb suggest warning: {exc}")

        merged = self._merge_identities(ani_results, [*imdb_results, *tmdb_results, *tvmaze_results])
        # Suppress dateless TVMaze stub duplicates if another verified record already holds the same IMDb ID
        ranked = rank_and_filter_candidates(nq, merged)[: max(1, limit)] if cleaned else merged[: max(1, limit)]

        if provider_errors >= providers_attempted and providers_attempted > 0 and not ranked:
            status = "provider_unavailable"
            notes.append("All live metadata providers were unreachable or returned errors.")
        elif provider_errors > 0:
            status = "partial_failure" if ranked else "no_matches"
        elif not ranked:
            status = "no_matches"
            notes.append(f"No verified metadata matches found for {cleaned!r}.")
        else:
            status = "ok"

        with self._lock:
            self._cache[cache_key] = (now, ranked, notes, status)
            self.last_search_status = status
        return (ranked, notes, status)

    def search_shows(
        self,
        query: str = "",
        demo_mode: bool = False,
        limit: int = 8,
    ) -> tuple[list[MediaIdentity], list[str]]:
        """Discover candidate shows from metadata providers without silent mock fallback in Live mode."""
        shows, notes, _ = self.search_shows_detailed(query=query, demo_mode=demo_mode, limit=limit)
        return (shows, notes)

    @staticmethod
    def _can_merge_identities(a: MediaIdentity, b: MediaIdentity) -> bool:
        """
        Determine whether two MediaIdentity records from different providers represent the exact same work.
        Never merge across conflicting media types (e.g. anime vs live-action TV), conflicting release years (> 1 yr),
        or conflicting countries (e.g. UK vs US).
        """
        # Upgrade TVMaze-vs-IMDb merge when TVMaze carries rich seasons + episode data over minimal IMDb stub
        # Never merge conflicting IMDb IDs, and never merge across conflicting media types unless one is dateless stub
        if a.imdb_id and b.imdb_id:
            return a.imdb_id == b.imdb_id
        if a.tmdb_id and b.tmdb_id and a.media_type == b.media_type:
            return a.tmdb_id == b.tmdb_id
        if a.anilist_id and b.anilist_id:
            return a.anilist_id == b.anilist_id
        if a.tvmaze_id and b.tvmaze_id:
            return a.tvmaze_id == b.tvmaze_id

        # Never merge different media types (e.g. 1999 One Piece Anime vs 2023 One Piece Live-Action TV),
        # UNLESS a movie is also returned as a Japanese animation movie/special or an IMDb anime entry.
        if a.media_type != b.media_type:
            if {a.media_type, b.media_type} == {"anime", "movie"} and (a.imdb_id or b.imdb_id):
                pass
            else:
                return False

        # Never merge if release years conflict by more than 1 year (or if one is missing a year when the other has one)
        if a.release_year is not None and b.release_year is not None:
            if abs(a.release_year - b.release_year) > 1:
                return False
        elif a.release_year is not None or b.release_year is not None:
            return False

        # Never merge across conflicting countries (e.g. The Office GB vs US)
        ca = (getattr(a, "country", "") or "").upper()
        cb = (getattr(b, "country", "") or "").upper()
        if ca and cb and ca != cb:
            return False

        titles_a = {
            _norm_key(t)
            for t in [a.official_title, getattr(a, "english_title", ""), a.romaji_title, *a.alternate_titles]
            if t and _norm_key(t)
        }
        titles_b = {
            _norm_key(t)
            for t in [b.official_title, getattr(b, "english_title", ""), b.romaji_title, *b.alternate_titles]
            if t and _norm_key(t)
        }
        return bool(titles_a & titles_b)

    @classmethod
    def _merge_identities(
        cls,
        primary_list: list[MediaIdentity],
        secondary_list: list[MediaIdentity],
    ) -> list[MediaIdentity]:
        """Merge overlapping identities across providers only when external IDs or (title + media_type + year + country) agree."""
        merged: list[MediaIdentity] = list(primary_list)

        for sec in secondary_list:
            matched: MediaIdentity | None = None
            for existing in merged:
                if cls._can_merge_identities(existing, sec):
                    matched = existing
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
                if not matched.synopsis and sec.synopsis:
                    matched.synopsis = sec.synopsis
                if not matched.genres and sec.genres:
                    matched.genres = list(sec.genres)
                if not matched.country and sec.country:
                    matched.country = sec.country
                if not matched.seasons and sec.seasons and matched.media_type != "movie":
                    matched.seasons = list(sec.seasons)
                    matched.episode_count = matched.episode_count or sec.episode_count
                # If a rich TVMaze record arrives after a minimal IMDb suggestion stub, upgrade title/seasons/provider
                if (
                    matched.canonical_id.startswith("imdb:")
                    and sec.tvmaze_id
                    and sec.seasons
                    and matched.media_type == "tv"
                ):
                    matched.canonical_id = sec.canonical_id
                    matched.tvmaze_id = sec.tvmaze_id
                    matched.media_type = sec.media_type
                    matched.metadata_provider = f"{sec.metadata_provider} + IMDb"
                for alt in [sec.official_title, *sec.alternate_titles]:
                    if alt and alt not in matched.alternate_titles and alt != matched.official_title:
                        matched.alternate_titles.append(alt)
            else:
                merged.append(sec)

        return merged
