"""Deterministic Episode-to-Torrent File Resolution Engine (Stages 3–7).

Connects canonical show/episode metadata, multi-provider torrent discovery,
metadata-only BitTorrent manifest inspection, and exact C++ playback targets.
"""

from __future__ import annotations

import json
import math
import posixpath
import re
import subprocess
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Callable

from .matching import (
    _CONFLICT_SPINOFF_TOKENS,
    ParsedRelease,
    compact_alnum_key,
    infer_technical_metadata,
    normalize_title_tokens,
    parse_release_title,
)


def normalize_match_text(text: str) -> str:
    """Return space-joined normalized title tokens using matching.py's tokenizer."""
    return normalize_title_tokens(text)[0]
from .models import (
    EpisodeMetadata,
    EpisodeResolutionResult,
    MediaIdentity,
    ResolvedFileCandidate,
    ScrapedItem,
    SourceSearchFilters,
)
from .providers import MultiProviderDiscoveryEngine, format_bytes_label, parse_size_to_bytes
from .torrent_manifest import (
    _SUBTITLE_EXTS as SUBTITLE_EXTENSIONS,
    _VIDEO_EXTS as VIDEO_EXTENSIONS,
    TorrentManifestFile,
    TorrentManifestInspector,
    TorrentManifestResult,
    normalize_info_hash,
)

# ---------------------------------------------------------------------------
# Stage 3: Advanced Deterministic Media Filename Parser
# ---------------------------------------------------------------------------

_TECH_NOISE_RE = re.compile(
    r"(?:\b(?:2160p|1080p|720p|480p|360p|540p|4k|8k|"
    r"1920x1080|1280x720|3840x2160|848x480|"
    r"x264|x265|h\.?264|h\.?265|hevc|avc|av1|vp9|xvid|divx|"
    r"10bit|10-bit|8bit|8-bit|12bit|hi10p|"
    r"5\.1|7\.1|2\.0|ddp?5\.1|dd5\.1|aac2\.0|eac3|ac3|dts(?:-hd)?|flac|opus|"
    r"bluray|blu-ray|bdrip|bdremux|remux|webrip|web-dl|webdl|hdtv|dvdrip|hdrip|"
    r"dual[- ]?audio|multi[- ]?sub|multi[- ]?audio|uncensored|repack|proper)\b|"
    r"\[[0-9a-fA-F]{8}\]|\([0-9a-fA-F]{8}\))",
    re.IGNORECASE,
)

_SXXEXX_MULTI_RE = re.compile(
    r"(?<![a-z0-9])s(?P<season>\d{1,2})\s*[.\-_ ]*e(?P<ep1>\d{1,4})"
    r"(?:v\d+)?\s*(?:-|to|~|&|e)\s*e?(?P<ep2>\d{1,4})(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_SXXEXX_RE = re.compile(
    r"(?<![a-z0-9])s(?P<season>\d{1,2})\s*[.\-_ ]*e(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_NXNN_MULTI_RE = re.compile(
    r"(?<![a-z0-9])(?P<season>\d{1,2})x(?P<ep1>\d{1,4})(?:v\d+)?\s*-\s*(?:(?P=season)x)?(?P<ep2>\d{1,4})(?![a-z0-9])",
    re.IGNORECASE,
)

_NXNN_RE = re.compile(
    r"(?<![a-z0-9])(?P<season>\d{1,2})x(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_SEASON_EP_WORDS_RE = re.compile(
    r"(?:season|series)\s*(?P<season>\d{1,2})\s*[,.\-_/ ]*\s*(?:episode|ep\.?|e)\s*#?(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_EP_EXPLICIT_MULTI_RE = re.compile(
    r"(?<![a-z0-9])(?:episode|ep\.?)\s*#?(?P<ep1>\d{1,4})(?:v\d+)?\s*(?:-|to|~)\s*(?:episode|ep\.?|e)?\s*#?(?P<ep2>\d{1,4})(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_EP_EXPLICIT_RE = re.compile(
    r"(?<![a-z0-9])(?:episode|ep\.?)\s*#?(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_SPECIAL_MARKER_RE = re.compile(
    r"(?<![a-z0-9])(?:sp|ova|oad|ona|special)\s*#?(?P<sp>\d{1,3})?(?:v(?P<ver>\d+))?(?![a-z0-9])",
    re.IGNORECASE,
)

_EXTRA_NON_EP_RE = re.compile(
    r"(?<![a-z0-9])(?:ncop\d*|nced\d*|creditless|textless|trailer|teaser|preview|"
    r"promo|pv\d*|cm\d*|menu\d*|recap|sample|interview)(?![a-z0-9])|"
    r"(?:^|/)(?:extras?|bonus|special features|bdmenu|scans?|ost|music)(?:/|$)",
    re.IGNORECASE,
)

_AMBIGUOUS_DISC_RE = re.compile(
    r"(?<![a-z0-9])(?:disc|disk|bdmv|dvd|cd|part|vol(?:ume)?)\s*#?\d*(?![a-z0-9])",
    re.IGNORECASE,
)

_BATCH_RANGE_RE = re.compile(
    r"[\[(]?(?P<s_ep>\d{1,4})\s*[-~]\s*(?P<e_ep>\d{1,4})[\])]?",
)

_FOLDER_SEASON_RE = re.compile(
    r"^(?:season\s*|series\s*|s)(?P<season>\d{1,2})\b|^s(?P<season_b>\d{1,2})[^a-z0-9]*$|"
    r"\bseason\s*(?P<season_c>\d{1,2})\b|\b(?P<ord>\d{1,2})(?:st|nd|rd|th)\s+season\b",
    re.IGNORECASE,
)

_FOLDER_SPECIAL_RE = re.compile(
    r"^(?:specials?|ovas?|oads?|sp|bonus\s+episodes?|season\s*0+)\b",
    re.IGNORECASE,
)

_FOLDER_PART_RE = re.compile(r"\b(?:part|cour)\s*(?P<part>\d{1,2})\b", re.IGNORECASE)

_DASH_EP_RE = re.compile(
    r"^(?P<prefix>.+?)\s+-\s+(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?(?:\s|\[|\(|$|[-_])",
)

_DASH_EP_MULTI_RE = re.compile(
    r"^(?P<prefix>.+?)\s+-\s+(?P<ep1>\d{1,4})(?:v\d+)?\s*[-~]\s*(?P<ep2>\d{1,4})(?:v(?P<ver>\d+))?(?:\s|\[|\(|$)",
)

_NUMERIC_STEM_RE = re.compile(r"^\s*#?(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?\s*$")

_TRAILING_EP_RE = re.compile(
    r"(?:^|[\s._\[(\-#])(?P<ep>\d{1,4})(?:v(?P<ver>\d+))?(?:\s*(?:[-–—]\s*[^\[\]()]+)?)\s*$"
)

_YEAR_STANDALONE_RE = re.compile(r"[\[(]?(19\d{2}|20\d{2})[\])]?")

_SPINOFF_CONFLICT_PHRASES: tuple[tuple[str, str], ...] = (
    ("shippuden", "naruto_shippuden"),
    ("shippuuden", "naruto_shippuden"),
    ("boruto", "boruto"),
    ("thousand year blood war", "bleach_tybw"),
    ("sennen kessen", "bleach_tybw"),
    ("tybw", "bleach_tybw"),
    ("live action", "live_action"),
    ("junior high", "spinoff"),
    ("chibi", "spinoff"),
    ("brotherhood", "fma_brotherhood"),
    ("kai", "db_kai"),
    ("gt", "db_gt"),
    ("daima", "db_daima"),
)


class ParsedMediaPath:
    """Structured deterministic parse result for a media file path within a torrent."""

    __slots__ = (
        "raw_path",
        "filename",
        "stem",
        "extension",
        "folders",
        "is_video",
        "is_subtitle",
        "is_extra_or_non_episode",
        "is_ambiguous_container",
        "is_special",
        "is_batch_range",
        "is_multi_episode",
        "season",
        "episode",
        "episode_end",
        "version",
        "folder_season",
        "folder_part",
        "release_year",
        "parse_method",
        "ambiguity_reason",
    )

    def __init__(
        self,
        *,
        raw_path: str,
        filename: str,
        stem: str,
        extension: str,
        folders: list[str],
        is_video: bool,
        is_subtitle: bool,
        is_extra_or_non_episode: bool,
        is_ambiguous_container: bool,
        is_special: bool,
        is_batch_range: bool,
        is_multi_episode: bool,
        season: int | None,
        episode: int | None,
        episode_end: int | None,
        version: int,
        folder_season: int | None,
        folder_part: int | None,
        release_year: int | None,
        parse_method: str,
        ambiguity_reason: str = "",
    ) -> None:
        self.raw_path = raw_path
        self.filename = filename
        self.stem = stem
        self.extension = extension
        self.folders = folders
        self.is_video = is_video
        self.is_subtitle = is_subtitle
        self.is_extra_or_non_episode = is_extra_or_non_episode
        self.is_ambiguous_container = is_ambiguous_container
        self.is_special = is_special
        self.is_batch_range = is_batch_range
        self.is_multi_episode = is_multi_episode
        self.season = season
        self.episode = episode
        self.episode_end = episode_end
        self.version = version
        self.folder_season = folder_season
        self.folder_part = folder_part
        self.release_year = release_year
        self.parse_method = parse_method
        self.ambiguity_reason = ambiguity_reason


def _collect_identity_title_numbers(identity: MediaIdentity | None) -> set[int]:
    """Collect numeric tokens appearing inside canonical show titles (e.g., 86, 1899, 100)."""
    nums: set[int] = set()
    if identity is None:
        return nums
    title_pool = [
        identity.official_title,
        identity.english_title,
        identity.romaji_title,
        identity.original_title,
        *identity.alternate_titles,
    ]
    for raw in title_pool:
        if not raw:
            continue
        for tok in re.findall(r"\b\d{1,4}\b", raw):
            try:
                nums.add(int(tok))
            except ValueError:
                pass
    return nums


def _strip_leading_group_and_tech(text: str) -> tuple[str, int | None]:
    """Remove release group tags, hashes, tech tokens, and extract release year if present."""
    year: int | None = None
    ym = re.findall(r"(?:^|[\s.(\[_])(19\d{2}|20\d{2})(?:$|[\s.)\]_])", text)
    if ym:
        try:
            cand_yr = int(ym[-1])
            if 1950 <= cand_yr <= 2035:
                year = cand_yr
        except ValueError:
            pass

    cleaned = re.sub(r"^\s*(?:\[[^\]]+\]|\([^)]+\))\s*", "", text)
    cleaned = _TECH_NOISE_RE.sub(" ", cleaned)
    # Remove trailing bracketed/parenthesized metadata such as (1080p) [CRC]
    cleaned = re.sub(r"\s*[\[(][^\]\)]*[\])]\s*$", "", cleaned)
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    return cleaned, year


def _strip_known_title_prefix(stem: str, identity: MediaIdentity | None) -> str:
    """Strip known show title phrases (including numbers like '86' or '1899') from the front."""
    if identity is None:
        return stem
    candidates = [
        identity.official_title,
        identity.english_title,
        identity.romaji_title,
        *identity.alternate_titles,
    ]
    norm_stem = normalize_match_text(stem)
    best_len = 0
    for title in candidates:
        t_norm = normalize_match_text(title)
        if not t_norm:
            continue
        if norm_stem == t_norm or norm_stem.startswith(t_norm + " "):
            best_len = max(best_len, len(t_norm))
    if best_len <= 0:
        return stem
    # Remove matching leading words proportionally from a dot/underscore-normalized stem
    word_count = len(norm_stem[:best_len].split())
    spaced = re.sub(r"[._]+", " ", stem).strip()
    parts = spaced.split()
    if word_count <= len(parts):
        remainder = " ".join(parts[word_count:]).strip(" -._")
        return remainder
    return stem


def parse_media_path(
    file_path: str,
    identity: MediaIdentity | None = None,
) -> ParsedMediaPath:
    """Parse a relative torrent file path into structured season/episode/special metadata."""
    normalized = (file_path or "").replace("\\", "/").strip("/")
    parts = [p for p in normalized.split("/") if p]
    filename = parts[-1] if parts else normalized
    folders = parts[:-1]
    stem, ext = posixpath.splitext(filename)
    ext_lower = ext.lower()

    is_video = ext_lower in VIDEO_EXTENSIONS
    is_subtitle = ext_lower in SUBTITLE_EXTENSIONS

    # Analyze folder context
    folder_season: int | None = None
    folder_special = False
    folder_part: int | None = None
    for folder in folders:
        if _FOLDER_SPECIAL_RE.search(folder):
            folder_special = True
            folder_season = 0
        fm = _FOLDER_SEASON_RE.search(folder)
        if fm:
            raw_s = fm.group("season") or fm.group("season_b") or fm.group("season_c") or fm.group("ord")
            if raw_s and raw_s.isdigit():
                s_val = int(raw_s)
                if s_val == 0:
                    folder_special = True
                folder_season = s_val
        pm = _FOLDER_PART_RE.search(folder)
        if pm:
            folder_part = int(pm.group("part"))

    # Extras / non-episode assets check
    is_extra = bool(_EXTRA_NON_EP_RE.search(normalized)) or bool(_EXTRA_NON_EP_RE.search(stem))
    if ext_lower in {".iso", ".img"}:
        return ParsedMediaPath(
            raw_path=file_path,
            filename=filename,
            stem=stem,
            extension=ext_lower,
            folders=folders,
            is_video=False,
            is_subtitle=False,
            is_extra_or_non_episode=False,
            is_ambiguous_container=True,
            is_special=folder_special,
            is_batch_range=False,
            is_multi_episode=False,
            season=folder_season,
            episode=None,
            episode_end=None,
            version=1,
            folder_season=folder_season,
            folder_part=folder_part,
            release_year=None,
            parse_method="ambiguous_disc_image",
            ambiguity_reason="disc image (.iso/.img) cannot target an episode stream directly",
        )

    cleaned_stem, release_year = _strip_leading_group_and_tech(stem)
    # Remove standalone release year after tech cleanup unless it matches a title number (e.g. 1899)
    title_nums = _collect_identity_title_numbers(identity)

    # Mask BDMV / Blu-ray structural playlist paths
    if any(f.upper() in {"BDMV", "STREAM", "VIDEO_TS"} for f in folders):
        return ParsedMediaPath(
            raw_path=file_path,
            filename=filename,
            stem=stem,
            extension=ext_lower,
            folders=folders,
            is_video=is_video,
            is_subtitle=is_subtitle,
            is_extra_or_non_episode=False,
            is_ambiguous_container=True,
            is_special=folder_special,
            is_batch_range=False,
            is_multi_episode=False,
            season=folder_season,
            episode=None,
            episode_end=None,
            version=1,
            folder_season=folder_season,
            folder_part=folder_part,
            release_year=release_year,
            parse_method="ambiguous_bdmv_stream",
            ambiguity_reason="BDMV/VIDEO_TS stream layout without unambiguous episode mapping",
        )

    def _build(
        *,
        season: int | None,
        episode: int | None,
        episode_end: int | None = None,
        version: int = 1,
        is_special: bool = False,
        is_batch_range: bool = False,
        is_multi_episode: bool = False,
        is_ambiguous: bool = False,
        ambiguity_reason: str = "",
        method: str,
    ) -> ParsedMediaPath:
        eff_season = season if season is not None else folder_season
        eff_special = is_special or folder_special or (eff_season == 0)
        return ParsedMediaPath(
            raw_path=file_path,
            filename=filename,
            stem=stem,
            extension=ext_lower,
            folders=folders,
            is_video=is_video,
            is_subtitle=is_subtitle,
            is_extra_or_non_episode=is_extra,
            is_ambiguous_container=is_ambiguous,
            is_special=eff_special,
            is_batch_range=is_batch_range,
            is_multi_episode=is_multi_episode,
            season=eff_season,
            episode=episode,
            episode_end=episode_end,
            version=version,
            folder_season=folder_season,
            folder_part=folder_part,
            release_year=release_year,
            parse_method=method,
            ambiguity_reason=ambiguity_reason,
        )

    # 1. Explicit SxxExx-Exx multi-episode
    m = _SXXEXX_MULTI_RE.search(cleaned_stem)
    if m:
        s_num = int(m.group("season"))
        ep1 = int(m.group("ep1"))
        ep2 = int(m.group("ep2"))
        ver = int(m.group("ver") or 1)
        if ep2 >= ep1 and (ep2 - ep1) <= 3:
            return _build(
                season=s_num,
                episode=ep1,
                episode_end=ep2,
                version=ver,
                is_special=(s_num == 0),
                is_multi_episode=True,
                method="sxxexx_multi",
            )

    # 2. Explicit SxxExx single episode
    m = _SXXEXX_RE.search(cleaned_stem)
    if m:
        s_num = int(m.group("season"))
        ep = int(m.group("ep"))
        ver = int(m.group("ver") or 1)
        return _build(
            season=s_num,
            episode=ep,
            version=ver,
            is_special=(s_num == 0),
            method="sxxexx",
        )

    # 3. 1x07-08 / 1x07
    m = _NXNN_MULTI_RE.search(cleaned_stem)
    if m:
        s_num = int(m.group("season"))
        ep1 = int(m.group("ep1"))
        ep2 = int(m.group("ep2"))
        if ep2 >= ep1 and (ep2 - ep1) <= 3:
            return _build(
                season=s_num,
                episode=ep1,
                episode_end=ep2,
                is_special=(s_num == 0),
                is_multi_episode=True,
                method="nxnn_multi",
            )

    m = _NXNN_RE.search(cleaned_stem)
    if m:
        s_num = int(m.group("season"))
        ep = int(m.group("ep"))
        ver = int(m.group("ver") or 1)
        return _build(
            season=s_num,
            episode=ep,
            version=ver,
            is_special=(s_num == 0),
            method="nxnn",
        )

    # 4. "Season 1 Episode 07"
    m = _SEASON_EP_WORDS_RE.search(cleaned_stem)
    if m:
        s_num = int(m.group("season"))
        ep = int(m.group("ep"))
        ver = int(m.group("ver") or 1)
        return _build(
            season=s_num,
            episode=ep,
            version=ver,
            is_special=(s_num == 0),
            method="season_episode_words",
        )

    # 5. Specials / OVA / OAD / SP01
    m = _SPECIAL_MARKER_RE.search(cleaned_stem)
    if m and not re.search(r"\bepisode\b", cleaned_stem, re.IGNORECASE):
        sp_num = int(m.group("sp")) if m.group("sp") else 1
        ver = int(m.group("ver") or 1)
        return _build(
            season=0,
            episode=sp_num,
            version=ver,
            is_special=True,
            method="special_marker",
        )

    # 6. Ambiguous Disc / Part container without episode number
    if _AMBIGUOUS_DISC_RE.search(cleaned_stem) and not _EP_EXPLICIT_RE.search(cleaned_stem):
        return _build(
            season=folder_season,
            episode=None,
            is_ambiguous=True,
            ambiguity_reason="ambiguous Disc/Part/Volume naming without explicit episode identifier",
            method="ambiguous_disc_part",
        )

    # 7. Explicit "Episode 07-08" / "EP 07"
    m = _EP_EXPLICIT_MULTI_RE.search(cleaned_stem)
    if m:
        ep1 = int(m.group("ep1"))
        ep2 = int(m.group("ep2"))
        ver = int(m.group("ver") or 1)
        if ep2 >= ep1 and (ep2 - ep1) <= 3:
            return _build(
                season=folder_season,
                episode=ep1,
                episode_end=ep2,
                version=ver,
                is_multi_episode=True,
                method="explicit_ep_multi",
            )

    m = _EP_EXPLICIT_RE.search(cleaned_stem)
    if m:
        ep = int(m.group("ep"))
        ver = int(m.group("ver") or 1)
        return _build(
            season=folder_season,
            episode=ep,
            version=ver,
            method="explicit_ep_word",
        )

    # 8. Pure numeric filename stem (e.g. "Season 1/07.mkv" or "07v2.mkv")
    m = _NUMERIC_STEM_RE.match(cleaned_stem)
    if m:
        ep = int(m.group("ep"))
        ver = int(m.group("ver") or 1)
        if ep in title_nums and not folders:
            return _build(
                season=folder_season,
                episode=None,
                is_ambiguous=True,
                ambiguity_reason="number matches show title without folder/episode context",
                method="title_number_only",
            )
        return _build(
            season=folder_season,
            episode=ep,
            version=ver,
            method="numeric_stem",
        )

    # 9. Fansub dash syntax: "[Group] Show - 07 [1080p].mkv" or "86 - 07.mkv"
    m = _DASH_EP_MULTI_RE.match(cleaned_stem)
    if m:
        ep1 = int(m.group("ep1"))
        ep2 = int(m.group("ep2"))
        ver = int(m.group("ver") or 1)
        if ep2 >= ep1 and (ep2 - ep1) <= 2:
            return _build(
                season=folder_season,
                episode=ep1,
                episode_end=ep2,
                version=ver,
                is_multi_episode=True,
                method="fansub_dash_multi",
            )
        if ep2 >= ep1 and (ep2 - ep1) >= 3:
            return _build(
                season=folder_season,
                episode=None,
                is_batch_range=True,
                method="batch_range",
            )

    m = _DASH_EP_RE.match(cleaned_stem)
    if m:
        ep = int(m.group("ep"))
        ver = int(m.group("ver") or 1)
        return _build(
            season=folder_season,
            episode=ep,
            version=ver,
            method="fansub_dash",
        )

    # 10. Batch ranges like [01-12] or 01~24 -> mark batch, NOT single episode
    bm = _BATCH_RANGE_RE.search(cleaned_stem)
    if bm:
        s_ep = int(bm.group("s_ep"))
        e_ep = int(bm.group("e_ep"))
        if e_ep > s_ep and (e_ep - s_ep) >= 2:
            return _build(
                season=folder_season,
                episode=None,
                is_batch_range=True,
                method="batch_range",
            )

    # 11. Strip known show title (to protect numeric titles like 86 / 1899) then look for trailing ep number
    without_title = _strip_known_title_prefix(cleaned_stem, identity)
    # Also strip trailing standalone year so "Show (2024)" isn't read as ep 2024
    if release_year is not None and release_year not in title_nums:
        without_title = re.sub(rf"\b{release_year}\b", " ", without_title).strip()

    if without_title:
        tm = _TRAILING_EP_RE.search(without_title)
        if tm:
            ep = int(tm.group("ep"))
            ver = int(tm.group("ver") or 1)
            if not (ep in title_nums and without_title == cleaned_stem) and not (1950 <= ep <= 2035 and ep not in title_nums):
                return _build(
                    season=folder_season,
                    episode=ep,
                    version=ver,
                    method="trailing_number",
                )

    return _build(
        season=folder_season,
        episode=None,
        method="unparsed",
    )


# ---------------------------------------------------------------------------
# Stage 4: Context-Aware Manifest Analyzer & Sibling Sequence Validator
# ---------------------------------------------------------------------------


def _normalize_show_alias_set(identity: MediaIdentity) -> list[str]:
    titles = [
        identity.official_title,
        identity.english_title,
        identity.romaji_title,
        identity.original_title,
        *identity.alternate_titles,
    ]
    out: list[str] = []
    seen: set[str] = set()
    for t in titles:
        key = compact_alnum_key(t)
        if key and key not in seen:
            seen.add(key)
            out.append(key)
    return out


def _detect_spinoff_contradiction(
    identity: MediaIdentity,
    release_title: str,
) -> str | None:
    """Return a contradiction reason if release clearly belongs to a different spinoff/adaptation."""
    show_norm = normalize_match_text(
        " ".join(
            p
            for p in [
                identity.official_title,
                identity.english_title,
                identity.romaji_title,
                *identity.alternate_titles,
            ]
            if p
        )
    )
    rel_norm = normalize_match_text(release_title)

    # Check phrase-level conflicts (e.g. Shippuden vs base Naruto, TYBW vs base Bleach, Live Action vs Anime)
    for phrase, _label in _SPINOFF_CONFLICT_PHRASES:
        in_rel = bool(re.search(rf"\b{re.escape(phrase)}\b", rel_norm))
        in_show = bool(re.search(rf"\b{re.escape(phrase)}\b", show_norm))
        if in_rel != in_show:
            # Allow Bleach TYBW if identity explicitly includes season 17 / TYBW season aliases
            if phrase in {"thousand year blood war", "sennen kessen", "tybw"}:
                season_text = normalize_match_text(
                    " ".join(
                        a
                        for s in identity.seasons
                        for a in [s.title, *s.season_aliases]
                    )
                )
                if re.search(rf"\b{re.escape(phrase)}\b", season_text):
                    continue
            if in_rel and not in_show:
                return f"spinoff/adaptation conflict ('{phrase}' in release does not match show)"

    # Live Action marker LA or (2023) country conflicts for anime
    if identity.media_type == "anime" and re.search(r"\blive\s+action\b", rel_norm):
        if "live action" not in show_norm:
            return "spinoff/adaptation conflict (live action vs anime)"

    # UK vs US regional mismatch (e.g., The Office UK vs US)
    country = (identity.country or "").upper()
    if country == "US" and re.search(r"\b(?:uk|british)\b", rel_norm) and not re.search(r"\b(?:uk|british)\b", show_norm):
        return "regional adaptation conflict (UK release vs US show)"
    if country == "GB" and re.search(r"\bus\b", rel_norm) and not re.search(r"\bus\b", show_norm):
        return "regional adaptation conflict (US release vs UK show)"

    return None


def _compute_absolute_episode_and_regular_counts(
    identity: MediaIdentity,
    target_season: int,
    target_episode: int,
) -> tuple[int | None, int, bool]:
    """Return (target_abs_episode, regular_season_count, is_single_season)."""
    regular_seasons = sorted(
        (s for s in identity.seasons if s.season_number >= 1),
        key=lambda s: s.season_number,
    )
    if target_season <= 0:
        return (None, len(regular_seasons), len(regular_seasons) <= 1)

    target_abs: int | None = None
    cumulative = 0
    for s in regular_seasons:
        if s.season_number == target_season:
            for ep in s.episodes:
                if ep.episode_number == target_episode and ep.absolute_episode is not None:
                    target_abs = ep.absolute_episode
                    break
            if target_abs is None:
                target_abs = cumulative + target_episode
            break
        s_span = max(
            int(s.episode_count or 0),
            max((e.episode_number for e in s.episodes), default=0),
        )
        cumulative += s_span

    return (target_abs, len(regular_seasons), len(regular_seasons) <= 1)


def _analyze_sibling_sequences(
    files: list[TorrentManifestFile],
    identity: MediaIdentity,
) -> dict[str, dict[str, Any]]:
    """Inspect sibling video files per folder to detect sequential episode series."""
    folder_groups: dict[str, list[tuple[TorrentManifestFile, ParsedMediaPath]]] = {}
    parsed_cache: dict[int, ParsedMediaPath] = {}
    for mf in files:
        parsed = parse_media_path(mf.relative_path, identity=identity)
        parsed_cache[id(mf)] = parsed
        if not (mf.file_type == "video") or parsed.is_extra_or_non_episode:
            continue
        folder_key = "/".join(parsed.folders)
        folder_groups.setdefault(folder_key, []).append((mf, parsed))

    folder_stats: dict[str, dict[str, Any]] = {}
    for folder_key, entries in folder_groups.items():
        ep_nums = sorted(
            {
                p.episode
                for _, p in entries
                if p.episode is not None and not p.is_ambiguous_container
            }
        )
        is_sequential = False
        if len(ep_nums) >= 3:
            # Check how many adjacent steps differ by 1
            diffs_one = sum(1 for i in range(1, len(ep_nums)) if ep_nums[i] - ep_nums[i - 1] == 1)
            if diffs_one >= max(2, len(ep_nums) // 2):
                is_sequential = True
        folder_stats[folder_key] = {
            "video_count": len(entries),
            "episode_numbers": ep_nums,
            "min_ep": min(ep_nums) if ep_nums else None,
            "max_ep": max(ep_nums) if ep_nums else None,
            "is_sequential": is_sequential,
        }
    return {"folder_stats": folder_stats, "parsed_cache": parsed_cache}


# ---------------------------------------------------------------------------
# Stage 5: Evidence-Based Confidence Engine & Candidate Evaluation
# ---------------------------------------------------------------------------


def _build_candidate_score(
    *,
    status: str,
    confidence: float,
    has_verified_manifest: bool,
    is_multi_episode: bool,
    extension: str,
    seeders: int,
    resolution: str,
    file_size_bytes: int,
    preferred_quality: str = "1080p",
) -> float:
    status_tier = {
        "confirmed": 10000.0,
        "probable": 6000.0,
        "possible": 3000.0,
        "unresolved": 1000.0,
        "rejected": -10000.0,
    }.get(status, 0.0)

    score = status_tier + confidence * 1000.0
    if has_verified_manifest:
        score += 500.0
    if not is_multi_episode:
        score += 150.0
    if extension in {".mkv", ".mp4"}:
        score += 60.0
    elif extension in VIDEO_EXTENSIONS:
        score += 20.0

    # Seeders contribution (logarithmic saturation so high-seed dead/wrong files never override correctness)
    score += min(math.log1p(max(0, seeders)) * 45.0, 320.0)

    # Quality preference
    q_rank = {"2160p": 3, "1080p": 4, "720p": 2, "480p": 1}
    if preferred_quality and resolution and resolution.lower() == preferred_quality.lower():
        score += 90.0
    else:
        score += q_rank.get(resolution.lower(), 1) * 18.0

    # Size efficiency: favor realistic single-episode sizes (150 MiB .. 3.5 GiB) cleanly
    if file_size_bytes > 0:
        mb = file_size_bytes / (1024 * 1024)
        if 120 <= mb <= 4000:
            score += 40.0
        elif mb < 30:
            score -= 80.0  # suspiciously tiny sample/corrupt file

    return round(score, 2)


def evaluate_manifest_files_for_episode(
    *,
    identity: MediaIdentity,
    release: ScrapedItem,
    parsed_release: ParsedRelease,
    manifest: TorrentManifestResult,
    target_season: int,
    target_episode: int,
    target_abs_episode: int | None,
    regular_season_count: int,
    is_single_season: bool,
    preferred_quality: str = "1080p",
) -> tuple[list[ResolvedFileCandidate], list[ResolvedFileCandidate], int]:
    """Evaluate every file inside a verified torrent manifest against the target episode.

    Returns ``(playable_candidates, unresolved_candidates, rejected_count)``.
    """
    analysis = _analyze_sibling_sequences(manifest.files, identity)
    folder_stats: dict[str, dict[str, Any]] = analysis["folder_stats"]
    parsed_cache: dict[int, ParsedMediaPath] = analysis["parsed_cache"]

    info_hash = normalize_info_hash(manifest.info_hash or release.info_hash, release.magnet_uri)
    tech_rel = infer_technical_metadata(release.release_title)
    total_video_files = sum(1 for f in manifest.files if f.file_type == "video")
    is_batch_source = total_video_files > 1 or release.classification in {
        "season_pack",
        "complete_series",
        "multi_episode",
    }

    playable: list[ResolvedFileCandidate] = []
    unresolved: list[ResolvedFileCandidate] = []
    rejected_count = 0

    for mf in manifest.files:
        parsed_file = parsed_cache[id(mf)]
        evidence: list[str] = ["canonical show matched", "verified torrent manifest"]
        contradictions: list[str] = []

        # Hard reject non-video files (subtitles, images, nfo, etc.)
        if mf.file_type != "video":
            rejected_count += 1
            continue

        # Hard reject extras, trailers, clean openings/endings, recaps
        if parsed_file.is_extra_or_non_episode:
            rejected_count += 1
            continue

        # Ambiguous Disc/Part/BDMV containers must never be guessed
        if parsed_file.is_ambiguous_container:
            unresolved.append(
                ResolvedFileCandidate(
                    candidate_id=f"{info_hash}:{mf.torrent_file_index}",
                    info_hash=info_hash,
                    magnet_uri=release.magnet_uri,
                    torrent_url=release.torrent_url,
                    release_title=release.release_title,
                    provider_id=release.provider_id,
                    provider_name=release.provider_name,
                    torrent_file_index=mf.torrent_file_index,
                    file_path=mf.relative_path,
                    filename=mf.filename,
                    file_size_bytes=mf.size_bytes,
                    file_size_label=mf.size_label or format_bytes_label(mf.size_bytes),
                    confidence=0.25,
                    status="unresolved",
                    can_auto_play=False,
                    source_kind=manifest.source_kind,
                    is_batch_source=is_batch_source,
                    resolution=release.resolution or tech_rel["resolution"],
                    codec=release.codec or tech_rel["codec"],
                    subtitle_lang=release.subtitle_lang or tech_rel["subtitle_lang"],
                    seeders=release.seeders,
                    leechers=release.leechers,
                    score=_build_candidate_score(
                        status="unresolved",
                        confidence=0.25,
                        has_verified_manifest=True,
                        is_multi_episode=False,
                        extension=parsed_file.extension,
                        seeders=release.seeders,
                        resolution=release.resolution or tech_rel["resolution"],
                        file_size_bytes=mf.size_bytes,
                        preferred_quality=preferred_quality,
                    ),
                    evidence=evidence,
                    contradictions=[parsed_file.ambiguity_reason or "ambiguous container layout"],
                )
            )
            continue

        # Special vs Regular Episode hard boundary
        want_special = target_season == 0
        if parsed_file.is_special != want_special:
            rejected_count += 1
            continue

        # Season check (file-level folder/tag first, fallback to release season)
        eff_season = parsed_file.season
        season_explicit = eff_season is not None
        if eff_season is None and parsed_release.season_number is not None:
            eff_season = parsed_release.season_number

        # Detect absolute numbering match!
        matched_via_absolute = False
        ep_match = False
        ep_is_multi = False

        if want_special:
            if parsed_file.episode is not None:
                if parsed_file.episode_end is not None:
                    ep_match = parsed_file.episode <= target_episode <= parsed_file.episode_end
                    ep_is_multi = ep_match and (parsed_file.episode_end > parsed_file.episode)
                else:
                    ep_match = parsed_file.episode == target_episode
        else:
            # 1) Relative season+episode check
            if eff_season is not None and eff_season != target_season:
                # Could still match ONLY IF file has NO explicit SxxExx season tag (just folder/release) or wait:
                # If file explicitly says S02E07 when we want S01E07 -> HARD REJECT!
                if parsed_file.season is not None and parsed_file.season != target_season:
                    # Wait: allow Season 1 + absolute ep number ONLY if eff_season was absent; explicit S02 != S01 is rejected!
                    rejected_count += 1
                    continue

            if parsed_file.episode is not None:
                if parsed_file.episode_end is not None:
                    if parsed_file.episode <= target_episode <= parsed_file.episode_end:
                        ep_match = True
                        ep_is_multi = parsed_file.episode_end > parsed_file.episode
                    elif (
                        target_abs_episode is not None
                        and not parsed_file.is_special
                        and parsed_file.season is None
                        and parsed_file.episode <= target_abs_episode <= parsed_file.episode_end
                    ):
                        ep_match = True
                        ep_is_multi = True
                        matched_via_absolute = True
                else:
                    if parsed_file.episode == target_episode:
                        ep_match = True
                    elif (
                        target_abs_episode is not None
                        and target_abs_episode != target_episode
                        and parsed_file.episode == target_abs_episode
                        and parsed_file.season in (None, 1)
                    ):
                        ep_match = True
                        matched_via_absolute = True

            # Inherit episode number ONLY for true single-video torrents whose release title explicitly names SxxExx / Ep
            if not ep_match and total_video_files == 1 and parsed_file.episode is None:
                if parsed_release.classification in {"episode", "multi_episode"} and parsed_release.episode_number is not None:
                    rel_season = parsed_release.season_number
                    rel_ep = parsed_release.episode_number
                    if rel_season is not None and rel_season != target_season:
                        rejected_count += 1
                        continue
                    if parsed_release.first_episode and parsed_release.last_episode and parsed_release.last_episode > parsed_release.first_episode:
                        if parsed_release.first_episode <= target_episode <= parsed_release.last_episode:
                            ep_match = True
                            ep_is_multi = True
                    elif rel_ep == target_episode:
                        ep_match = True
                    elif target_abs_episode is not None and rel_ep == target_abs_episode and rel_season in (None, 1):
                        ep_match = True
                        matched_via_absolute = True

        if not ep_match:
            rejected_count += 1
            continue

        # Season conflict on release title (e.g., S01 file inside torrent explicitly titled S02? Wait - check release mismatch only if single-file torrent!)
        if (
            total_video_files == 1
            and parsed_release.season_number is not None
            and parsed_release.season_number != target_season
            and not matched_via_absolute
        ):
            rejected_count += 1
            continue

        # Compute granular confidence & evidence
        confidence = 0.62
        if matched_via_absolute:
            evidence.append(
                f"absolute episode {target_abs_episode} mapped to S{target_season:02d}E{target_episode:02d}"
            )
            confidence += 0.30
        elif parsed_file.parse_method in {"sxxexx", "nxnn", "season_episode_words"}:
            evidence.append(f"explicit S{target_season:02d}E{target_episode:02d} match")
            confidence += 0.34
        elif ep_is_multi:
            evidence.append(f"multi-episode file contains episode {target_episode}")
            contradictions.append("multi-episode range file requires explicit user selection")
            confidence += 0.16
        else:
            evidence.append(f"explicit episode {target_episode} matched")
            confidence += 0.24

        if parsed_file.folder_season is not None and parsed_file.folder_season == target_season:
            evidence.append(f"season {target_season} folder matched")
            confidence += 0.08
        elif eff_season == target_season:
            evidence.append(f"season {target_season} context matched")
            confidence += 0.06
        elif not want_special and not matched_via_absolute and not season_explicit:
            if is_single_season:
                evidence.append("single-season show context")
                confidence += 0.08
            else:
                # Multi-season show without explicit season on file or folder or absolute offset
                if parsed_release.season_number == target_season:
                    evidence.append(f"batch release specifies season {target_season}")
                    confidence += 0.07
                else:
                    contradictions.append(
                        f"missing explicit season tag for {regular_season_count}-season show"
                    )
                    confidence -= 0.12

        folder_key = "/".join(parsed_file.folders)
        f_stat = folder_stats.get(folder_key)
        if f_stat and f_stat.get("is_sequential"):
            evidence.append("sibling episode sequence verified")
            confidence += 0.06

        if total_video_files == 1:
            evidence.append("single-video verified torrent")
            confidence += 0.04

        if parsed_file.version > 1:
            evidence.append(f"version v{parsed_file.version}")

        # Tech metadata derived from filename first, fallback release title
        tech_file = infer_technical_metadata(mf.filename)
        eff_res = tech_file["resolution"] or release.resolution or tech_rel["resolution"]
        eff_codec = tech_file["codec"] or release.codec or tech_rel["codec"]
        eff_sub = tech_file["subtitle_lang"] or release.subtitle_lang or tech_rel["subtitle_lang"]

        confidence = max(0.0, min(0.99, round(confidence, 3)))

        if ep_is_multi:
            status = "probable"
        elif confidence >= 0.85 and not contradictions:
            status = "confirmed"
        elif confidence >= 0.68:
            status = "probable"
        else:
            status = "possible"

        can_auto_play = status == "confirmed" and len(contradictions) == 0

        playable.append(
            ResolvedFileCandidate(
                candidate_id=f"{info_hash}:{mf.torrent_file_index}",
                info_hash=info_hash,
                magnet_uri=release.magnet_uri,
                torrent_url=release.torrent_url,
                release_title=release.release_title,
                provider_id=release.provider_id,
                provider_name=release.provider_name,
                torrent_file_index=mf.torrent_file_index,
                file_path=mf.relative_path,
                filename=mf.filename,
                file_size_bytes=mf.size_bytes,
                file_size_label=mf.size_label or format_bytes_label(mf.size_bytes),
                confidence=confidence,
                status=status,
                can_auto_play=can_auto_play,
                source_kind=manifest.source_kind,
                is_batch_source=is_batch_source,
                resolution=eff_res,
                codec=eff_codec,
                subtitle_lang=eff_sub,
                seeders=release.seeders,
                leechers=release.leechers,
                score=_build_candidate_score(
                    status=status,
                    confidence=confidence,
                    has_verified_manifest=True,
                    is_multi_episode=ep_is_multi,
                    extension=parsed_file.extension,
                    seeders=release.seeders,
                    resolution=eff_res,
                    file_size_bytes=mf.size_bytes,
                    preferred_quality=preferred_quality,
                ),
                evidence=evidence,
                contradictions=contradictions,
            )
        )

    return (playable, unresolved, rejected_count)


# ---------------------------------------------------------------------------
# Stage 6: C++ Libtorrent Subprocess Magnet Metadata Helper
# ---------------------------------------------------------------------------


def make_intezam_magnet_fetcher(
    binary_finder: Callable[[], Path | None],
) -> Callable[[str, float, str], TorrentManifestResult]:
    """Build a callback using compiled ``intezam --metadata-only --manifest-json`` for magnets."""

    def _fetch(magnet_uri: str, timeout: float, expected_info_hash: str) -> TorrentManifestResult:
        binary = binary_finder()
        norm_hash = normalize_info_hash(expected_info_hash, magnet_uri)
        if binary is None:
            return TorrentManifestResult(
                status="unavailable",
                info_hash=norm_hash,
                source_kind="libtorrent_magnet",
                error=None,
                inspection_note="Intezam C++ binary unavailable for magnet metadata retrieval.",
            )

        timeout_clamped = max(3, min(120, int(math.ceil(timeout))))
        cmd = [
            str(binary),
            "--metadata-only",
            "--manifest-json",
            "--timeout",
            str(timeout_clamped),
        ]
        try:
            proc = subprocess.run(
                cmd,
                input=f"{magnet_uri.strip()}\n",
                text=True,
                capture_output=True,
                timeout=timeout_clamped + 5,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return TorrentManifestResult(
                status="unavailable",
                info_hash=norm_hash,
                source_kind="libtorrent_magnet",
                error="Timed out retrieving magnet metadata from peers.",
                inspection_note="Metadata-only peer discovery timed out without video download.",
            )
        except OSError as exc:
            return TorrentManifestResult(
                status="error",
                info_hash=norm_hash,
                source_kind="libtorrent_magnet",
                error=f"Failed to invoke intezam metadata helper: {exc}",
                inspection_note="Subprocess invocation error during magnet inspection.",
            )

        payload_text = (proc.stdout or "").strip()
        data: dict[str, Any] | None = None
        if payload_text:
            try:
                parsed = json.loads(payload_text)
                if isinstance(parsed, dict):
                    data = parsed
            except json.JSONDecodeError:
                for line in reversed(payload_text.splitlines()):
                    line_s = line.strip()
                    if line_s.startswith("{") and line_s.endswith("}"):
                        try:
                            parsed = json.loads(line_s)
                            if isinstance(parsed, dict):
                                data = parsed
                                break
                        except json.JSONDecodeError:
                            continue

        if data is None or not data.get("ok"):
            err_msg = (
                (data or {}).get("error")
                or (proc.stderr or "").strip()
                or f"Metadata process exited with code {proc.returncode}"
            )
            return TorrentManifestResult(
                status="unavailable" if "timeout" in str(err_msg).lower() else "error",
                info_hash=norm_hash,
                source_kind="libtorrent_magnet",
                error=str(err_msg),
                inspection_note="Magnet metadata could not be retrieved without payload downloads.",
            )

        res_hash = normalize_info_hash(str(data.get("info_hash") or ""), magnet_uri) or norm_hash
        if norm_hash and res_hash and len(norm_hash) == 40 and res_hash != norm_hash:
            return TorrentManifestResult(
                status="error",
                info_hash=res_hash,
                source_kind="libtorrent_magnet",
                error="Retrieved magnet metadata info-hash did not match requested torrent.",
                inspection_note="Rejected mismatched BTIH from peer metadata.",
            )

        raw_files = data.get("files") or []
        files: list[TorrentManifestFile] = []
        v_count = 0
        s_count = 0
        a_count = 0
        total_size = int(data.get("total_size_bytes") or 0)

        for entry in raw_files:
            if not isinstance(entry, dict):
                continue
            rel_path = str(entry.get("relative_path") or entry.get("filename") or "").replace("\\", "/").strip("/")
            fname = str(entry.get("filename") or posixpath.basename(rel_path) or rel_path)
            sz = int(entry.get("size_bytes") or 0)
            idx = int(entry.get("torrent_file_index") if entry.get("torrent_file_index") is not None else 0)
            ext = posixpath.splitext(fname)[1].lower()
            if ext in VIDEO_EXTENSIONS:
                ftype = "video"
                v_count += 1
            elif ext in SUBTITLE_EXTENSIONS:
                ftype = "subtitle"
                s_count += 1
            else:
                ftype = "auxiliary"
                a_count += 1
            files.append(
                TorrentManifestFile(
                    relative_path=rel_path,
                    filename=fname,
                    size_bytes=sz,
                    size_label=format_bytes_label(sz),
                    file_type=ftype,
                    extension=ext,
                    torrent_file_index=idx,
                )
            )

        if total_size <= 0:
            total_size = sum(f.size_bytes for f in files)

        return TorrentManifestResult(
            status="verified",
            info_hash=res_hash,
            source_kind="libtorrent_magnet",
            cached=False,
            torrent_name=str(data.get("torrent_name") or ""),
            total_files=len(files),
            total_size_bytes=total_size,
            total_size_label=format_bytes_label(total_size),
            video_count=v_count,
            subtitle_count=s_count,
            auxiliary_count=a_count,
            inspection_note="Verified via libtorrent metadata-only peer discovery (0 bytes downloaded).",
            files=files,
        )

    return _fetch


# ---------------------------------------------------------------------------
# Stage 7: Episode Resolution Engine Orchestrator
# ---------------------------------------------------------------------------


class EpisodeResolver:
    """Progressive, deterministic Episode-to-Torrent File Resolution Engine."""

    def __init__(
        self,
        discovery_engine: MultiProviderDiscoveryEngine,
        manifest_inspector: TorrentManifestInspector,
    ) -> None:
        self.discovery_engine = discovery_engine
        self.manifest_inspector = manifest_inspector
        self._cancel_lock = threading.Lock()
        self._cancel_events: dict[str, threading.Event] = {}

    def cancel(self, request_id: str = "") -> bool:
        """Signal cooperative cancellation for an active resolution request (or all active requests)."""
        with self._cancel_lock:
            if request_id:
                ev = self._cancel_events.get(request_id)
                if ev is not None:
                    ev.set()
                    return True
                return False
            any_set = False
            for ev in self._cancel_events.values():
                ev.set()
                any_set = True
            return any_set

    def _register_cancel_token(self, request_id: str) -> threading.Event:
        ev = threading.Event()
        with self._cancel_lock:
            self._cancel_events[request_id] = ev
        return ev

    def _unregister_cancel_token(self, request_id: str) -> None:
        with self._cancel_lock:
            self._cancel_events.pop(request_id, None)

    @staticmethod
    def _lookup_episode_metadata(
        identity: MediaIdentity,
        season: int,
        episode: int,
    ) -> EpisodeMetadata | None:
        for s_meta in identity.seasons:
            if s_meta.season_number == season:
                for ep in s_meta.episodes:
                    if ep.episode_number == episode:
                        return ep
        return None

    @staticmethod
    def _precheck_release_candidate(
        *,
        identity: MediaIdentity,
        release: ScrapedItem,
        parsed_release: ParsedRelease,
        target_season: int,
        target_episode: int,
        target_abs_episode: int | None,
    ) -> tuple[bool, float, str]:
        """Cheap Phase A heuristic pre-filter & priority score before inspecting torrent manifests.

        Returns ``(keep, priority_score, reject_reason)``.
        """
        spinoff_err = _detect_spinoff_contradiction(identity, release.release_title)
        if spinoff_err:
            return (False, 0.0, spinoff_err)

        if release.match_bucket == "rejected":
            return (False, 0.0, release.match_reason or "rejected by show matcher")

        want_special = target_season == 0
        if parsed_release.classification == "special" and not want_special:
            return (False, 0.0, "special/OVA release does not match regular episode")

        if parsed_release.classification == "movie" and identity.media_type != "movie":
            return (False, 0.0, "feature film release does not match episodic series")

        # Hard episode/season elimination on clearly scoped single-episode releases
        if parsed_release.classification == "episode":
            rel_ep = parsed_release.episode_number
            rel_s = parsed_release.season_number
            if rel_s is not None and rel_s != target_season:
                # Could be S1 covering absolute ep? Only if rel_s == 1 and target_abs_episode matches rel_ep
                if not (rel_s == 1 and target_abs_episode is not None and rel_ep == target_abs_episode):
                    return (False, 0.0, f"release specifies season {rel_s} (target season {target_season})")

            if rel_ep is not None and not want_special:
                matches_rel = rel_ep == target_episode
                matches_abs = (
                    target_abs_episode is not None
                    and rel_ep == target_abs_episode
                    and rel_s in (None, 1)
                )
                if not (matches_rel or matches_abs):
                    return (False, 0.0, f"release specifies episode {rel_ep} (target {target_episode})")
            elif want_special and rel_ep is not None and rel_ep != target_episode:
                return (False, 0.0, f"special number {rel_ep} != {target_episode}")

        elif parsed_release.classification == "multi_episode":
            if parsed_release.season_number is not None and parsed_release.season_number != target_season:
                return (False, 0.0, "multi-episode season mismatch")
            f_ep = parsed_release.first_episode
            l_ep = parsed_release.last_episode
            if f_ep is not None and l_ep is not None:
                in_rel = f_ep <= target_episode <= l_ep
                in_abs = (
                    target_abs_episode is not None
                    and parsed_release.season_number in (None, 1)
                    and f_ep <= target_abs_episode <= l_ep
                )
                if not (in_rel or in_abs):
                    return (False, 0.0, f"multi-episode range {f_ep}-{l_ep} excludes target")

        elif parsed_release.classification == "season_pack":
            # Only reject season packs whose single explicit season clearly excludes target season
            # unless it mentions multi-season range
            rel_s = parsed_release.season_number
            if rel_s is not None and rel_s != target_season:
                if not re.search(r"s(?:eason)?\s*\d+\s*[-~]\s*s?\d+|\bcomplete\b", release.release_title, re.IGNORECASE):
                    return (False, 0.0, f"season {rel_s} pack excludes season {target_season}")

        # Prioritize direct single-episode hits over multi-episode over season packs over complete series
        prio = 0.0
        if parsed_release.classification == "episode":
            prio += 500.0
        elif parsed_release.classification == "multi_episode":
            prio += 320.0
        elif parsed_release.classification == "season_pack":
            prio += 240.0
        elif parsed_release.classification == "complete_series":
            prio += 180.0
        else:
            prio += 120.0

        if release.torrent_url:
            prio += 90.0  # HTTP .torrent metadata is fast & deterministic
        prio += min(math.log1p(max(0, release.seeders)) * 25.0, 200.0)
        if parsed_release.season_number == target_season:
            prio += 60.0
        return (True, prio, "")

    @staticmethod
    def _deduplicate_releases(items: list[ScrapedItem]) -> tuple[list[ScrapedItem], int]:
        """Deduplicate torrents by lowercase 40-char info_hash while merging provider mirrors."""
        by_key: dict[str, ScrapedItem] = {}
        ordered: list[ScrapedItem] = []
        dup_count = 0

        for item in items:
            ih = normalize_info_hash(item.info_hash, item.magnet_uri or item.raw_link)
            if not item.magnet_uri and ih:
                pass
            dedup_key = f"btih:{ih}" if ih else f"link:{(item.magnet_uri or item.torrent_url or item.release_title).strip().lower()}"
            existing = by_key.get(dedup_key)
            if existing is None:
                # Preserve exact original magnet_uri, store normalized info_hash if empty
                if ih and not item.info_hash:
                    item.info_hash = ih
                by_key[dedup_key] = item
                ordered.append(item)
            else:
                dup_count += 1
                if item.provider_name and item.provider_name not in existing.mirror_providers and item.provider_name != existing.provider_name:
                    existing.mirror_providers.append(item.provider_name)
                if not existing.torrent_url and item.torrent_url:
                    existing.torrent_url = item.torrent_url
                if not existing.magnet_uri and item.magnet_uri:
                    existing.magnet_uri = item.magnet_uri
                if item.seeders > existing.seeders:
                    existing.seeders = item.seeders
                    existing.leechers = max(existing.leechers, item.leechers)

        return (ordered, dup_count)

    def resolve_episode(
        self,
        identity: MediaIdentity,
        season: int,
        episode: int,
        options: dict[str, Any] | None = None,
        *,
        injected_releases: list[ScrapedItem] | None = None,
    ) -> EpisodeResolutionResult:
        """Resolve an official episode to exact verified torrent file candidates."""
        t_start = time.perf_counter()
        opts = dict(options or {})
        mode = str(opts.get("mode") or "live").strip().lower()
        demo_mode = mode in {"mock", "mock data", "demo"}
        request_id = str(opts.get("request_id") or f"epres-{uuid.uuid4().hex[:12]}")
        max_manifest_fetches = max(1, min(20, int(opts.get("max_manifest_fetches") or 8)))
        allow_magnet_peers = bool(
            opts.get("allow_magnet_peers")
            if "allow_magnet_peers" in opts
            else (not demo_mode and self.manifest_inspector.magnet_metadata_fetcher is not None)
        )
        magnet_timeout = float(opts.get("magnet_timeout") or 12.0)
        force_refresh = bool(opts.get("force_refresh", False))
        preferred_quality = str(opts.get("preferred_quality") or "1080p")
        max_workers = max(1, min(4, int(opts.get("max_workers") or 4)))
        inspect_timeout_budget = float(opts.get("total_timeout") or 30.0)

        cancel_ev = self._register_cancel_token(request_id)
        errors: list[str] = []

        try:
            target_abs, reg_season_count, is_single_season = _compute_absolute_episode_and_regular_counts(
                identity, season, episode
            )
            ep_meta = self._lookup_episode_metadata(identity, season, episode)
            episode_title = ep_meta.title if ep_meta and ep_meta.title else f"Episode {episode}"

            # Discovery phase
            raw_items: list[ScrapedItem] = []
            if injected_releases is not None:
                raw_items = list(injected_releases)
            else:
                filters = SourceSearchFilters(
                    category="anime_all" if identity.media_type == "anime" else "all",
                    imdb_id=identity.imdb_id,
                )
                discovered, statuses, _, _ = self.discovery_engine.discover_for_show(
                    identity,
                    page=1,
                    demo_mode=demo_mode,
                    force_refresh=force_refresh,
                    filters=filters,
                )
                raw_items.extend(discovered)
                for st in statuses:
                    if st.status == "error" and st.error:
                        errors.append(f"{st.provider_name}: {st.error}")

            if cancel_ev.is_set():
                return self._build_cancelled_result(
                    identity, season, episode, episode_title, target_abs, request_id, t_start, errors
                )

            # Deduplicate across mirrors by BTIH
            unique_items, dedup_count = self._deduplicate_releases(raw_items)

            # Phase A: Cheap Heuristic Ranking & Rejection
            t_rank_start = time.perf_counter()
            shortlist: list[tuple[float, ScrapedItem, ParsedRelease]] = []
            rejected_count = 0

            for rel in unique_items:
                parsed_rel = parse_release_title(rel.release_title)
                keep, prio, _reason = self._precheck_release_candidate(
                    identity=identity,
                    release=rel,
                    parsed_release=parsed_rel,
                    target_season=season,
                    target_episode=episode,
                    target_abs_episode=target_abs,
                )
                if not keep:
                    rejected_count += 1
                    continue
                shortlist.append((prio, rel, parsed_rel))

            shortlist.sort(key=lambda tup: tup[0], reverse=True)
            ranking_ms = (time.perf_counter() - t_rank_start) * 1000.0

            # Separate into bounded inspection queue vs remaining backlog
            to_inspect = shortlist[:max_manifest_fetches]
            remaining_count = max(0, len(shortlist) - len(to_inspect))

            # Phase B/C: Bounded Parallel Manifest Inspection (cache hits first!)
            t_manifest_start = time.perf_counter()
            cache_hits = 0
            cache_misses = 0
            manifest_requests = 0

            all_playable: list[ResolvedFileCandidate] = []
            all_unresolved: list[ResolvedFileCandidate] = []

            def _inspect_one(item: ScrapedItem) -> tuple[ScrapedItem, TorrentManifestResult, bool]:
                ih = normalize_info_hash(item.info_hash, item.magnet_uri)
                # Track whether this was already cached cleanly in manifest_inspector
                pre_cached = None if force_refresh else self.manifest_inspector._get_cached(ih, item.torrent_url)
                if pre_cached is not None:
                    return (item, pre_cached, True)
                res = self.manifest_inspector.inspect(
                    torrent_url=item.torrent_url,
                    magnet_uri=item.magnet_uri,
                    info_hash=ih,
                    force_refresh=force_refresh,
                    allow_magnet_peers=allow_magnet_peers,
                    magnet_timeout=magnet_timeout,
                )
                return (item, res, bool(res.cached))

            if to_inspect and not cancel_ev.is_set():
                with ThreadPoolExecutor(max_workers=max_workers) as pool:
                    future_map = {
                        pool.submit(_inspect_one, rel): (rel, parsed_rel)
                        for _prio, rel, parsed_rel in to_inspect
                    }
                    try:
                        for future in as_completed(future_map, timeout=inspect_timeout_budget):
                            if cancel_ev.is_set():
                                break
                            if (time.perf_counter() - t_manifest_start) > inspect_timeout_budget:
                                errors.append("Bounded manifest inspection budget reached.")
                                break
                            rel, parsed_rel = future_map[future]
                            try:
                                _, manifest, was_cached = future.result()
                            except Exception as exc:
                                errors.append(f"Manifest check failed for {rel.release_title[:48]}: {exc}")
                                continue

                            if was_cached:
                                cache_hits += 1
                            else:
                                cache_misses += 1
                                manifest_requests += 1

                            if manifest.status == "verified" and manifest.files:
                                p_cands, u_cands, r_cnt = evaluate_manifest_files_for_episode(
                                    identity=identity,
                                    release=rel,
                                    parsed_release=parsed_rel,
                                    manifest=manifest,
                                    target_season=season,
                                    target_episode=episode,
                                    target_abs_episode=target_abs,
                                    regular_season_count=reg_season_count,
                                    is_single_season=is_single_season,
                                    preferred_quality=preferred_quality,
                                )
                                all_playable.extend(p_cands)
                                all_unresolved.extend(u_cands)
                                rejected_count += r_cnt
                            else:
                                # Manifest unavailable/error (e.g. offline magnet-only release) -> NEVER mark confirmed
                                ih = normalize_info_hash(manifest.info_hash or rel.info_hash, rel.magnet_uri)
                                tech = infer_technical_metadata(rel.release_title)
                                rel_sz = parse_size_to_bytes(rel.file_size or rel.size)
                                all_unresolved.append(
                                    ResolvedFileCandidate(
                                        candidate_id=f"{ih or rel.id}:unverified",
                                        info_hash=ih,
                                        magnet_uri=rel.magnet_uri,
                                        torrent_url=rel.torrent_url,
                                        release_title=rel.release_title,
                                        provider_id=rel.provider_id,
                                        provider_name=rel.provider_name,
                                        torrent_file_index=None,
                                        file_path="",
                                        filename="",
                                        file_size_bytes=rel_sz,
                                        file_size_label=rel.file_size or format_bytes_label(rel_sz),
                                        confidence=0.35 if manifest.status == "unavailable" else 0.15,
                                        status="unresolved",
                                        can_auto_play=False,
                                        source_kind=manifest.source_kind,
                                        is_batch_source=parsed_rel.classification in {"season_pack", "complete_series"},
                                        resolution=rel.resolution or tech["resolution"],
                                        codec=rel.codec or tech["codec"],
                                        subtitle_lang=rel.subtitle_lang or tech["subtitle_lang"],
                                        seeders=rel.seeders,
                                        leechers=rel.leechers,
                                        score=_build_candidate_score(
                                            status="unresolved",
                                            confidence=0.3,
                                            has_verified_manifest=False,
                                            is_multi_episode=False,
                                            extension="",
                                            seeders=rel.seeders,
                                            resolution=rel.resolution or tech["resolution"],
                                            file_size_bytes=rel_sz,
                                            preferred_quality=preferred_quality,
                                        ),
                                        evidence=["canonical show matched", "release title heuristic matched"],
                                        contradictions=[
                                            manifest.error
                                            or manifest.inspection_note
                                            or "torrent file manifest unverified (exact file index unknown)"
                                        ],
                                    )
                                )
                    except TimeoutError:
                        errors.append("Bounded manifest inspection budget reached.")

            manifest_fetch_ms = (time.perf_counter() - t_manifest_start) * 1000.0

            # Separate confirmed/probable into primary candidates, possible into possible_matches
            candidates = [c for c in all_playable if c.status in {"confirmed", "probable"}]
            possible_matches = [c for c in all_playable if c.status == "possible"]

            candidates.sort(key=lambda c: c.score, reverse=True)
            possible_matches.sort(key=lambda c: c.score, reverse=True)
            all_unresolved.sort(key=lambda c: c.score, reverse=True)

            total_ms = (time.perf_counter() - t_start) * 1000.0
            res_stage = "cancelled" if cancel_ev.is_set() else ("verified" if candidates else "completed")

            return EpisodeResolutionResult(
                show_id=identity.canonical_id,
                show_title=identity.official_title,
                season=season,
                episode=episode,
                episode_title=episode_title,
                absolute_episode=target_abs,
                resolution_stage=res_stage,
                candidates=candidates,
                possible_matches=possible_matches,
                unresolved_candidates=all_unresolved,
                rejected_count=rejected_count,
                has_more=remaining_count > 0,
                errors=errors,
                metrics={
                    "ranking_ms": round(ranking_ms, 2),
                    "manifest_fetch_ms": round(manifest_fetch_ms, 2),
                    "total_ms": round(total_ms, 2),
                    "cache_hits": cache_hits,
                    "cache_misses": cache_misses,
                    "manifest_requests": manifest_requests,
                    "deduplicated_torrents": dedup_count,
                    "candidates_considered": len(unique_items),
                    "candidates_inspected": len(to_inspect),
                    "cancelled": cancel_ev.is_set(),
                },
                request_id=request_id,
            )
        finally:
            self._unregister_cancel_token(request_id)

    @staticmethod
    def _build_cancelled_result(
        identity: MediaIdentity,
        season: int,
        episode: int,
        episode_title: str,
        target_abs: int | None,
        request_id: str,
        t_start: float,
        errors: list[str],
    ) -> EpisodeResolutionResult:
        return EpisodeResolutionResult(
            show_id=identity.canonical_id,
            show_title=identity.official_title,
            season=season,
            episode=episode,
            episode_title=episode_title,
            absolute_episode=target_abs,
            resolution_stage="cancelled",
            errors=errors,
            metrics={
                "ranking_ms": 0.0,
                "manifest_fetch_ms": 0.0,
                "total_ms": round((time.perf_counter() - t_start) * 1000.0, 2),
                "cache_hits": 0,
                "cache_misses": 0,
                "manifest_requests": 0,
                "deduplicated_torrents": 0,
                "candidates_considered": 0,
                "candidates_inspected": 0,
                "cancelled": True,
            },
            request_id=request_id,
        )
