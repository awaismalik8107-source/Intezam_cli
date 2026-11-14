import type { MediaSource, SeasonMetadata } from "./types";

export type ReleaseClassification = "episode" | "batch" | "unclassified";
export type BatchKind = "multi-episode" | "season-pack" | "complete-series";

export interface ReleaseIdentity {
  showTitle: string;
  season: number | null;
  firstEpisode: number | null;
  lastEpisode: number | null;
  classification: ReleaseClassification;
  batchKind?: BatchKind;
  releaseGroup?: string | null;
}

export interface EpisodeEntry {
  season: number;
  number: number;
  title?: string;
  airDate?: string;
  sources: MediaSource[];
}

export interface BatchEntry {
  season: number | null;
  firstEpisode: number | null;
  lastEpisode: number | null;
  batchKind: BatchKind;
  batchLabel: string;
  source: MediaSource;
}

const FILE_EXT_RE = /\.(?:mkv|mp4|avi|webm|m4v|ts|mov|iso|zip|7z|rar|torrent)$/i;
const LEADING_GROUPS_RE = /^(?:\s*\[[^\]]+\]\s*)+/;
const FIRST_GROUP_RE = /^\s*\[([^\]]+)\]/;
const CRC32_TAG_RE = /\s*\[[0-9a-f]{8}\]/gi;

// Technical tags at the end of a title that do not carry season/episode/batch identity
const TRAILING_TECH_TAG_RE =
  /\s*[\[(](?![^\])]*(?:\b(?:batch|complete|season|series|pack|ep(?:isode)?)\b|\bs\d{1,2}\b|\d{1,3}\s*[-~]\s*\d{1,3}))[^\])]*(?:2160p|1080p|720p|480p|4k|x26[45]|hevc|avc|av1|10bit|8bit|web[- ]?dl|webrip|bdrip|bluray|remux|aac|flac|opus|ac3|dual[- ]?audio|multi[- ]?(?:audio|sub(?:title)?s?)|multiple\s+subtitle|eng|jpn|sub|dub|mkv|mp4)[^\])]*[\])]\s*$/i;

const SPECIAL_SUFFIX_RE =
  /\s+[-–—]\s+(?:special(?:\s+pv|\s+ost|\s+original\s+soundtrack)?|ova|oad|ona|movie|pv\d*|ncop\d*|nced\d*|ost|original\s+soundtrack|recap|preview)\b.*$/i;

function stripTrailingTechTags(value: string): string {
  let current = value.trim();
  let previous = "";
  while (current !== previous) {
    previous = current;
    current = current.replace(TRAILING_TECH_TAG_RE, "").trim();
  }
  return current;
}

function tidyTitle(value: string): string {
  let cleaned = value
    .trim()
    .replace(FILE_EXT_RE, "")
    .replace(LEADING_GROUPS_RE, "")
    .replace(CRC32_TAG_RE, "");

  // Convert underscores to spaces; if dot-separated scene release, convert dots to spaces
  cleaned = cleaned.replace(/_/g, " ");
  if (/^[^\s]+\.[^\s]+\.[^\s]+/.test(cleaned)) {
    cleaned = cleaned.replace(/\./g, " ");
  }

  cleaned = stripTrailingTechTags(cleaned)
    .replace(/\s{2,}/g, " ")
    .replace(/[\s\-–—:]+$/, "")
    .trim();

  return cleaned;
}

function cleanShowPrefix(value: string): string {
  return stripTrailingTechTags(value)
    .replace(/\s{2,}/g, " ")
    .replace(/[\s\-–—:]+$/, "")
    .trim();
}

function extractSeasonFromPrefix(rawPrefix: string): {
  showTitle: string;
  season: number | null;
} {
  const cleaned = cleanShowPrefix(rawPrefix);
  const match =
    cleaned.match(/^(.*?)(?:\s+[-–—:]\s*|\s+)(?:Season\s*(\d{1,2})|S(\d{1,2})|(\d{1,2})(?:st|nd|rd|th)\s+Season)$/i);
  if (match) {
    const seasonNum = Number(match[2] ?? match[3] ?? match[4]);
    const baseTitle = cleanShowPrefix(match[1]);
    if (baseTitle.length >= 2 && seasonNum >= 1 && seasonNum <= 30) {
      return { showTitle: baseTitle, season: seasonNum };
    }
  }
  return { showTitle: cleaned, season: null };
}

export function formatBatchLabel(
  batchKind: BatchKind,
  season: number | null,
  firstEpisode: number | null,
  lastEpisode: number | null,
): string {
  if (batchKind === "complete-series") {
    return "Complete-Series Pack";
  }
  if (batchKind === "multi-episode") {
    if (firstEpisode !== null && lastEpisode !== null) {
      const epRange = `Episodes ${String(firstEpisode).padStart(2, "0")}–${String(lastEpisode).padStart(2, "0")}`;
      return season !== null ? `Season ${season} · ${epRange} Range` : `${epRange} Range`;
    }
    return "Multi-Episode Range";
  }
  return season !== null ? `Season ${season} Full Batch` : "Full-Season Batch";
}

/**
 * Parse anime release naming conventions conservatively.
 * Never fabricate episode numbers for a batch/season/complete/special torrent.
 */
export function parseReleaseTitle(raw: string): ReleaseIdentity {
  const groupMatch = raw.match(FIRST_GROUP_RE);
  const releaseGroup = groupMatch ? groupMatch[1].trim() : null;
  const text = tidyTitle(raw);

  // 1. Explicit SxxExx or "Season X Episode Y" (single episode or episode range)
  const epMatch =
    text.match(
      /^(.*?)\s+(?:[-–—:]\s*)?S(\d{1,2})\s*E(\d{1,3})(?:v\d+)?(?:\s*[-~]\s*E?(\d{1,3})(?:v\d+)?)?(?=\b|[\s[(]|$)/i,
    ) ??
    text.match(
      /^(.*?)\s+(?:[-–—:]\s*)?Season\s*(\d{1,2})\s*(?:[-–—:]?\s*)?(?:Episode|Ep\.?|E)\s*(\d{1,3})(?:v\d+)?(?:\s*[-~]\s*(?:Episode|Ep\.?|E)?\s*(\d{1,3})(?:v\d+)?)?(?=\b|[\s[(]|$)/i,
    );
  if (epMatch) {
    const season = Number(epMatch[2]);
    const first = Number(epMatch[3]);
    const last = epMatch[4] ? Number(epMatch[4]) : first;
    if (first >= 1 && last >= first && last - first <= 200) {
      const isRange = last > first;
      return {
        showTitle: cleanShowPrefix(epMatch[1]) || text,
        season,
        firstEpisode: first,
        lastEpisode: last,
        classification: isRange ? "batch" : "episode",
        batchKind: isRange ? "multi-episode" : undefined,
        releaseGroup,
      };
    }
  }

  // 2. Multi-episode range: "Show - 01-12", "Show (Batch 01-24)", "Show [01-12]"
  const rangeMatch = text.match(
    /^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?\s*(?:Batch\s*)?(?:Ep(?:isodes?)?\.?\s*)?(\d{1,3})\s*[-~]\s*(\d{1,3})(?:\s*(?:Batch|Complete|END|Fin))?\s*[\])]?(?:\s*[\[(]?(?:Batch|Complete)[\])]?)?(?=$|\s|[[(])/i,
  );
  if (rangeMatch && !/\b(?:season|part|cour)\s*$/i.test(rangeMatch[1])) {
    const first = Number(rangeMatch[2]);
    const last = Number(rangeMatch[3]);
    if (first >= 1 && last > first && last <= 300 && last - first <= 200) {
      const prefix = extractSeasonFromPrefix(rangeMatch[1]);
      return {
        showTitle: prefix.showTitle || text,
        season: prefix.season,
        firstEpisode: first,
        lastEpisode: last,
        classification: "batch",
        batchKind: "multi-episode",
        releaseGroup,
      };
    }
  }

  // 3. Dashed or explicit single episode: "Show Name - 01", "Show Name Season 2 - 04", "Show Name Episode 05"
  const dashedSingle =
    text.match(
      /^(.*?)\s+[-–—]\s*(?:Ep(?:isode)?\.?\s*)?(\d{1,3})(?:v\d+)?(?=$|\s|[[(])/i,
    ) ??
    text.match(
      /^(.*?)\s+(?:Episode|Ep\.?)\s*(\d{1,3})(?:v\d+)?(?=$|\s|[[(])/i,
    );
  if (dashedSingle) {
    const epNum = Number(dashedSingle[2]);
    if (epNum >= 1 && epNum <= 500) {
      const prefix = extractSeasonFromPrefix(dashedSingle[1]);
      return {
        showTitle: prefix.showTitle || text,
        season: prefix.season ?? 1,
        firstEpisode: epNum,
        lastEpisode: epNum,
        classification: "episode",
        releaseGroup,
      };
    }
  }

  // 4. Complete Series / All-Season Packs
  const completeSeriesMatch = text.match(
    /^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Complete\s+Series(?:\s+Batch|\s+Pack)?|All\s+Seasons?(?:\s+Batch|\s+Pack)?|Full\s+Series(?:\s+Batch|\s+Pack)?)[\])]?(?=$|\s|[[(])/i,
  );
  if (completeSeriesMatch) {
    const prefix = extractSeasonFromPrefix(completeSeriesMatch[1]);
    return {
      showTitle: prefix.showTitle || text,
      season: null,
      firstEpisode: null,
      lastEpisode: null,
      classification: "batch",
      batchKind: "complete-series",
      releaseGroup,
    };
  }

  // 5. Full-Season Batch Packs: "Show Season 1 Batch", "Show S02 Complete", "Show 2nd Season Batch", "Show Season 1"
  const seasonBatchMatch = text.match(
    /^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Season\s*(\d{1,2})|S(\d{1,2})|(\d{1,2})(?:st|nd|rd|th)\s+Season)(?:\s*(?:[-–—]?\s*)?(?:Complete(?:\s+Season|\s+Batch|\s+Pack)?|Batch(?:\s+Pack)?|Season\s+Pack|Pack|BD(?:\s+Batch)?))?[\])]?(?=$|\s|[[(])/i,
  );
  if (seasonBatchMatch) {
    const seasonNum = Number(
      seasonBatchMatch[2] ?? seasonBatchMatch[3] ?? seasonBatchMatch[4],
    );
    return {
      showTitle: cleanShowPrefix(seasonBatchMatch[1]) || text,
      season: seasonNum,
      firstEpisode: null,
      lastEpisode: null,
      classification: "batch",
      batchKind: "season-pack",
      releaseGroup,
    };
  }

  // 6. Generic Batch / Complete Pack without explicit season
  const genericBatchMatch = text.match(
    /^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Complete\s*(?:Season|Batch|Pack)|Season\s*Pack|Batch\s*Pack|Batch)[\])]?(?=$|\s|[[(])/i,
  );
  if (genericBatchMatch) {
    const prefix = extractSeasonFromPrefix(genericBatchMatch[1]);
    return {
      showTitle: prefix.showTitle || text,
      season: prefix.season,
      firstEpisode: null,
      lastEpisode: null,
      classification: "batch",
      batchKind: prefix.season !== null ? "season-pack" : "complete-series",
      releaseGroup,
    };
  }

  // 7. Unclassified / Specials / OVAs / Movies / OSTs (never fabricate episode numbers)
  const strippedSpecial = text.replace(SPECIAL_SUFFIX_RE, "").trim();
  const prefix = extractSeasonFromPrefix(strippedSpecial);
  return {
    showTitle: prefix.showTitle || cleanShowPrefix(text) || raw.trim(),
    season: prefix.season,
    firstEpisode: null,
    lastEpisode: null,
    classification: "unclassified",
    releaseGroup,
  };
}

export function showKey(title: string): string {
  return title
    .toLowerCase()
    .normalize("NFKC")
    .replace(/[^\p{L}\p{N}]+/gu, "-")
    .replace(/^-|-$/g, "");
}

function sourceDedupKey(source: MediaSource): string {
  if (source.infoHash) {
    return `btih:${source.infoHash.toLowerCase()}`;
  }
  const magnet = (source.item.magnet_link || source.item.resolved_link || "").trim();
  if (magnet.toLowerCase().startsWith("magnet:?")) {
    const btihMatch = magnet.match(/xt=urn:btih:([A-Za-z0-9]{32,40})/i);
    if (btihMatch) return `btih:${btihMatch[1].toLowerCase()}`;
    return `magnet:${magnet.toLowerCase()}`;
  }
  return `src:${(source.item.torrent_link || source.item.view_url || source.sourceTitle).trim().toLowerCase()}`;
}

function resolveOfficialSeasonAndEpisode(
  parsed: ReleaseIdentity,
  officialSeasons?: SeasonMetadata[],
): { season: number; episode: number } | null {
  if (parsed.firstEpisode === null) return null;
  let season = parsed.season;
  let episode = parsed.firstEpisode;

  if (officialSeasons && officialSeasons.length > 0) {
    if (season === null) {
      const normalizedPrefix = showKey(parsed.showTitle);
      for (const sm of officialSeasons) {
        if (sm.season_number < 2) continue;
        const candidates = [sm.title, ...(sm.season_aliases || [])].filter(Boolean);
        if (
          candidates.some((alias) => {
            const aliasKey = showKey(alias);
            return aliasKey.length > 3 && normalizedPrefix.includes(aliasKey);
          })
        ) {
          season = sm.season_number;
          break;
        }
      }
    }

    if (season === null || season === 1) {
      const season1 = officialSeasons.find((s) => s.season_number === 1);
      if (season1 && season1.episode_count > 0 && episode > season1.episode_count) {
        let running = 0;
        const sortedSeasons = [...officialSeasons]
          .filter((s) => s.season_number >= 1)
          .sort((a, b) => a.season_number - b.season_number);
        for (const sm of sortedSeasons) {
          if (sm.episode_count <= 0) break;
          if (episode <= running + sm.episode_count) {
            season = sm.season_number;
            episode = episode - running;
            break;
          }
          running += sm.episode_count;
        }
      }
    }
  }

  return { season: season ?? 1, episode };
}

export function buildEpisodeCatalog(
  sources: MediaSource[],
  officialSeasons?: SeasonMetadata[],
): {
  episodes: EpisodeEntry[];
  batches: BatchEntry[];
  other: MediaSource[];
  seasons: number[];
} {
  const episodeMap = new Map<string, { entry: EpisodeEntry; seen: Set<string> }>();
  const batches: BatchEntry[] = [];
  const batchSeen = new Set<string>();
  const other: MediaSource[] = [];
  const otherSeen = new Set<string>();

  const officialEpLookup = new Map<string, { title?: string; airDate?: string }>();
  if (officialSeasons) {
    for (const sm of officialSeasons) {
      for (const ep of sm.episodes || []) {
        officialEpLookup.set(`${sm.season_number}:${ep.episode_number}`, {
          title: ep.title || undefined,
          airDate: ep.air_date || undefined,
        });
      }
    }
  }

  for (const source of sources) {
    const parsed = parseReleaseTitle(source.sourceTitle);
    const dedupKey = sourceDedupKey(source);

    if (parsed.classification === "episode" && parsed.firstEpisode !== null) {
      const resolved = resolveOfficialSeasonAndEpisode(parsed, officialSeasons) ?? {
        season: parsed.season ?? 1,
        episode: parsed.firstEpisode,
      };
      const season = resolved.season;
      const epNum = resolved.episode;
      const key = `${season}:${epNum}`;
      const officialMeta = officialEpLookup.get(key);
      const record = episodeMap.get(key) ?? {
        entry: {
          season,
          number: epNum,
          title: officialMeta?.title,
          airDate: officialMeta?.airDate,
          sources: [],
        },
        seen: new Set<string>(),
      };
      if (!record.seen.has(dedupKey)) {
        record.entry.sources.push(source);
        record.seen.add(dedupKey);
      }
      episodeMap.set(key, record);
    } else if (parsed.classification === "batch") {
      if (!batchSeen.has(dedupKey)) {
        batchSeen.add(dedupKey);
        const batchKind: BatchKind = parsed.batchKind ?? (parsed.season !== null ? "season-pack" : "complete-series");
        batches.push({
          season: parsed.season,
          firstEpisode: parsed.firstEpisode,
          lastEpisode: parsed.lastEpisode,
          batchKind,
          batchLabel: formatBatchLabel(batchKind, parsed.season, parsed.firstEpisode, parsed.lastEpisode),
          source,
        });
      }
    } else {
      if (!otherSeen.has(dedupKey)) {
        otherSeen.add(dedupKey);
        other.push(source);
      }
    }
  }

  const episodes = [...episodeMap.values()]
    .map((r) => r.entry)
    .sort((a, b) => a.season - b.season || a.number - b.number);

  const seasons = [
    ...new Set([
      ...(officialSeasons || [])
        .map((s) => s.season_number)
        .filter((s) => s >= 1 && episodes.some((ep) => ep.season === s)),
      ...episodes.map((entry) => entry.season),
      ...batches
        .map((batch) => batch.season)
        .filter((season): season is number => season !== null),
    ]),
  ].sort((a, b) => a - b);

  return { episodes, batches, other, seasons };
}
