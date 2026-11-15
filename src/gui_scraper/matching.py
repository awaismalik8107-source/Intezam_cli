"""Title normalization, confidence-based show identity matching, and episode/batch mapping engine."""

from __future__ import annotations

from dataclasses import dataclass
import re
import unicodedata

from .models import MediaIdentity, ReleaseMatchResult, TorrentRelease

_FILE_EXT_RE = re.compile(
    r"\.(?:mkv|mp4|avi|webm|m4v|ts|mov|iso|zip|7z|rar|torrent)$",
    re.IGNORECASE,
)
_LEADING_GROUPS_RE = re.compile(r"^(?:\s*\[[^\]]+\]\s*)+")
_FIRST_GROUP_RE = re.compile(r"^\s*\[([^\]]+)\]")
_CRC32_TAG_RE = re.compile(r"\s*\[[0-9a-f]{8}\]", re.IGNORECASE)
_YEAR_PARENS_RE = re.compile(r"[\[(](19\d{2}|20\d{2})[\])]")
_YEAR_TOKEN_RE = re.compile(r"\b(19\d{2}|20\d{2})\b")

_TRAILING_TECH_TAG_RE = re.compile(
    r"\s*[\[(](?![^\])]*(?:\b(?:batch|complete|season|series|pack|ep(?:isode)?)\b|\bs\d{1,2}\b|\d{1,3}\s*[-~]\s*\d{1,3}))"
    r"[^\])]*(?:2160p|1080p|720p|480p|4k|x26[45]|h\.?26[45]|hevc|avc|av1|10bit|8bit|hi10p|"
    r"web[- ]?dl|webrip|bdrip|bluray|blu-ray|remux|aac|flac|opus|ac3|eac3|ddp|dual[- ]?audio|"
    r"multi[- ]?(?:audio|sub(?:title)?s?)|multiple\s+subtitle|eng|jpn|sub|dub|raw|mkv|mp4)[^\])]*[\])]\s*$",
    re.IGNORECASE,
)
_BARE_TECH_TAIL_RE = re.compile(
    r"\s+\b(?:2160p|1080p|720p|480p|4k|web[- ]?dl|webrip|bdrip|brrip|bluray|blu-ray|dvdrip|hdrip|hdtv|remux|x26[45]|h\.?26[45]|hevc|avc|av1|10bit|aac|flac|dual[- ]?audio|multi[- ]?subs?)\b.*$",
    re.IGNORECASE,
)
_SPECIAL_RE = re.compile(
    r"(?:\s+[-–—:]\s*|\s+\b)(?:special(?:\s+pv|\s+ost|\s+original\s+soundtrack|\s+preview)?|"
    r"ova|oad|ona\s+special|movie|film|the\s+movie|pv\d*|ncop\d*|nced\d*|ost|original\s+soundtrack|recap|sp\d{1,2})\b.*$",
    re.IGNORECASE,
)

_SXXEXX_RE = re.compile(
    r"^(.*?)\s*(?:[-–—:]\s*|\s+)S(\d{1,2})\s*E(\d{1,3})(?:v\d+)?(?:\s*[-~]\s*E?(\d{1,3})(?:v\d+)?)?(?=\b|[\s[(._-]|$)",
    re.IGNORECASE,
)
_SEASON_EPISODE_WORDS_RE = re.compile(
    r"^(.*?)\s*(?:[-–—:]\s*|\s+)Season\s*(\d{1,2})\s*(?:[-–—:]?\s*)?(?:Episode|Ep\.?|E)\s*(\d{1,3})(?:v\d+)?(?:\s*[-~]\s*(?:Episode|Ep\.?|E)?\s*(\d{1,3})(?:v\d+)?)?(?=\b|[\s[(._-]|$)",
    re.IGNORECASE,
)
_MULTI_EP_RANGE_RE = re.compile(
    r"^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?\s*(?:Batch\s*)?(?:Ep(?:isodes?)?\.?\s*)?(\d{1,3})\s*[-~]\s*(\d{1,3})(?:\s*(?:Batch|Complete|END|Fin))?\s*[\])]?(?:\s*[\[(]?(?:Batch|Complete)[\])]?)?(?=$|\s|[\[(])",
    re.IGNORECASE,
)
_DASHED_EP_RE = re.compile(
    r"^(.*?)\s+[-–—]\s*(?:Ep(?:isode)?\.?\s*)?(\d{1,3})(?:v\d+)?(?=$|\s|[\[(])",
    re.IGNORECASE,
)
_EXPLICIT_EP_WORD_RE = re.compile(
    r"^(.*?)\s+(?:Episode|Ep\.?)\s*(\d{1,3})(?:v\d+)?(?=$|\s|[\[(])",
    re.IGNORECASE,
)
_COMPLETE_SERIES_RE = re.compile(
    r"^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Complete\s+Series(?:\s+Batch|\s+Pack)?|All\s+Seasons?(?:\s+Batch|\s+Pack)?|Full\s+Series(?:\s+Batch|\s+Pack)?)[\])]?(?=$|\s|[\[(])",
    re.IGNORECASE,
)
_SEASON_BATCH_RE = re.compile(
    r"^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Season\s*(\d{1,2})|S(\d{1,2})|(\d{1,2})(?:st|nd|rd|th)\s+Season)(?:\s*(?:[-–—]?\s*)?(?:Complete(?:\s+Season|\s+Batch|\s+Pack)?|Batch(?:\s+Pack)?|Season\s+Pack|Pack|BD(?:\s+Batch)?|Full\s+Season))?[\])]?(?=$|\s|[\[(])",
    re.IGNORECASE,
)
_GENERIC_BATCH_RE = re.compile(
    r"^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Complete\s*(?:Season|Batch|Pack)|Season\s*Pack|Batch\s*Pack|Batch|Complete)[\])]?(?=$|\s|[\[(])",
    re.IGNORECASE,
)
_PREFIX_SEASON_RE = re.compile(
    r"^(.*?)(?:\s+[-–—:]\s*|\s+)(?:Season\s*(\d{1,2})|S(\d{1,2})|(\d{1,2})(?:st|nd|rd|th)\s+Season|Part\s*(\d{1,2})|Cour\s*(\d{1,2}))$",
    re.IGNORECASE,
)


@dataclass(slots=True)
class ParsedRelease:
    """Normalized structural decomposition of a raw torrent release title."""

    raw_title: str
    show_title: str
    season_qualified_title: str
    alternative_title: str = ""
    release_group: str = ""
    release_year: int | None = None
    season_number: int | None = None
    episode_number: int | None = None
    first_episode: int | None = None
    last_episode: int | None = None
    classification: str = "unclassified"  # "episode" | "multi_episode" | "season_pack" | "complete_series" | "movie" | "special" | "unclassified"
    batch_label: str = ""
    resolution: str = ""
    codec: str = ""
    container: str = ""
    subtitle_lang: str = ""


def infer_technical_metadata(raw_title: str) -> dict[str, str]:
    """Extract resolution, video codec, container, and subtitle/audio tags from a raw title."""
    text = raw_title or ""
    ext_m = _FILE_EXT_RE.search(text)
    container = ext_m.group(0).lstrip(".").upper() if ext_m else ""
    if not container:
        if re.search(r"\bmkv\b", text, re.IGNORECASE):
            container = "MKV"
        elif re.search(r"\bmp4\b", text, re.IGNORECASE):
            container = "MP4"

    resolution = ""
    if re.search(r"\b(?:2160p|4k|3840x2160)\b", text, re.IGNORECASE):
        resolution = "2160p"
    elif re.search(r"\b(?:1080p|1920x1080)\b", text, re.IGNORECASE):
        resolution = "1080p"
    elif re.search(r"\b(?:720p|1280x720)\b", text, re.IGNORECASE):
        resolution = "720p"
    elif re.search(r"\b(?:480p|540p|848x480)\b", text, re.IGNORECASE):
        resolution = "480p"

    codec = ""
    if re.search(r"\b(?:hevc|x265|h\.?265)\b", text, re.IGNORECASE):
        codec = "HEVC / x265"
    elif re.search(r"\b(?:avc|x264|h\.?264)\b", text, re.IGNORECASE):
        codec = "H.264 / x264"
    elif re.search(r"\bav1\b", text, re.IGNORECASE):
        codec = "AV1"

    subtitle_lang = ""
    if re.search(r"\bdual[- ]?audio\b", text, re.IGNORECASE):
        subtitle_lang = "Dual-Audio"
    elif re.search(r"\b(?:multi[- ]?subs?|multiple\s+subtitle)\b", text, re.IGNORECASE):
        subtitle_lang = "Multi-Sub"
    elif re.search(r"\braw\b", text, re.IGNORECASE):
        subtitle_lang = "RAW"
    elif re.search(r"\b(?:eng(?:lish)?\s*sub|eng)\b", text, re.IGNORECASE):
        subtitle_lang = "ENG"

    return {
        "resolution": resolution,
        "codec": codec,
        "container": container,
        "subtitle_lang": subtitle_lang,
    }


def _strip_trailing_tech_tags(value: str) -> str:
    current = value.strip()
    previous = ""
    while current != previous:
        previous = current
        current = _TRAILING_TECH_TAG_RE.sub("", current).strip()
    current = _BARE_TECH_TAIL_RE.sub("", current).strip()
    return current


def _clean_prefix(value: str) -> str:
    normalized = value
    if " " not in normalized.strip() and normalized.count(".") >= 2:
        normalized = normalized.replace(".", " ").replace("_", " ")
    cleaned = _strip_trailing_tech_tags(normalized)
    had_tech_strip = cleaned != normalized.strip()
    cleaned = _YEAR_PARENS_RE.sub("", cleaned)
    if had_tech_strip:
        # Strip scene release year only when scene tech tags followed it (e.g. "It.2017.1080p.BRRip")
        m_yr = re.match(r"^(.{2,}?)\s+\b(?:19\d{2}|20\d{2})\s*$", cleaned)
        if m_yr:
            cleaned = m_yr.group(1)
    cleaned = re.sub(r"\s{2,}", " ", cleaned)
    cleaned = re.sub(r"[\s\-–—:]+$", "", cleaned)
    return cleaned.strip()


def _extract_season_from_prefix(raw_prefix: str) -> tuple[str, str, int | None]:
    cleaned = _clean_prefix(raw_prefix)
    m = _PREFIX_SEASON_RE.match(cleaned)
    if m:
        season_num = int(
            m.group(2) or m.group(3) or m.group(4) or m.group(5) or m.group(6)
        )
        base_title = _clean_prefix(m.group(1))
        if len(base_title) >= 2 and 1 <= season_num <= 30:
            return (base_title, cleaned, season_num)
    return (cleaned, cleaned, None)


def format_batch_label(
    classification: str,
    season: int | None,
    first_ep: int | None,
    last_ep: int | None,
) -> str:
    if classification == "complete_series":
        return "Complete-Series Pack"
    if classification == "multi_episode":
        if first_ep is not None and last_ep is not None:
            ep_range = f"Episodes {first_ep:02d}–{last_ep:02d}"
            return f"Season {season} · {ep_range} Range" if season is not None else f"{ep_range} Range"
        return "Multi-Episode Range"
    if classification == "season_pack":
        return f"Season {season} Full Batch" if season is not None else "Full-Season Batch"
    return ""


def parse_release_title(raw_title: str) -> ParsedRelease:
    """Parse an anime or TV torrent release title into structured show, season, episode, and batch metadata."""
    raw = (raw_title or "").strip()
    group_m = _FIRST_GROUP_RE.match(raw)
    release_group = group_m.group(1).strip() if group_m else ""
    tech = infer_technical_metadata(raw)

    alternative_title = ""
    working_raw = raw
    if "|" in raw:
        pipe_parts = [p.strip() for p in raw.split("|") if p.strip()]
        if len(pipe_parts) >= 2:
            working_raw = pipe_parts[0]
            alternative_title = " | ".join(pipe_parts[1:])

    year_m = _YEAR_PARENS_RE.search(working_raw)
    release_year: int | None = int(year_m.group(1)) if year_m else None

    cleaned = _FILE_EXT_RE.sub("", working_raw)
    cleaned = _LEADING_GROUPS_RE.sub("", cleaned)
    cleaned = _CRC32_TAG_RE.sub("", cleaned)
    cleaned = cleaned.replace("_", " ")
    if re.match(r"^[^\s]+\.[^\s]+\.[^\s]+", cleaned):
        cleaned = cleaned.replace(".", " ")

    if release_year is None:
        # Check for year right before SxxExx in scene releases (e.g. "Show 2023 S01E01")
        scene_year = re.search(r"\b(19\d{2}|20\d{2})\s+S\d{1,2}E\d{1,3}\b", cleaned, re.IGNORECASE)
        if scene_year and int(scene_year.group(1)) != 2099:
            release_year = int(scene_year.group(1))
        else:
            tech_year = re.search(
                r"\b(19\d{2}|20\d{2})\s+(?:2160p|1080p|720p|480p|4k|web[- ]?dl|webrip|bdrip|brrip|bluray|blu-ray|dvdrip|hdrip|hdtv|remux|x26[45]|h\.?26[45])\b",
                cleaned,
                re.IGNORECASE,
            )
            if tech_year and int(tech_year.group(1)) != 2099:
                release_year = int(tech_year.group(1))

    # Strip parenthesized language/audio/technical tags that are not episode/season/batch indicators
    cleaned = re.sub(
        r"\((?![^()]*\b(?:Season|S\d{1,2}|Ep(?:isode)?|Batch|Complete|Part|Cour)\b)[^()]*(?:Multi[- ]?Audio|Dual[- ]?Audio|German|Deutsch|English|Japanese|French|Spanish|Italian|Portuguese|Russian|1080p|720p|2160p|480p|AV1|HEVC|x26[45]|Opus|FLAC|AAC|BD|BluRay|WEB)[^()]*\)",
        "",
        cleaned,
        flags=re.IGNORECASE,
    ).strip()

    cleaned = _strip_trailing_tech_tags(cleaned)
    if release_year is not None:
        m_trailing_yr = re.match(rf"^(.{{2,}}?)\s+{release_year}\s*$", cleaned)
        if m_trailing_yr:
            cleaned = m_trailing_yr.group(1)
    cleaned = re.sub(r"\s{2,}", " ", cleaned).strip()

    # 1. Explicit SxxExx or "Season X Episode Y"
    ep_match = _SXXEXX_RE.match(cleaned) or _SEASON_EPISODE_WORDS_RE.match(cleaned)
    if ep_match:
        season = int(ep_match.group(2))
        first = int(ep_match.group(3))
        last = int(ep_match.group(4)) if ep_match.group(4) else first
        raw_pref = ep_match.group(1)
        if release_year is not None:
            raw_pref = re.sub(rf"\b{release_year}\b", "", raw_pref).strip()
        base_title = _clean_prefix(raw_pref) or cleaned
        if 1 <= first <= last and (last - first) <= 200:
            is_range = last > first
            cls_kind = "multi_episode" if is_range else "episode"
            return ParsedRelease(
                raw_title=raw,
                show_title=base_title,
                season_qualified_title=f"{base_title} Season {season}" if season > 1 else base_title,
                release_group=release_group,
                release_year=release_year,
                season_number=season,
                episode_number=None if is_range else first,
                first_episode=first,
                last_episode=last,
                classification=cls_kind,
                batch_label=format_batch_label(cls_kind, season, first, last) if is_range else "",
                **tech,
            )

    # 2. Multi-episode range: "Show - 01-12", "Show (Batch 01-24)"
    range_match = _MULTI_EP_RANGE_RE.match(cleaned)
    if range_match and not re.search(r"\b(?:season|part|cour)\s*$", range_match.group(1), re.IGNORECASE):
        first = int(range_match.group(2))
        last = int(range_match.group(3))
        if 1 <= first < last <= 300 and (last - first) <= 200:
            base_title, qual_title, season = _extract_season_from_prefix(range_match.group(1))
            return ParsedRelease(
                raw_title=raw,
                show_title=base_title or cleaned,
                season_qualified_title=qual_title or base_title or cleaned,
                release_group=release_group,
                release_year=release_year,
                season_number=season,
                episode_number=None,
                first_episode=first,
                last_episode=last,
                classification="multi_episode",
                batch_label=format_batch_label("multi_episode", season, first, last),
                **tech,
            )

    # 3. Dashed single episode: "Show - 01", "Show Season 2 - 04", "Show 2nd Season - 05"
    dashed_match = _DASHED_EP_RE.match(cleaned) or _EXPLICIT_EP_WORD_RE.match(cleaned)
    if dashed_match:
        ep_num = int(dashed_match.group(2))
        if 1 <= ep_num <= 500:
            base_title, qual_title, season = _extract_season_from_prefix(dashed_match.group(1))
            return ParsedRelease(
                raw_title=raw,
                show_title=base_title or cleaned,
                season_qualified_title=qual_title or base_title or cleaned,
                release_group=release_group,
                release_year=release_year,
                season_number=season if season is not None else 1,
                episode_number=ep_num,
                first_episode=ep_num,
                last_episode=ep_num,
                classification="episode",
                **tech,
            )

    # 4. Complete Series Pack
    comp_match = _COMPLETE_SERIES_RE.match(cleaned)
    if comp_match:
        base_title, qual_title, _ = _extract_season_from_prefix(comp_match.group(1))
        return ParsedRelease(
            raw_title=raw,
            show_title=base_title or cleaned,
            season_qualified_title=qual_title or base_title or cleaned,
            release_group=release_group,
            release_year=release_year,
            season_number=None,
            classification="complete_series",
            batch_label="Complete-Series Pack",
            **tech,
        )

    # 5. Season Batch ("Show - Season 1 Batch", "Show S02 Complete")
    s_batch = _SEASON_BATCH_RE.match(cleaned)
    if s_batch:
        season_num = int(s_batch.group(2) or s_batch.group(3) or s_batch.group(4))
        base_title = _clean_prefix(s_batch.group(1)) or cleaned
        return ParsedRelease(
            raw_title=raw,
            show_title=base_title,
            season_qualified_title=f"{base_title} Season {season_num}",
            release_group=release_group,
            release_year=release_year,
            season_number=season_num,
            classification="season_pack",
            batch_label=format_batch_label("season_pack", season_num, None, None),
            **tech,
        )

    # 6. Generic Batch ("Show [Batch]", "Show (Complete)")
    gen_batch = _GENERIC_BATCH_RE.match(cleaned)
    if gen_batch:
        base_title, qual_title, season = _extract_season_from_prefix(gen_batch.group(1))
        cls_kind = "season_pack" if season is not None else "complete_series"
        return ParsedRelease(
            raw_title=raw,
            show_title=base_title or cleaned,
            season_qualified_title=qual_title or base_title or cleaned,
            release_group=release_group,
            release_year=release_year,
            season_number=season,
            classification=cls_kind,
            batch_label=format_batch_label(cls_kind, season, None, None),
            **tech,
        )

    # 7. Special / OVA / Movie / OST
    if _SPECIAL_RE.search(cleaned):
        stripped = _SPECIAL_RE.sub("", cleaned).strip()
        base_title, qual_title, season = _extract_season_from_prefix(stripped)
        return ParsedRelease(
            raw_title=raw,
            show_title=base_title or _clean_prefix(cleaned) or raw,
            season_qualified_title=qual_title or base_title or raw,
            release_group=release_group,
            release_year=release_year,
            season_number=season,
            classification="special",
            **tech,
        )

    base_title, qual_title, season = _extract_season_from_prefix(cleaned)
    return ParsedRelease(
        raw_title=raw,
        show_title=base_title or _clean_prefix(cleaned) or raw,
        season_qualified_title=qual_title or base_title or raw,
        alternative_title=alternative_title,
        release_group=release_group,
        release_year=release_year,
        season_number=season,
        classification="unclassified",
        **tech,
    )


_STOPWORDS = {"the", "a", "an", "no", "of", "in", "on", "to", "and", "wa", "ni", "ga"}
_CONFLICT_SPINOFF_TOKENS = {
    "movie",
    "film",
    "boruto",
    "zero",
    "apocrypha",
    "kaleid",
    "grand",
    "strange",
    "gt",
    "kai",
    "daima",
    "stampede",
    "brotherhood",
    "shippuden",
    "chibi",
    "junior",
    "spin",
    "spinoff",
    "rock",
    "sd",
    "sennen",
    "kessen",
    "thousand",
    "blood",
}

_CANONICAL_ALIAS_CLUSTERS: tuple[tuple[str, ...], ...] = (
    ("dandadan", "dan da dan", "ダンダダン"),
    ("frieren", "sousou no frieren", "frieren beyond journeys end", "葬送のフリーレン"),
    ("your name", "kimi no na wa", "君の名は"),
    ("attack on titan", "shingeki no kyojin", "aot", "snk", "進撃の巨人"),
    ("one piece", "ワンピース"),
    ("naruto", "ナルト"),
    ("naruto shippuden", "naruto shippuuden", "ナルト 疾風伝"),
    ("bleach", "ブリーチ"),
    (
        "bleach thousand year blood war",
        "bleach sennen kessen hen",
        "bleach tybw",
        "千年血戦篇",
    ),
    ("delicious in dungeon", "dungeon meshi", "ダンジョン飯"),
    ("solo leveling", "ore dake level up na ken", "俺だけレベルアップな件"),
    ("the apothecary diaries", "kusuriya no hitorigoto", "薬屋のひとりごと"),
    ("orb on the movements of the earth", "chi chikyuu no undou ni tsuite", "チ 地球の運動について"),
    ("jujutsu kaisen", "jjk", "呪術廻戦"),
    ("demon slayer", "kimetsu no yaiba", "kny", "鬼滅の刃"),
    ("my hero academia", "boku no hero academia", "mha", "bnha", "僕のヒーローアカデミア"),
    ("fullmetal alchemist brotherhood", "fmab", "hagane no renkinjutsushi"),
    ("chainsaw man", "csm", "チェンソーマン"),
    ("one punch man", "opm", "ワンパンマン"),
)


def compact_alnum_key(text: str) -> str:
    """Return a lowercase, whitespace-free, punctuation-free key preserving Latin and CJK/Kana characters."""
    norm = unicodedata.normalize("NFKC", text or "").lower()
    norm = norm.replace("’", "").replace("‘", "").replace("'", "")
    return re.sub(r"[^\w]|_", "", norm)


def normalize_title_tokens(title: str) -> tuple[str, list[str]]:
    """Normalize a show title into a canonical string and significant token list."""
    text = unicodedata.normalize("NFKC", title or "").lower()
    text = text.replace("’", "'").replace("‘", "'").replace("'", "")
    text = re.sub(r"[^\w\s]|_", " ", text)
    tokens = [tok for tok in text.split() if tok]
    norm_str = " ".join(tokens)
    sig_tokens = [tok for tok in tokens if tok not in _STOPWORDS] or tokens
    return (norm_str, sig_tokens)


@dataclass(slots=True)
class NormalizedQuery:
    """Stage 3: Structured representation of a user search query with qualifiers and alias expansions."""

    raw_query: str
    core_query: str
    normalized_text: str
    compact_key: str
    tokens: list[str]
    significant_tokens: list[str]
    explicit_year: int | None = None
    media_qualifier: str | None = None  # "anime" | "live_action" | "movie" | "tv" | None
    country_qualifier: str | None = None  # "US" | "GB" | "JP" | "DE" | None
    is_short_ambiguous: bool = False
    expanded_aliases: list[str] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.expanded_aliases is None:
            self.expanded_aliases = []


def preprocess_search_query(raw_query: str) -> NormalizedQuery:
    """
    Stage 3 Query-Processing Pipeline:
    Normalizes Unicode/punctuation, extracts year/adaptation/country qualifiers,
    strips accidental release-group/tech tags, and expands known multilingual/spacing aliases.
    """
    raw = (raw_query or "").strip()
    if not raw:
        return NormalizedQuery(
            raw_query="",
            core_query="",
            normalized_text="",
            compact_key="",
            tokens=[],
            significant_tokens=[],
            is_short_ambiguous=False,
            expanded_aliases=[],
        )

    working = unicodedata.normalize("NFKC", raw).strip()
    # Strip leading [Group] or trailing file extension if user pasted a release name
    working = _FILE_EXT_RE.sub("", working)
    working = _LEADING_GROUPS_RE.sub("", working)
    working = _CRC32_TAG_RE.sub("", working).strip()

    media_qualifier: str | None = None
    country_qualifier: str | None = None
    explicit_year: int | None = None

    # 1. Extract media-type / adaptation qualifiers: (Live Action), [Anime], (Movie), (TV)
    if re.search(r"[\[(]?\blive[\s-]*action\b[\])]?", working, re.IGNORECASE):
        media_qualifier = "live_action"
        working = re.sub(r"[\[(]?\blive[\s-]*action(?:\s+series|\s+tv)?\b[\])]?", " ", working, flags=re.IGNORECASE).strip()
    elif re.search(r"[\[(]\s*anime\s*[\])]|(?<=\s)\banime\s*$", working, re.IGNORECASE):
        media_qualifier = "anime"
        working = re.sub(r"[\[(]\s*anime\s*[\])]|(?<=\s)\banime\s*$", " ", working, flags=re.IGNORECASE).strip()
    elif re.search(r"[\[(]\s*movie\s*[\])]", working, re.IGNORECASE):
        media_qualifier = "movie"
        working = re.sub(r"[\[(]\s*movie\s*[\])]", " ", working, flags=re.IGNORECASE).strip()

    # 2. Extract country qualifiers: (US), (UK), US, UK at end of query
    country_m = re.search(r"(?:[\[(]\s*(US|USA|UK|GB|JP)\s*[\])]|(?<=\s)(US|UK)\s*$)", working, re.IGNORECASE)
    if country_m:
        raw_c = (country_m.group(1) or country_m.group(2) or "").upper()
        country_qualifier = "GB" if raw_c in {"UK", "GB"} else ("US" if raw_c in {"US", "USA"} else raw_c)
        working = (working[: country_m.start()] + " " + working[country_m.end() :]).strip()

    # 3. Extract explicit release year (only when accompanied by another title token so "1984" stays intact)
    year_m = re.search(r"(?:[\[(]\s*(19\d{2}|20\d{2})\s*[\])]|(?<=\s)(19\d{2}|20\d{2})\s*$)", working)
    if year_m:
        candidate_year = int(year_m.group(1) or year_m.group(2))
        remainder = (working[: year_m.start()] + " " + working[year_m.end() :]).strip(" -–—:()")
        if len(remainder) >= 2:
            explicit_year = candidate_year
            working = remainder

    working = _strip_trailing_tech_tags(working)
    core_query = re.sub(r"\s{2,}", " ", working).strip(" -–—:") or raw

    norm_str, sig_tokens = normalize_title_tokens(core_query)
    all_tokens = [tok for tok in norm_str.split() if tok]
    c_key = compact_alnum_key(core_query)

    is_short = bool(
        c_key
        and (
            len(c_key) <= 4
            or (len(all_tokens) == 1 and len(all_tokens[0]) <= 4)
        )
        and not any(ord(ch) > 0x3000 for ch in c_key)
    )

    expanded: list[str] = []
    seen_exp_norm: set[str] = {norm_str}
    for cluster in _CANONICAL_ALIAS_CLUSTERS:
        cluster_keys = {compact_alnum_key(alias) for alias in cluster}
        if c_key in cluster_keys or norm_str in cluster:
            for alias in cluster:
                a_norm, _ = normalize_title_tokens(alias)
                if a_norm and a_norm not in seen_exp_norm:
                    seen_exp_norm.add(a_norm)
                    expanded.append(alias)

    return NormalizedQuery(
        raw_query=raw,
        core_query=core_query,
        normalized_text=norm_str,
        compact_key=c_key,
        tokens=all_tokens,
        significant_tokens=sig_tokens,
        explicit_year=explicit_year,
        media_qualifier=media_qualifier,
        country_qualifier=country_qualifier,
        is_short_ambiguous=is_short,
        expanded_aliases=expanded,
    )


@dataclass(slots=True)
class CandidateRankResult:
    """Stage 4: Explainable ranking evaluation for a candidate MediaIdentity."""

    score: float
    confidence_tier: str  # "high" | "medium" | "low" | "rejected"
    reason: str
    matched_title: str = ""


_PROMO_NOISE_RE = re.compile(
    r"(?:×|\bcm\b|\bcommercial\b|\btrailer\b|\banniversary\b|\bpv\b|\brecap\b|\bmusic\s+video\b|●●|\bminami\s+alps\b|\bsuntory\b)",
    re.IGNORECASE,
)


def rank_media_candidate(
    query: str | NormalizedQuery,
    candidate: MediaIdentity,
) -> CandidateRankResult:
    """
    Stage 4 Deterministic, Explainable Ranking Algorithm:
    Evaluates a candidate MediaIdentity against a query using:
      1. Exact canonical title
      2. Exact alternative / Romaji / native title
      3. External metadata identity
      4. Normalized / compact spacing title equality
      5. Release year agreement
      6. Media-type & country agreement
      7. Token similarity & prefix containment
      8. Fuzzy bigram similarity (never used alone for high confidence)
      9. Conflicting sequel / remake / promotional noise qualifiers
    """
    nq = preprocess_search_query(query) if isinstance(query, str) else query
    if not nq.compact_key:
        return CandidateRankResult(score=1.0, confidence_tier="high", reason="Default browse catalog")

    probe_variants: list[tuple[str, str, list[str], bool]] = [
        (nq.normalized_text, nq.compact_key, nq.significant_tokens, True)
    ]
    for alias in nq.expanded_aliases:
        a_norm, a_sig = normalize_title_tokens(alias)
        a_comp = compact_alnum_key(alias)
        if a_comp:
            probe_variants.append((a_norm, a_comp, a_sig, False))

    canonical_titles = [
        t.strip()
        for t in [
            candidate.official_title,
            getattr(candidate, "english_title", ""),
            candidate.romaji_title,
        ]
        if t and t.strip()
    ]
    alt_titles = [
        t.strip()
        for t in [
            candidate.original_title,
            *(candidate.alternate_titles or []),
        ]
        if t and t.strip()
    ]
    for s_meta in candidate.seasons or []:
        for s_alias in s_meta.season_aliases or []:
            if s_alias and s_alias.strip():
                alt_titles.append(s_alias.strip())

    best_base_score = 0.0
    best_reason = "No title match"
    best_matched_title = candidate.official_title
    has_token_or_exact_hit = False

    all_title_entries: list[tuple[str, bool]] = [
        *((t, True) for t in canonical_titles),
        *((t, False) for t in alt_titles),
    ]

    for cand_raw, is_canonical in all_title_entries:
        cand_norm, cand_sig = normalize_title_tokens(cand_raw)
        cand_comp = compact_alnum_key(cand_raw)
        if not cand_comp:
            continue
        cand_all_tokens = cand_norm.split()
        cand_set = set(cand_sig)
        cand_all_set = set(cand_all_tokens)

        for probe_norm, probe_comp, probe_sig, is_primary_probe in probe_variants:
            probe_set = set(probe_sig)
            probe_all_set = set(probe_norm.split())

            # 1 & 2. Exact normalized title match
            if cand_norm == probe_norm:
                has_token_or_exact_hit = True
                sc = 1.00 if (is_canonical and is_primary_probe) else 0.97
                rsn = (
                    "Exact canonical title match"
                    if (is_canonical and is_primary_probe)
                    else "Exact alternative/Romaji title match"
                )
                if sc > best_base_score:
                    best_base_score, best_reason, best_matched_title = sc, rsn, cand_raw
                continue

            # 4. Compact title equality (e.g. "Dandadan" == "DAN DA DAN" == "Dan Da Dan")
            if cand_comp == probe_comp:
                has_token_or_exact_hit = True
                sc = 0.96 if is_primary_probe else 0.94
                rsn = "Normalized compact title equality"
                if sc > best_base_score:
                    best_base_score, best_reason, best_matched_title = sc, rsn, cand_raw
                continue

            # Short-title conservative rule ("IT", "You", "Dark")
            if nq.is_short_ambiguous and is_primary_probe:
                if probe_norm in cand_all_set:
                    has_token_or_exact_hit = True
                    # Extra words present alongside short query -> cap at medium confidence
                    sc = 0.62 if is_canonical else 0.56
                    rsn = "Ambiguous short-title partial token match"
                    if sc > best_base_score:
                        best_base_score, best_reason, best_matched_title = sc, rsn, cand_raw
                continue

            # 7. Token & prefix similarity
            overlap_sig = len(probe_set & cand_set)
            overlap_all = len(probe_all_set & cand_all_set)
            if overlap_sig > 0 or overlap_all > 0:
                has_token_or_exact_hit = True
                extra_in_cand = cand_set - probe_set
                has_sequel_conflict = bool(extra_in_cand & _CONFLICT_SPINOFF_TOKENS)

                if probe_set and probe_set.issubset(cand_set) and cand_norm.startswith(probe_norm):
                    if has_sequel_conflict:
                        # Standalone named sequel/spinoff (e.g. "Naruto: Shippuden" or "Bleach: Thousand-Year Blood War")
                        sc = 0.84 if is_canonical else 0.80
                        rsn = "Distinct sequel/spinoff prefix match"
                    else:
                        # Primary title prefix (e.g. "Frieren" -> "Frieren: Beyond Journey's End")
                        sc = 0.89 if is_canonical else 0.85
                        rsn = "Primary title prefix match"
                elif probe_set and probe_set.issubset(cand_set) and probe_norm in cand_norm:
                    # Query appears mid-title (e.g. "Your Name" inside "Call Me by Your Name")
                    sc = 0.72 if is_canonical else 0.68
                    rsn = "Contained phrase match"
                else:
                    dice = (2.0 * overlap_sig) / max(1, len(probe_set) + len(cand_set))
                    if has_sequel_conflict:
                        dice *= 0.78
                    sc = round(0.76 * dice, 4)
                    rsn = "Token similarity match"

                if sc > best_base_score:
                    best_base_score, best_reason, best_matched_title = sc, rsn, cand_raw
                continue

            # 8. Substring / character bigram similarity (never sufficient alone for high confidence)
            if len(probe_comp) >= 4 and (probe_comp in cand_comp or cand_comp in probe_comp):
                has_token_or_exact_hit = True
                ratio = min(len(probe_comp), len(cand_comp)) / max(len(probe_comp), len(cand_comp))
                sc = min(0.72, 0.55 + 0.20 * ratio)
                rsn = "Substring containment match"
                if sc > best_base_score:
                    best_base_score, best_reason, best_matched_title = sc, rsn, cand_raw

    if not has_token_or_exact_hit or best_base_score <= 0.0:
        return CandidateRankResult(
            score=0.0,
            confidence_tier="rejected",
            reason="Zero title token overlap with query",
            matched_title=candidate.official_title,
        )

    score = best_base_score
    reasons = [best_reason]

    # 9. Penalize promotional/commercial/crossover/mini-special noise unless explicitly queried
    if _PROMO_NOISE_RE.search(candidate.official_title) and not _PROMO_NOISE_RE.search(nq.raw_query):
        score -= 0.38
        reasons.append("promotional/special noise penalty")

    # 5. Release year agreement
    if nq.explicit_year is not None:
        if candidate.release_year is not None:
            diff = abs(candidate.release_year - nq.explicit_year)
            if diff == 0:
                score += 0.12
                reasons.append(f"exact year match ({nq.explicit_year})")
            elif diff == 1:
                score += 0.04
                reasons.append(f"near year match ({candidate.release_year})")
            else:
                score -= 0.30
                reasons.append(f"year mismatch ({candidate.release_year} != {nq.explicit_year})")
        else:
            score -= 0.10

    # 6. Media-type & country agreement
    if nq.media_qualifier:
        if nq.media_qualifier == "live_action":
            if candidate.media_type == "tv":
                score += 0.12
                reasons.append("live-action TV match")
            elif candidate.media_type == "anime":
                score -= 0.30
                reasons.append("anime vs live-action qualifier mismatch")
        elif nq.media_qualifier == "anime":
            if candidate.media_type == "anime":
                score += 0.12
                reasons.append("anime media-type match")
            elif candidate.media_type == "tv":
                score -= 0.30
                reasons.append("TV vs anime qualifier mismatch")
        elif nq.media_qualifier == "movie":
            if candidate.media_type == "movie":
                score += 0.12
                reasons.append("movie media-type match")
            else:
                score -= 0.22

    if nq.country_qualifier:
        cand_country = (getattr(candidate, "country", "") or "").upper()
        if cand_country:
            if cand_country == nq.country_qualifier:
                score += 0.12
                reasons.append(f"country match ({cand_country})")
            else:
                score -= 0.22
                reasons.append(f"country mismatch ({cand_country} != {nq.country_qualifier})")

    # 3. External metadata richness & series completeness tie-breaker
    # Helps major verified works (e.g. "Dark" 2017 3-season TV series, or "Your Name." 2016 movie)
    # rank ahead of obscure dateless stubs or tie-ins with the same short name.
    if candidate.imdb_id:
        score += 0.02
    if candidate.release_year is not None:
        score += 0.03
    else:
        score -= 0.08
    if getattr(candidate, "synopsis", ""):
        score += 0.015
    elif candidate.release_year is None:
        score -= 0.06
    if getattr(candidate, "poster_url", ""):
        score += 0.01
    if len(candidate.seasons) >= 2:
        score += 0.015
    elif candidate.media_type == "movie" and candidate.release_year:
        score += 0.015
    elif candidate.media_type == "tv" and not candidate.seasons and not getattr(candidate, "synopsis", ""):
        # Sparse unenriched TV stubs (e.g. legacy IMDb-only TV entries with zero episode data)
        score -= 0.08

    if best_base_score < 0.95 and nq.explicit_year is None:
        score = min(0.94, score)

    final_score = round(max(0.0, min(1.0, score)), 4)
    if final_score >= 0.78:
        tier = "high"
    elif final_score >= 0.48:
        tier = "medium"
    elif final_score >= 0.25:
        tier = "low"
    else:
        tier = "rejected"

    return CandidateRankResult(
        score=final_score,
        confidence_tier=tier,
        reason=f"{'; '.join(reasons)} [matched '{best_matched_title}']",
        matched_title=best_matched_title,
    )


def rank_and_filter_candidates(
    query: str | NormalizedQuery,
    candidates: list[MediaIdentity],
    min_score: float = 0.28,
) -> list[MediaIdentity]:
    """
    Score, annotate, filter out rejected noise, and deterministically sort candidate MediaIdentity records.
    """
    nq = preprocess_search_query(query) if isinstance(query, str) else query
    if not nq.compact_key:
        return list(candidates)

    scored: list[tuple[float, int, int, int, int, int, MediaIdentity]] = []
    for idx, cand in enumerate(candidates):
        res = rank_media_candidate(nq, cand)
        cand.match_score = res.score
        cand.confidence = res.score
        cand.confidence_tier = res.confidence_tier
        cand.match_reason = res.reason
        if res.confidence_tier == "rejected" or res.score < min_score:
            continue
        has_year_and_poster = 1 if (cand.release_year is not None and bool(cand.poster_url)) else 0
        has_imdb = 1 if cand.imdb_id else 0
        ep_total = getattr(cand, "episode_count", 0) or sum(s.episode_count for s in cand.seasons)
        season_bonus = (
            1
            if (len(cand.seasons) >= 2 or ep_total >= 25 or (cand.media_type == "movie" and cand.release_year))
            else 0
        )
        ep_bonus = min(100, ep_total)
        scored.append((res.score, has_year_and_poster, has_imdb, season_bonus, ep_bonus, -idx, cand))

    scored.sort(key=lambda item: (item[0], item[1], item[2], item[3], item[4], item[5]), reverse=True)
    return [item[6] for item in scored]


class TitleIdentityMatcher:
    """Confidence-based title matcher linking parsed torrent releases to canonical MediaIdentity records."""

    MATCH_THRESHOLD = 0.78
    AMBIGUOUS_THRESHOLD = 0.45

    @classmethod
    def score_release_against_identity(
        cls,
        parsed: ParsedRelease,
        identity: MediaIdentity,
    ) -> tuple[float, int | None, str]:
        """
        Compute a confidence score in [0.0, 1.0] between a ParsedRelease and a MediaIdentity.
        Returns (confidence, matched_season_from_alias, reason).
        """
        rel_norm, rel_tokens = normalize_title_tokens(parsed.show_title)
        qual_norm, qual_tokens = normalize_title_tokens(parsed.season_qualified_title)
        alt_norm, alt_tokens = normalize_title_tokens(parsed.alternative_title)
        if not rel_norm and not alt_norm:
            return (0.0, None, "Empty release title")

        # Build candidate reference titles: (candidate_title, associated_season_or_None)
        candidates: list[tuple[str, int | None]] = []
        for t in [
            identity.official_title,
            getattr(identity, "english_title", ""),
            identity.romaji_title,
            identity.original_title,
            *identity.alternate_titles,
        ]:
            if t and t.strip():
                candidates.append((t.strip(), None))

        for s_meta in identity.seasons:
            for alias in s_meta.season_aliases:
                if alias and alias.strip():
                    candidates.append((alias.strip(), s_meta.season_number))

        best_score = 0.0
        best_season: int | None = None
        best_reason = "No match"

        probes: list[tuple[str, list[str], bool]] = [
            (qual_norm, qual_tokens, True),
            (rel_norm, rel_tokens, False),
        ]
        if alt_norm:
            probes.append((alt_norm, alt_tokens, False))

        for cand_str, cand_season in candidates:
            cand_norm, cand_tokens = normalize_title_tokens(cand_str)
            if not cand_norm:
                continue

            # Compare against base show_title, season_qualified_title, and pipe alternative_title
            for probe_norm, probe_tokens, is_qual in probes:
                if not probe_norm:
                    continue
                score, reason = cls._compare_normalized(
                    probe_norm,
                    probe_tokens,
                    cand_norm,
                    cand_tokens,
                )
                if is_qual and cand_season is not None and score >= 0.90:
                    # Slight preference for exact season alias match
                    score = min(1.0, score + 0.02)
                if score > best_score:
                    best_score = score
                    best_season = cand_season if is_qual else (cand_season if best_season is None else best_season)
                    best_reason = f"{reason} ('{cand_str}')"

        # Remake / reboot year check
        if parsed.release_year is not None and identity.release_year is not None:
            year_diff = abs(parsed.release_year - identity.release_year)
            if year_diff <= 1:
                best_score = min(1.0, best_score + 0.05)
                best_reason += f" [year match {parsed.release_year}]"
            else:
                best_score *= 0.35
                best_reason += f" [year mismatch: release {parsed.release_year} vs show {identity.release_year}]"

        return (round(best_score, 4), best_season, best_reason)

    @staticmethod
    def _compare_normalized(
        probe_norm: str,
        probe_tokens: list[str],
        cand_norm: str,
        cand_tokens: list[str],
    ) -> tuple[float, str]:
        if probe_norm == cand_norm:
            return (1.0, "Exact normalized title match")

        probe_comp = compact_alnum_key(probe_norm)
        cand_comp = compact_alnum_key(cand_norm)
        if probe_comp and probe_comp == cand_comp:
            return (0.98, "Exact compact spacing match")

        probe_set = set(probe_tokens)
        cand_set = set(cand_tokens)
        if not probe_set or not cand_set:
            return (0.0, "Empty token set")

        if probe_set == cand_set:
            return (0.96, "Exact significant token match")

        # Check for conflicting spinoff/reboot/movie tokens present in one but not the other
        extra_in_probe = probe_set - cand_set
        extra_in_cand = cand_set - probe_set
        if (extra_in_probe & _CONFLICT_SPINOFF_TOKENS) or (extra_in_cand & _CONFLICT_SPINOFF_TOKENS):
            overlap = len(probe_set & cand_set)
            if overlap > 0:
                return (0.42, "Conflicting sequel/spinoff/reboot qualifier token")
            return (0.0, "Disjoint tokens")

        overlap = len(probe_set & cand_set)
        if overlap == 0:
            return (0.0, "Zero token overlap")

        dice = (2.0 * overlap) / (len(probe_set) + len(cand_set))

        # Guard against short single-word title matching a much longer unrelated title
        # e.g. "Bleach" vs "Bleach Something Unrelated", or "Psycho" vs "Mob Psycho 100"
        if len(extra_in_probe) > 0 and len(cand_set) <= 2:
            dice *= 0.65

        return (dice, "Token similarity match")


def match_release_to_identity(
    release: TorrentRelease,
    identity: MediaIdentity,
) -> ReleaseMatchResult:
    """Map a normalized TorrentRelease to a canonical MediaIdentity, resolving season, episode, and batch type."""
    parsed = parse_release_title(release.title)
    confidence, alias_season, reason = TitleIdentityMatcher.score_release_against_identity(
        parsed, identity
    )

    if not release.resolution and parsed.resolution:
        release.resolution = parsed.resolution
    if not release.codec and parsed.codec:
        release.codec = parsed.codec
    if not release.container and parsed.container:
        release.container = parsed.container
    if not release.subtitle_lang and parsed.subtitle_lang:
        release.subtitle_lang = parsed.subtitle_lang
    if not release.release_group and parsed.release_group:
        release.release_group = parsed.release_group

    # Conflicting media-type check: e.g. S01E01 episodic live-action release vs 1999 Anime when year conflicts
    if (
        identity.media_type == "movie"
        and parsed.classification in {"episode", "season_pack", "complete_series"}
        and parsed.season_number is not None
    ):
        confidence *= 0.45
        reason += " [episodic SxxExx release vs movie identity]"

    if confidence < TitleIdentityMatcher.AMBIGUOUS_THRESHOLD:
        return ReleaseMatchResult(
            release=release,
            canonical_id=identity.canonical_id,
            classification="unclassified",
            match_bucket="rejected",
            confidence=confidence,
            match_reason=reason,
        )

    if confidence < TitleIdentityMatcher.MATCH_THRESHOLD:
        return ReleaseMatchResult(
            release=release,
            canonical_id=identity.canonical_id,
            classification="ambiguous",
            match_bucket="possible",
            season_number=parsed.season_number or alias_season,
            episode_number=parsed.episode_number,
            first_episode=parsed.first_episode,
            last_episode=parsed.last_episode,
            batch_label=parsed.batch_label,
            confidence=confidence,
            match_reason=reason,
        )

    # Standalone Movie Identity Handling:
    # A movie release (like "[PHTMini] Your Name. (2016) (BD 1080p AV1...)") should be classified as a confirmed
    # feature-film release (Season 1, Episode 1 = Full Movie) rather than buried in unclassified specials!
    if identity.media_type == "movie" and parsed.classification in {"unclassified", "special"}:
        return ReleaseMatchResult(
            release=release,
            canonical_id=identity.canonical_id,
            classification="movie",
            match_bucket="confirmed",
            season_number=1,
            episode_number=1,
            first_episode=1,
            last_episode=1,
            batch_label="Feature Film",
            confidence=confidence,
            match_reason=reason,
        )

    # Resolve season number using explicit season, alias season, or absolute episode mapping
    resolved_season = parsed.season_number
    if (
        alias_season is not None
        and alias_season > 1
        and (resolved_season is None or resolved_season == 1)
    ):
        resolved_season = alias_season

    resolved_ep = parsed.episode_number
    if (
        parsed.classification == "episode"
        and resolved_ep is not None
        and (resolved_season is None or resolved_season == 1)
        and len(identity.seasons) > 1
    ):
        # Check if episode number exceeds Season 1 count and maps to a subsequent season via absolute numbering
        s1_count = identity.seasons[0].episode_count
        if s1_count > 0 and resolved_ep > s1_count:
            rem = resolved_ep
            for s_meta in identity.seasons:
                if s_meta.episode_count > 0 and rem > s_meta.episode_count:
                    rem -= s_meta.episode_count
                else:
                    resolved_season = s_meta.season_number
                    resolved_ep = rem
                    break

    bucket = (
        "unclassified"
        if parsed.classification in {"unclassified", "special"}
        else "confirmed"
    )

    return ReleaseMatchResult(
        release=release,
        canonical_id=identity.canonical_id,
        classification=parsed.classification,
        match_bucket=bucket,
        season_number=resolved_season,
        episode_number=resolved_ep,
        first_episode=parsed.first_episode if parsed.classification != "episode" else resolved_ep,
        last_episode=parsed.last_episode if parsed.classification != "episode" else resolved_ep,
        batch_label=format_batch_label(
            parsed.classification,
            resolved_season,
            parsed.first_episode,
            parsed.last_episode,
        ),
        confidence=confidence,
        match_reason=reason,
    )
