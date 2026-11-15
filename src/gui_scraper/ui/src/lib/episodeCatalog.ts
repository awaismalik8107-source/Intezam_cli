import type {
  ManifestFileType,
  MediaSource,
  SeasonMetadata,
  TorrentManifestFile,
} from "./types";

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
    /^(.*?)\s*(?:[-–—:]\s*|\s+)[\[(]?(?:Complete\s+(?:Series|Collection)(?:\s+Batch|\s+Pack)?|All\s+Seasons?(?:\s+Batch|\s+Pack)?|Full\s+Series(?:\s+Batch|\s+Pack)?)[\])]?(?=$|\s|[[(])/i,
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
  mediaKind?: string,
): {
  episodes: EpisodeEntry[];
  batches: BatchEntry[];
  other: MediaSource[];
  possibleMatches: MediaSource[];
  seasons: number[];
} {
  const episodeMap = new Map<string, { entry: EpisodeEntry; seen: Set<string> }>();
  const batches: BatchEntry[] = [];
  const batchSeen = new Set<string>();
  const other: MediaSource[] = [];
  const otherSeen = new Set<string>();
  const possibleMatches: MediaSource[] = [];
  const possibleSeen = new Set<string>();
  const isMovieShow = mediaKind === "movie";

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
    const dedupKey = sourceDedupKey(source);
    const bucket = source.matchBucket || source.item.match_bucket;
    if (bucket === "possible") {
      if (!possibleSeen.has(dedupKey)) {
        possibleSeen.add(dedupKey);
        possibleMatches.push(source);
      }
      continue;
    }

    const backendClass = source.item.classification;
    if (isMovieShow || backendClass === "movie") {
      const key = "1:1";
      const record = episodeMap.get(key) ?? {
        entry: {
          season: 1,
          number: 1,
          title: "Feature Film Releases",
          sources: [],
        },
        seen: new Set<string>(),
      };
      if (!record.seen.has(dedupKey)) {
        record.entry.sources.push(source);
        record.seen.add(dedupKey);
      }
      episodeMap.set(key, record);
      continue;
    }

    const parsed = parseReleaseTitle(source.sourceTitle);
    const effectiveIsEpisode =
      backendClass === "episode" || parsed.classification === "episode";
    const backendSeason =
      typeof source.item.season_number === "number" ? source.item.season_number : null;
    const backendEpisode =
      typeof source.item.episode_number === "number" ? source.item.episode_number : null;

    if (effectiveIsEpisode && (backendEpisode !== null || parsed.firstEpisode !== null)) {
      const resolved =
        backendSeason !== null && backendEpisode !== null
          ? { season: backendSeason, episode: backendEpisode }
          : (resolveOfficialSeasonAndEpisode(parsed, officialSeasons) ?? {
              season: parsed.season ?? 1,
              episode: parsed.firstEpisode ?? 1,
            });
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
    } else if (
      backendClass === "season_pack" ||
      backendClass === "multi_episode" ||
      backendClass === "complete_series" ||
      parsed.classification === "batch"
    ) {
      if (!batchSeen.has(dedupKey)) {
        batchSeen.add(dedupKey);
        const effSeason = backendSeason ?? parsed.season;
        const effFirst =
          typeof source.item.first_episode === "number"
            ? source.item.first_episode
            : parsed.firstEpisode;
        const effLast =
          typeof source.item.last_episode === "number"
            ? source.item.last_episode
            : parsed.lastEpisode;
        const batchKind: BatchKind =
          backendClass === "complete_series"
            ? "complete-series"
            : backendClass === "multi_episode"
              ? "multi-episode"
              : backendClass === "season_pack"
                ? "season-pack"
                : (parsed.batchKind ?? (effSeason !== null ? "season-pack" : "complete-series"));
        batches.push({
          season: effSeason,
          firstEpisode: effFirst,
          lastEpisode: effLast,
          batchKind,
          batchLabel:
            source.item.batch_label ||
            formatBatchLabel(batchKind, effSeason, effFirst, effLast),
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

  return { episodes, batches, other, possibleMatches, seasons };
}

export interface OfficialEpisodeItem {
  seasonNumber: number;
  episodeNumber: number;
  title: string;
  titleAvailable: boolean;
  airDate: string;
  overview: string;
  stillUrl: string;
}

export interface OfficialSeasonGroup {
  seasonNumber: number;
  label: string;
  providerTitle: string;
  episodeCount: number;
  airYear: number | null;
  episodes: OfficialEpisodeItem[];
}

export interface BatchOfficialCatalog {
  seasons: OfficialSeasonGroup[];
  seasonNumbers: number[];
  defaultSeason: number | null;
  coverageMode: "complete-series" | "season-pack" | "inferred-range" | "unverified";
  coverageUnverified: boolean;
  coverageNotice: string;
  inferredRange: {
    season: number | null;
    firstEpisode: number;
    lastEpisode: number;
  } | null;
  metadataProvider: string;
}

export type FileEpisodeMatchConfidence = "confirmed" | "possible" | "unmatched";

export interface MatchedManifestFile {
  file: TorrentManifestFile;
  fileType: ManifestFileType;
  confidence: FileEpisodeMatchConfidence;
  matchedSeason: number | null;
  matchedEpisode: number | null;
  matchedEpisodeEnd: number | null;
  officialEpisodeTitle: string | null;
  reason: string;
}

export function formatSeasonLabel(seasonNumber: number): string {
  return seasonNumber === 0 ? "Specials" : `Season ${seasonNumber}`;
}

export function buildEpisodeFirstSeasons(
  officialSeasons: SeasonMetadata[] | undefined,
  catalog: { episodes: EpisodeEntry[]; seasons: number[] },
  isMovie = false,
): {
  seasons: number[];
  episodesBySeason: Map<number, EpisodeEntry[]>;
  totalEpisodeCount: number;
} {
  const seasonSet = new Set<number>(catalog.seasons);
  const bySeason = new Map<number, Map<number, EpisodeEntry>>();

  const ensureSeason = (sNum: number): Map<number, EpisodeEntry> => {
    seasonSet.add(sNum);
    let inner = bySeason.get(sNum);
    if (!inner) {
      inner = new Map<number, EpisodeEntry>();
      bySeason.set(sNum, inner);
    }
    return inner;
  };

  if (!isMovie && officialSeasons && officialSeasons.length > 0) {
    for (const sm of officialSeasons) {
      if (!Number.isFinite(sm.season_number) || sm.season_number < 0) continue;
      const inner = ensureSeason(sm.season_number);
      for (const ep of sm.episodes || []) {
        if (!ep || ep.episode_number <= 0) continue;
        inner.set(ep.episode_number, {
          season: sm.season_number,
          number: ep.episode_number,
          title: (ep.title || "").trim() || undefined,
          airDate: (ep.air_date || "").trim() || undefined,
          sources: [],
        });
      }
      const targetCount = Math.max(sm.episode_count || 0, inner.size);
      for (let epNum = 1; epNum <= targetCount; epNum += 1) {
        if (!inner.has(epNum)) {
          inner.set(epNum, {
            season: sm.season_number,
            number: epNum,
            sources: [],
          });
        }
      }
    }
  }

  for (const epEntry of catalog.episodes) {
    const inner = ensureSeason(epEntry.season);
    const existing = inner.get(epEntry.number);
    if (existing) {
      existing.sources = [...epEntry.sources];
      if (!existing.title && epEntry.title) existing.title = epEntry.title;
      if (!existing.airDate && epEntry.airDate) existing.airDate = epEntry.airDate;
    } else {
      inner.set(epEntry.number, { ...epEntry, sources: [...epEntry.sources] });
    }
  }

  const orderedSeasons = [...seasonSet].sort((a, b) => {
    if (a === 0 && b !== 0) return 1;
    if (b === 0 && a !== 0) return -1;
    return a - b;
  });

  const episodesBySeason = new Map<number, EpisodeEntry[]>();
  let totalEpisodeCount = 0;
  for (const sNum of orderedSeasons) {
    const eps = [...(bySeason.get(sNum)?.values() ?? [])].sort((a, b) => a.number - b.number);
    episodesBySeason.set(sNum, eps);
    totalEpisodeCount += eps.length;
  }

  return { seasons: orderedSeasons, episodesBySeason, totalEpisodeCount };
}

export function selectDefaultBatchSeason(
  batch: Pick<BatchEntry, "batchKind" | "season">,
  seasonNumbers: number[],
): number | null {
  if (seasonNumbers.length === 0) return null;
  if (
    (batch.batchKind === "season-pack" || batch.batchKind === "multi-episode") &&
    batch.season !== null &&
    seasonNumbers.includes(batch.season)
  ) {
    return batch.season;
  }
  if (seasonNumbers.includes(1)) {
    return 1;
  }
  return seasonNumbers[0];
}

function sortSeasonNumbers(nums: number[]): number[] {
  return [...nums].sort((a, b) => {
    if (a === 0 && b !== 0) return 1;
    if (b === 0 && a !== 0) return -1;
    return a - b;
  });
}

function normalizeOfficialSeason(sm: SeasonMetadata): OfficialSeasonGroup {
  const byEp = new Map<number, OfficialEpisodeItem>();
  for (const ep of sm.episodes || []) {
    if (!ep || ep.episode_number <= 0) continue;
    const rawTitle = (ep.title || "").trim();
    const hasTitle = rawTitle.length > 0;
    byEp.set(ep.episode_number, {
      seasonNumber: sm.season_number,
      episodeNumber: ep.episode_number,
      title: hasTitle ? rawTitle : "Title unavailable",
      titleAvailable: hasTitle,
      airDate: (ep.air_date || "").trim(),
      overview: (ep.overview || "").trim(),
      stillUrl: (ep.still_url || "").trim(),
    });
  }

  const targetCount = Math.max(sm.episode_count || 0, byEp.size);
  for (let epNum = 1; epNum <= targetCount; epNum += 1) {
    if (!byEp.has(epNum)) {
      byEp.set(epNum, {
        seasonNumber: sm.season_number,
        episodeNumber: epNum,
        title: "Title unavailable",
        titleAvailable: false,
        airDate: "",
        overview: "",
        stillUrl: "",
      });
    }
  }

  const episodes = [...byEp.values()].sort((a, b) => a.episodeNumber - b.episodeNumber);
  return {
    seasonNumber: sm.season_number,
    label: formatSeasonLabel(sm.season_number),
    providerTitle: (sm.title || "").trim(),
    episodeCount: episodes.length,
    airYear: sm.air_year ?? null,
    episodes,
  };
}

/**
 * Build an independent official season & episode catalog from `media.officialSeasons`,
 * working even when zero individual episode torrent sources exist.
 */
export function buildBatchOfficialCatalog(
  batch: BatchEntry,
  officialSeasons?: SeasonMetadata[],
  metadataProvider = "",
): BatchOfficialCatalog {
  const providerLabel = metadataProvider.trim() || "Unavailable";
  const rawSeasons = (officialSeasons || []).filter(
    (s) => Number.isFinite(s.season_number) && s.season_number >= 0,
  );
  if (rawSeasons.length === 0) {
    return {
      seasons: [],
      seasonNumbers: [],
      defaultSeason: null,
      coverageMode: "unverified",
      coverageUnverified: true,
      coverageNotice:
        "No official season/episode catalog was returned by metadata providers. Release coverage is unverified.",
      inferredRange: null,
      metadataProvider: providerLabel,
    };
  }

  const normalizedMap = new Map<number, OfficialSeasonGroup>();
  for (const sm of rawSeasons) {
    const group = normalizeOfficialSeason(sm);
    const existing = normalizedMap.get(group.seasonNumber);
    if (!existing || group.episodes.length > existing.episodes.length) {
      normalizedMap.set(group.seasonNumber, group);
    }
  }

  const seasonNumbers = sortSeasonNumbers([...normalizedMap.keys()]);
  const seasons = seasonNumbers
    .map((num) => normalizedMap.get(num))
    .filter((s): s is OfficialSeasonGroup => Boolean(s));
  const regularSeasons = seasons.filter((s) => s.seasonNumber >= 1);
  const defaultSeason = selectDefaultBatchSeason(batch, seasonNumbers);

  if (batch.batchKind === "complete-series") {
    return {
      seasons,
      seasonNumbers,
      defaultSeason,
      coverageMode: "complete-series",
      coverageUnverified: false,
      coverageNotice:
        "Displaying all available official seasons for reference. Inspect the .torrent manifest under Verified Files to confirm which episodes this release actually contains.",
      inferredRange: null,
      metadataProvider: providerLabel,
    };
  }

  if (batch.batchKind === "season-pack") {
    const hasMatchedSeason =
      batch.season !== null && seasonNumbers.includes(batch.season);
    return {
      seasons,
      seasonNumbers,
      defaultSeason,
      coverageMode: hasMatchedSeason ? "season-pack" : "unverified",
      coverageUnverified: !hasMatchedSeason,
      coverageNotice: hasMatchedSeason
        ? `Defaulting to ${formatSeasonLabel(batch.season as number)} based on the release title. Inspect Verified Files to verify the actual manifest contents.`
        : "Target season for this pack could not be matched to the official catalog; showing official seasons (coverage unverified).",
      inferredRange: null,
      metadataProvider: providerLabel,
    };
  }

  // multi-episode batch
  if (batch.firstEpisode !== null && batch.lastEpisode !== null) {
    const firstEp = batch.firstEpisode;
    const lastEp = batch.lastEpisode;

    if (batch.season !== null) {
      const targetSeason = normalizedMap.get(batch.season);
      const maxEp = targetSeason ? targetSeason.episodeCount : 0;
      if (targetSeason && (maxEp === 0 || lastEp <= maxEp)) {
        const filteredSeasons = seasons.map((s) =>
          s.seasonNumber === batch.season
            ? {
                ...s,
                episodes: s.episodes.filter(
                  (ep) => ep.episodeNumber >= firstEp && ep.episodeNumber <= lastEp,
                ),
              }
            : s,
        );
        return {
          seasons: filteredSeasons,
          seasonNumbers,
          defaultSeason: batch.season,
          coverageMode: "inferred-range",
          coverageUnverified: false,
          coverageNotice: `Showing inferred range ${formatSeasonLabel(batch.season)} · E${String(firstEp).padStart(2, "0")}–E${String(lastEp).padStart(2, "0")} (inferred from release title; actual files require manifest inspection).`,
          inferredRange: { season: batch.season, firstEpisode: firstEp, lastEpisode: lastEp },
          metadataProvider: providerLabel,
        };
      }
    } else if (regularSeasons.length === 1) {
      const onlySeason = regularSeasons[0];
      if (onlySeason.episodeCount === 0 || lastEp <= onlySeason.episodeCount) {
        const filteredSeasons = seasons.map((s) =>
          s.seasonNumber === onlySeason.seasonNumber
            ? {
                ...s,
                episodes: s.episodes.filter(
                  (ep) => ep.episodeNumber >= firstEp && ep.episodeNumber <= lastEp,
                ),
              }
            : s,
        );
        return {
          seasons: filteredSeasons,
          seasonNumbers,
          defaultSeason: onlySeason.seasonNumber,
          coverageMode: "inferred-range",
          coverageUnverified: false,
          coverageNotice: `Showing inferred range ${onlySeason.label} · E${String(firstEp).padStart(2, "0")}–E${String(lastEp).padStart(2, "0")} (inferred from release title; actual files require manifest inspection).`,
          inferredRange: {
            season: onlySeason.seasonNumber,
            firstEpisode: firstEp,
            lastEpisode: lastEp,
          },
          metadataProvider: providerLabel,
        };
      }
    }
  }

  return {
    seasons,
    seasonNumbers,
    defaultSeason,
    coverageMode: "unverified",
    coverageUnverified: true,
    coverageNotice:
      "The selected torrent's exact season/episode coverage cannot be verified reliably from its release name. Showing the official catalog for reference.",
    inferredRange: null,
    metadataProvider: providerLabel,
  };
}

const NON_EPISODE_FILE_RE =
  /(?:^|[\s._\-\[\(])(?:ncop\d*|nced\d*|creditless|opening|ending|menu\s*\d*|pv\s*\d*|preview|trailer|teaser|cm\s*\d*|making[\s._-]*of|interview|soundtrack|ost|scans?|booklet|sample|font(?:s)?)(?=$|[\s._\-\]\)])/i;
const OP_ED_SHORT_RE =
  /(?:^|[\s._\-\[\(])(?:op|ed)\s*\d+(?:v\d+)?(?=$|[\s._\-\]\)])/i;
const AMBIGUOUS_LABEL_RE =
  /(?:^|[\s._\-\[\(])(?:bonus|extra|extras|digest|recap|disc|disk|part|vol(?:ume)?)(?:\s*\d+)?(?=$|[\s._\-\]\)])/i;
const TECH_STRIP_FOR_FILE_RE =
  /[\[\(][^\]\)]*(?:2160p|1080p|720p|480p|4k|x26[45]|h\.?26[45]|hevc|avc|av1|10bit|8bit|web[- ]?dl|webrip|bdrip|bluray|aac|flac|opus|ac3|eac3|ddp\d*|dual[- ]?audio|multi|eng|jpn|[0-9a-f]{8})[^\]\)]*[\]\)]/gi;

function extractFolderSeason(relativePath: string): number | null {
  const idx = relativePath.lastIndexOf("/");
  if (idx <= 0) return null;
  const dirPart = relativePath.slice(0, idx);
  if (/(?:^|\/)\s*(?:specials?|ova|oad|season\s*0|s00)\s*(?:$|\/)/i.test(dirPart)) {
    return 0;
  }
  const match = dirPart.match(
    /(?:^|\/)[^/]*(?:\bseason\s*(\d{1,2})\b|\bs(\d{1,2})\b|\b(\d{1,2})(?:st|nd|rd|th)\s+season\b)/i,
  );
  if (match) {
    const val = Number(match[1] ?? match[2] ?? match[3]);
    if (Number.isFinite(val) && val >= 0 && val <= 40) return val;
  }
  return null;
}

function normalizeFileType(raw: string): ManifestFileType {
  if (raw === "video" || raw === "subtitle") return raw;
  return "auxiliary";
}

function lookupOfficialTitle(
  officialSeasons: SeasonMetadata[] | undefined,
  season: number | null,
  episode: number | null,
): string | null {
  if (!officialSeasons || season === null || episode === null) return null;
  const sm = officialSeasons.find((s) => s.season_number === season);
  if (!sm) return null;
  const ep = (sm.episodes || []).find((e) => e.episode_number === episode);
  if (!ep) return null;
  const t = (ep.title || "").trim();
  return t || "Title unavailable";
}

function mapAbsoluteToOfficialSeason(
  epNum: number,
  regularSeasons: SeasonMetadata[],
): { season: number; episode: number } | null {
  if (regularSeasons.length <= 1 || epNum <= 0) return null;
  const sorted = [...regularSeasons].sort((a, b) => a.season_number - b.season_number);
  let running = 0;
  for (const sm of sorted) {
    const count = Math.max(sm.episode_count || 0, (sm.episodes || []).length);
    if (count <= 0) return null;
    if (epNum <= running + count) {
      return { season: sm.season_number, episode: epNum - running };
    }
    running += count;
  }
  return null;
}

/**
 * Conservatively match torrent manifest filenames to official episodes.
 * Ambiguous filenames are downgraded to "possible" or "unmatched" rather than falsely confirmed.
 */
export function matchManifestFilesToEpisodes(
  files: TorrentManifestFile[],
  batch: Pick<BatchEntry, "batchKind" | "season">,
  officialSeasons?: SeasonMetadata[],
): MatchedManifestFile[] {
  const regularSeasons = (officialSeasons || []).filter((s) => s.season_number >= 1);
  const seasonByNum = new Map<number, SeasonMetadata>();
  for (const s of officialSeasons || []) {
    seasonByNum.set(s.season_number, s);
  }
  const hasSpecialsSeason = seasonByNum.has(0);

  return files.map((file) => {
    const fileType = normalizeFileType(file.file_type);
    if (fileType === "auxiliary") {
      return {
        file,
        fileType,
        confidence: "unmatched",
        matchedSeason: null,
        matchedEpisode: null,
        matchedEpisodeEnd: null,
        officialEpisodeTitle: null,
        reason: "Auxiliary / non-episode file",
      };
    }

    const baseName = file.filename.replace(FILE_EXT_RE, "").trim();
    const cleanedName = baseName
      .replace(TECH_STRIP_FOR_FILE_RE, " ")
      .replace(/[\[\(]\s*[\]\)]/g, " ")
      .replace(/\s{2,}/g, " ")
      .trim();

    if (NON_EPISODE_FILE_RE.test(baseName) || OP_ED_SHORT_RE.test(baseName)) {
      return {
        file,
        fileType,
        confidence: "unmatched",
        matchedSeason: null,
        matchedEpisode: null,
        matchedEpisodeEnd: null,
        officialEpisodeTitle: null,
        reason: "Creditless OP/ED, preview, or non-episode extra",
      };
    }

    const folderSeason = extractFolderSeason(file.relative_path);

    // 1. Explicit SxxExx (including S00Exx specials and multi-episode S01E01-E02)
    const sxxexx = cleanedName.match(
      /\bS(\d{1,2})\s*E(\d{1,3})(?:v\d+)?(?:\s*[-~]\s*E?(\d{1,3})(?:v\d+)?)?(?=\b|[\s._\-\[\(]|$)/i,
    ) ??
      cleanedName.match(
        /\bSeason\s*(\d{1,2})\s*[-–—:]?\s*(?:Episode|Ep\.?|E)\s*(\d{1,3})(?:v\d+)?(?:\s*[-~]\s*(?:Episode|Ep\.?|E)?\s*(\d{1,3}))?/i,
      );

    if (sxxexx) {
      const fSeason = Number(sxxexx[1]);
      const epStart = Number(sxxexx[2]);
      const epEnd = sxxexx[3] ? Number(sxxexx[3]) : null;
      const seasonMeta = seasonByNum.get(fSeason);
      const seasonCap = seasonMeta
        ? Math.max(seasonMeta.episode_count || 0, (seasonMeta.episodes || []).length)
        : 0;

      if (epEnd !== null && epEnd > epStart) {
        return {
          file,
          fileType,
          confidence: "possible",
          matchedSeason: fSeason,
          matchedEpisode: epStart,
          matchedEpisodeEnd: epEnd,
          officialEpisodeTitle: lookupOfficialTitle(officialSeasons, fSeason, epStart),
          reason: `Multi-episode range inside single file (E${String(epStart).padStart(2, "0")}–E${String(epEnd).padStart(2, "0")})`,
        };
      }
      if (folderSeason !== null && folderSeason !== fSeason) {
        return {
          file,
          fileType,
          confidence: "possible",
          matchedSeason: fSeason,
          matchedEpisode: epStart,
          matchedEpisodeEnd: null,
          officialEpisodeTitle: lookupOfficialTitle(officialSeasons, fSeason, epStart),
          reason: `Filename indicates ${formatSeasonLabel(fSeason)} but folder indicates ${formatSeasonLabel(folderSeason)}`,
        };
      }
      if (AMBIGUOUS_LABEL_RE.test(cleanedName)) {
        return {
          file,
          fileType,
          confidence: "possible",
          matchedSeason: fSeason,
          matchedEpisode: epStart,
          matchedEpisodeEnd: null,
          officialEpisodeTitle: lookupOfficialTitle(officialSeasons, fSeason, epStart),
          reason: "Filename contains extra/bonus/part marker alongside episode tag",
        };
      }
      if (seasonCap > 0 && epStart > seasonCap) {
        return {
          file,
          fileType,
          confidence: "possible",
          matchedSeason: fSeason,
          matchedEpisode: epStart,
          matchedEpisodeEnd: null,
          officialEpisodeTitle: null,
          reason: `Episode ${epStart} exceeds official count (${seasonCap}) for ${formatSeasonLabel(fSeason)}`,
        };
      }

      return {
        file,
        fileType,
        confidence: "confirmed",
        matchedSeason: fSeason,
        matchedEpisode: epStart,
        matchedEpisodeEnd: null,
        officialEpisodeTitle: lookupOfficialTitle(officialSeasons, fSeason, epStart),
        reason: `Explicit S${String(fSeason).padStart(2, "0")}E${String(epStart).padStart(2, "0")} identifier`,
      };
    }

    // 2. Reject ambiguous Bonus / Part / Disc / Digest / OVA markers from confirming a regular episode
    if (AMBIGUOUS_LABEL_RE.test(cleanedName)) {
      const ovaNum = cleanedName.match(/\b(?:ova|oad|sp|special)\s*(\d{1,2})\b/i);
      if (ovaNum && hasSpecialsSeason) {
        const spNum = Number(ovaNum[1]);
        return {
          file,
          fileType,
          confidence: "possible",
          matchedSeason: 0,
          matchedEpisode: spNum,
          matchedEpisodeEnd: null,
          officialEpisodeTitle: lookupOfficialTitle(officialSeasons, 0, spNum),
          reason: "Special/OVA release candidate; verify against Specials list",
        };
      }
      return {
        file,
        fileType,
        confidence: "unmatched",
        matchedSeason: null,
        matchedEpisode: null,
        matchedEpisodeEnd: null,
        officialEpisodeTitle: null,
        reason: "Ambiguous bonus, part, disc, or digest file",
      };
    }

    const spMatch = cleanedName.match(/\b(?:sp|ova|oad|special)\s*(\d{1,2})\b/i);
    if (spMatch) {
      const spNum = Number(spMatch[1]);
      if (hasSpecialsSeason) {
        return {
          file,
          fileType,
          confidence: folderSeason === 0 ? "confirmed" : "possible",
          matchedSeason: 0,
          matchedEpisode: spNum,
          matchedEpisodeEnd: null,
          officialEpisodeTitle: lookupOfficialTitle(officialSeasons, 0, spNum),
          reason:
            folderSeason === 0
              ? `Specials folder SP${String(spNum).padStart(2, "0")}`
              : `Possible match to Specials (SP${String(spNum).padStart(2, "0")})`,
        };
      }
      return {
        file,
        fileType,
        confidence: "unmatched",
        matchedSeason: null,
        matchedEpisode: null,
        matchedEpisodeEnd: null,
        officialEpisodeTitle: null,
        reason: "Special/OVA file; no Specials season in official catalog",
      };
    }

    // 3. Single dashed/explicit episode number: "Show - 01", "Show Ep 05", or "Season 1/01 - Title"
    const cleanNoGroup = cleanedName
      .replace(LEADING_GROUPS_RE, "")
      .replace(CRC32_TAG_RE, "")
      .replace(/\b(?:19\d{2}|20\d{2})\b/g, "")
      .replace(/\b(?:2160p|1080p|720p|480p|10bit|8bit|x264|x265|hevc|av1|avc|opus|aac|flac|h\.?26[45]|ddp?\s*5\.1|dd\s*2\.0)\b/gi, "")
      .trim();

    const dashedEp =
      cleanNoGroup.match(/\s+[-–—]\s*(?:Ep(?:isode)?\.?\s*)?(\d{1,3})(?:v\d+)?(?=$|\s|[._\[(])/i) ??
      cleanNoGroup.match(/\b(?:Episode|Ep\.?)\s*(\d{1,3})(?:v\d+)?(?=$|\s|[._\[(])/i) ??
      (folderSeason !== null
        ? cleanNoGroup.match(/^(\d{1,3})(?:v\d+)?(?=\s+[-–—.]|\s*$)/)
        : null);

    if (dashedEp) {
      const epNum = Number(dashedEp[1]);
      if (epNum >= 1 && epNum <= 500) {
        // Explicit inline season prefix in filename (e.g. "Show Season 2 - 04")
        const inlinePrefixSeason = extractSeasonFromPrefix(
          cleanNoGroup.slice(0, dashedEp.index ?? cleanNoGroup.length),
        ).season;

        if (inlinePrefixSeason !== null && folderSeason !== null && inlinePrefixSeason !== folderSeason) {
          return {
            file,
            fileType,
            confidence: "possible",
            matchedSeason: inlinePrefixSeason,
            matchedEpisode: epNum,
            matchedEpisodeEnd: null,
            officialEpisodeTitle: lookupOfficialTitle(officialSeasons, inlinePrefixSeason, epNum),
            reason: `Season conflict between filename (${formatSeasonLabel(inlinePrefixSeason)}) and folder (${formatSeasonLabel(folderSeason)})`,
          };
        }

        const explicitSeason =
          inlinePrefixSeason ??
          folderSeason ??
          (batch.batchKind === "season-pack" && batch.season !== null ? batch.season : null) ??
          (regularSeasons.length === 1 ? regularSeasons[0].season_number : null);

        if (explicitSeason !== null) {
          const sMeta = seasonByNum.get(explicitSeason);
          const sCap = sMeta
            ? Math.max(sMeta.episode_count || 0, (sMeta.episodes || []).length)
            : 0;
          if (sCap > 0 && epNum > sCap) {
            const absMapped = mapAbsoluteToOfficialSeason(epNum, regularSeasons);
            if (absMapped) {
              return {
                file,
                fileType,
                confidence: "possible",
                matchedSeason: absMapped.season,
                matchedEpisode: absMapped.episode,
                matchedEpisodeEnd: null,
                officialEpisodeTitle: lookupOfficialTitle(
                  officialSeasons,
                  absMapped.season,
                  absMapped.episode,
                ),
                reason: `Absolute episode ${epNum} exceeds ${formatSeasonLabel(explicitSeason)} count; possibly ${formatSeasonLabel(absMapped.season)} E${String(absMapped.episode).padStart(2, "0")}`,
              };
            }
            return {
              file,
              fileType,
              confidence: "possible",
              matchedSeason: explicitSeason,
              matchedEpisode: epNum,
              matchedEpisodeEnd: null,
              officialEpisodeTitle: null,
              reason: `Episode ${epNum} exceeds known episode count (${sCap}) for ${formatSeasonLabel(explicitSeason)}`,
            };
          }
          return {
            file,
            fileType,
            confidence: "confirmed",
            matchedSeason: explicitSeason,
            matchedEpisode: epNum,
            matchedEpisodeEnd: null,
            officialEpisodeTitle: lookupOfficialTitle(officialSeasons, explicitSeason, epNum),
            reason:
              inlinePrefixSeason !== null
                ? `Season-qualified episode ${formatSeasonLabel(explicitSeason)} E${String(epNum).padStart(2, "0")}`
                : folderSeason !== null
                  ? `Matched via ${formatSeasonLabel(folderSeason)} folder + episode ${epNum}`
                  : batch.season !== null
                    ? `Matched via Season ${batch.season} pack context + episode ${epNum}`
                    : `Single-season series episode ${epNum}`,
          };
        }

        // Multi-season show without season tag in filename or folder: never falsely confirm!
        const absMapped = mapAbsoluteToOfficialSeason(epNum, regularSeasons);
        if (absMapped) {
          return {
            file,
            fileType,
            confidence: "possible",
            matchedSeason: absMapped.season,
            matchedEpisode: absMapped.episode,
            matchedEpisodeEnd: null,
            officialEpisodeTitle: lookupOfficialTitle(
              officialSeasons,
              absMapped.season,
              absMapped.episode,
            ),
            reason: `No season tag in path; episode ${epNum} could map to ${formatSeasonLabel(absMapped.season)} E${String(absMapped.episode).padStart(2, "0")}`,
          };
        }

        return {
          file,
          fileType,
          confidence: "possible",
          matchedSeason: 1,
          matchedEpisode: epNum,
          matchedEpisodeEnd: null,
          officialEpisodeTitle: lookupOfficialTitle(officialSeasons, 1, epNum),
          reason: `Episode number ${epNum} found without season identifier in multi-season pack`,
        };
      }
    }

    return {
      file,
      fileType,
      confidence: "unmatched",
      matchedSeason: null,
      matchedEpisode: null,
      matchedEpisodeEnd: null,
      officialEpisodeTitle: null,
      reason: "No reliable season/episode pattern detected in filename",
    };
  });
}
