import type { MediaSource } from "./types";

export type ReleaseClassification = "episode" | "batch" | "unclassified";

export interface ReleaseIdentity {
  showTitle: string;
  season: number | null;
  firstEpisode: number | null;
  lastEpisode: number | null;
  classification: ReleaseClassification;
}

export interface EpisodeEntry {
  season: number;
  number: number;
  sources: MediaSource[];
}

export interface BatchEntry {
  season: number | null;
  firstEpisode: number | null;
  lastEpisode: number | null;
  source: MediaSource;
}

function tidyTitle(value: string): string {
  return value
    .replace(/^\s*\[[^\]]+\]\s*/, "")
    .replace(/[._]/g, " ")
    .replace(/\s{2,}/g, " ")
    .replace(/[\s\-–—]+$/, "")
    .trim();
}

/**
 * Parse release naming conventions conservatively. Never fabricate episode
 * numbers for a complete/season torrent whose contents have not been probed.
 */
export function parseReleaseTitle(raw: string): ReleaseIdentity {
  const text = tidyTitle(raw);
  const epMatch =
    text.match(/^(.*?)\s+S(\d{1,2})\s*E(\d{1,3})(?:\s*[-~]\s*E?(\d{1,3}))?(?=\b|[\s[(])/i) ??
    text.match(/^(.*?)\s+Season\s*(\d{1,2})\s*(?:Episode|Ep\.?|E)\s*(\d{1,3})(?:\s*[-~]\s*(\d{1,3}))?/i);
  if (epMatch) {
    const first = Number(epMatch[3]);
    const last = epMatch[4] ? Number(epMatch[4]) : first;
    if (last >= first && last - first <= 200) {
      return {
        showTitle: tidyTitle(epMatch[1]), season: Number(epMatch[2]),
        firstEpisode: first, lastEpisode: last,
        classification: last > first ? "batch" : "episode",
      };
    }
  }

  // Common anime names: [SubsPlease] Show Name - 01 (1080p), Show - 01-12 Batch.
  const dashed = text.match(/^(.*?)\s+[-–—]\s*(?:Ep(?:isode)?\.?\s*)?(\d{1,3})(?:\s*[-~]\s*(\d{1,3}))?(?=$|\s|[[(])/i);
  if (dashed) {
    const first = Number(dashed[2]);
    const last = dashed[3] ? Number(dashed[3]) : first;
    if (last >= first && last - first <= 200) {
      return {
        showTitle: tidyTitle(dashed[1]), season: 1,
        firstEpisode: first, lastEpisode: last,
        classification: last > first ? "batch" : "episode",
      };
    }
  }

  const seasonMatch = text.match(/^(.*?)\s+(?:Season\s*|S)(\d{1,2})(?=$|\s|[[(])/i);
  if (seasonMatch) {
    return {
      showTitle: tidyTitle(seasonMatch[1]), season: Number(seasonMatch[2]),
      firstEpisode: null, lastEpisode: null, classification: "batch",
    };
  }

  const batchMatch = text.match(/^(.*?)\s*(?:[-–—]\s*)?(?:Complete\s*(?:Season|Series)|Batch|Season\s*Pack|Complete\s*Pack)(?=$|\s|[[(])/i);
  if (batchMatch) {
    return {
      showTitle: tidyTitle(batchMatch[1]), season: null,
      firstEpisode: null, lastEpisode: null, classification: "batch",
    };
  }

  const withoutReleaseTags = text
    .replace(/(?:\s*[[(][^\])]*(?:2160p|1080p|720p|480p|x26[45]|HEVC|AV1|WEB[- ]?DL|BDRip|BluRay)[^\])]*[\])])+$/gi, "")
    .trim();
  return {
    showTitle: tidyTitle(withoutReleaseTags) || text, season: null,
    firstEpisode: null, lastEpisode: null, classification: "unclassified",
  };
}

export function showKey(title: string): string {
  return title.toLowerCase().normalize("NFKC").replace(/[^\p{L}\p{N}]+/gu, "-").replace(/^-|-$/g, "");
}

export function buildEpisodeCatalog(sources: MediaSource[]): {
  episodes: EpisodeEntry[];
  batches: BatchEntry[];
  other: MediaSource[];
  seasons: number[];
} {
  const episodeMap = new Map<string, EpisodeEntry>();
  const batches: BatchEntry[] = [];
  const other: MediaSource[] = [];

  for (const source of sources) {
    const parsed = parseReleaseTitle(source.sourceTitle);
    if (parsed.classification === "episode" && parsed.firstEpisode !== null) {
      const season = parsed.season ?? 1;
      const key = `${season}:${parsed.firstEpisode}`;
      const entry = episodeMap.get(key) ?? { season, number: parsed.firstEpisode, sources: [] };
      entry.sources.push(source);
      episodeMap.set(key, entry);
    } else if (parsed.classification === "batch") {
      batches.push({
        season: parsed.season, firstEpisode: parsed.firstEpisode,
        lastEpisode: parsed.lastEpisode, source,
      });
    } else {
      other.push(source);
    }
  }

  const episodes = [...episodeMap.values()].sort((a, b) => a.season - b.season || a.number - b.number);
  const seasons = [...new Set([
    ...episodes.map((entry) => entry.season),
    ...batches.map((batch) => batch.season).filter((season): season is number => season !== null),
  ])].sort((a, b) => a - b);
  return { episodes, batches, other, seasons };
}
