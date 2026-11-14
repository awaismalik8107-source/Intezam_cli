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
    r"\s+\b(?:2160p|1080p|720p|480p|4k|web[- ]?dl|webrip|bdrip|bluray|blu-ray|x26[45]|h\.?26[45]|hevc|avc|av1|10bit|aac|flac|dual[- ]?audio|multi[- ]?subs?)\b.*$",
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
    release_group: str = ""
    release_year: int | None = None
    season_number: int | None = None
    episode_number: int | None = None
    first_episode: int | None = None
    last_episode: int | None = None
    classification: str = "unclassified"  # "episode" | "multi_episode" | "season_pack" | "complete_series" | "special" | "unclassified"
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
    cleaned = _strip_trailing_tech_tags(value)
    cleaned = _YEAR_PARENS_RE.sub("", cleaned)
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

    year_m = _YEAR_PARENS_RE.search(raw)
    release_year: int | None = int(year_m.group(1)) if year_m else None

    cleaned = _FILE_EXT_RE.sub("", raw)
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

    cleaned = _strip_trailing_tech_tags(cleaned)
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
}


def normalize_title_tokens(title: str) -> tuple[str, list[str]]:
    """Normalize a show title into a canonical string and significant token list."""
    text = unicodedata.normalize("NFKC", title or "").lower()
    text = text.replace("’", "'").replace("‘", "'").replace("'", "")
    text = re.sub(r"[^\w\s]", " ", text)
    tokens = [tok for tok in text.split() if tok]
    norm_str = " ".join(tokens)
    sig_tokens = [tok for tok in tokens if tok not in _STOPWORDS] or tokens
    return (norm_str, sig_tokens)


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
        if not rel_norm:
            return (0.0, None, "Empty release title")

        # Build candidate reference titles: (candidate_title, associated_season_or_None)
        candidates: list[tuple[str, int | None]] = []
        for t in [
            identity.official_title,
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

        for cand_str, cand_season in candidates:
            cand_norm, cand_tokens = normalize_title_tokens(cand_str)
            if not cand_norm:
                continue

            # Compare against both base show_title and season_qualified_title
            for probe_norm, probe_tokens, is_qual in [
                (qual_norm, qual_tokens, True),
                (rel_norm, rel_tokens, False),
            ]:
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
                return (0.52, "Conflicting sequel/spinoff/reboot qualifier token")
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

    if confidence < TitleIdentityMatcher.AMBIGUOUS_THRESHOLD:
        return ReleaseMatchResult(
            release=release,
            canonical_id=identity.canonical_id,
            classification="unclassified",
            confidence=confidence,
            match_reason=reason,
        )

    if confidence < TitleIdentityMatcher.MATCH_THRESHOLD:
        return ReleaseMatchResult(
            release=release,
            canonical_id=identity.canonical_id,
            classification="ambiguous",
            season_number=parsed.season_number or alias_season,
            episode_number=parsed.episode_number,
            first_episode=parsed.first_episode,
            last_episode=parsed.last_episode,
            batch_label=parsed.batch_label,
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

    return ReleaseMatchResult(
        release=release,
        canonical_id=identity.canonical_id,
        classification=parsed.classification,
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
