import type { MediaEntry, MediaSource, QualityTier, ScrapedItem, ScraperConfig } from "./types";

const SIZE_UNIT_MULTIPLIERS: Record<string, number> = {
  "": 1,
  b: 1,
  kb: 1e3,
  kib: 1024,
  mb: 1e6,
  mib: 1024 ** 2,
  gb: 1e9,
  gib: 1024 ** 3,
  tb: 1e12,
  tib: 1024 ** 4,
};

export const QUALITY_RANK: Record<QualityTier, number> = {
  "4K": 4,
  "1080p": 3,
  "720p": 2,
  Other: 1,
};

export function parseSizeBytes(sizeStr: string): number {
  const cleaned = String(sizeStr || "").replace(/,/g, "").trim();
  const match = cleaned.match(/^([0-9]+(?:\.[0-9]+)?)\s*([a-zA-Z]*)$/);
  if (!match) return -1;
  const value = parseFloat(match[1]);
  const unit = match[2].toLowerCase();
  return value * (SIZE_UNIT_MULTIPLIERS[unit] ?? 1);
}

export function stripStatusPrefix(msg: string): string {
  return String(msg || "").replace(/^[✅❌]\s*/, "").trim();
}

export function extractMagnetInfoHash(uri: string): string | null {
  const match = String(uri || "").match(/xt=urn:btih:([a-zA-Z0-9]+)/i);
  return match ? match[1] : null;
}

export function extractHostname(uri: string): string | null {
  const trimmed = (uri || "").trim();
  if (!trimmed.toLowerCase().startsWith("http")) return null;
  try {
    return new URL(trimmed).hostname || null;
  } catch {
    return null;
  }
}

export function parsePeers(stats: string): { seeders: number | null; leechers: number | null } {
  const sMatch = String(stats || "").match(/S:\s*([\d,]+)/i);
  const lMatch = String(stats || "").match(/L:\s*([\d,]+)/i);
  return {
    seeders: sMatch ? Number(sMatch[1].replace(/,/g, "")) : null,
    leechers: lMatch ? Number(lMatch[1].replace(/,/g, "")) : null,
  };
}

function inferQuality(title: string): { tier: QualityTier; label: string | null } {
  const upper = title.toUpperCase();
  if (/\b(2160P|4K|UHD)\b/.test(upper)) return { tier: "4K", label: "2160p" };
  if (/\b1080P\b/.test(upper)) return { tier: "1080p", label: "1080p" };
  if (/\b720P\b/.test(upper)) return { tier: "720p", label: "720p" };
  if (/\b(480P|576P|SD)\b/.test(upper)) return { tier: "Other", label: "SD" };
  return { tier: "Other", label: null };
}

function inferCodec(title: string): string | null {
  const upper = title.toUpperCase();
  if (/\b(X265|H\.?265|HEVC)\b/.test(upper)) return "HEVC / x265";
  if (/\b(X264|H\.?264|AVC)\b/.test(upper)) return "H.264 / x264";
  if (/\bAV1\b/.test(upper)) return "AV1";
  if (/\bVP9\b/.test(upper)) return "VP9";
  return null;
}

function inferContainer(title: string, resolvedLink: string): string | null {
  const combined = `${title} ${resolvedLink}`.toUpperCase();
  if (/\bMKV\b|\.MKV\b/.test(combined)) return "MKV";
  if (/\bMP4\b|\.MP4\b/.test(combined)) return "MP4";
  if (/\bWEBM\b|\.WEBM\b/.test(combined)) return "WEBM";
  if (/\bAVI\b|\.AVI\b/.test(combined)) return "AVI";
  if (/\bISO\b|\.ISO\b/.test(combined)) return "ISO";
  if (/\bZIP\b|\.ZIP\b/.test(combined)) return "ZIP";
  return null;
}

function extractYear(title: string): string | null {
  const match = title.match(/(?:^|[\s.(])((?:19|20)\d{2})(?:[\s.)]|$)/);
  return match ? match[1] : null;
}

function extractSeasonEpisode(title: string): string | null {
  const se = title.match(/\b(S\d{1,2}E\d{1,2})\b/i);
  if (se) return se[1].toUpperCase();
  const seasonOnly = title.match(/\b(S\d{1,2}|Season\s+\d{1,2})\b/i);
  return seasonOnly ? seasonOnly[1].toUpperCase() : null;
}

function cleanDisplayTitle(rawTitle: string): string {
  // Preserve genuine title while trimming release group tags and mirror suffixes for grouping
  let cleaned = rawTitle
    .replace(/^\[[^\]]+\]\s*/, "")
    .replace(/\s+—\s+(Official Release.*|Magnet Mirror.*|Minimal NetInstall.*|Base32 Magnet.*|Documentation.*|Malformed.*|Broken.*)$/i, "")
    .replace(/\s*\((?:40-char Hex BTIH|Validation Test|PDF\/ZIP|Season \d+ Batch)\)\s*$/i, "")
    .replace(/\s*-\s*\d{1,3}(?:v\d)?\b.*$/i, "")
    .trim();

  // Strip trailing release codec/resolution clutter only if meaningful text remains
  const stripped = cleaned
    .replace(/[\s._\-\[\(]+(?:2160p|1080p|720p|480p|x264|x265|hevc|web-?dl|bluray|remux).*$/i, "")
    .trim();
  if (stripped.length >= 3) {
    cleaned = stripped;
  }
  return cleaned || rawTitle;
}

export function adaptScrapedItem(item: ScrapedItem, providerName: string): MediaSource {
  const quality = inferQuality(item.title);
  const parsedPeers = parsePeers(item.stats);
  const seeders = typeof item.seeders === "number" ? item.seeders : parsedPeers.seeders;
  const leechers = typeof item.leechers === "number" ? item.leechers : parsedPeers.leechers;
  const primaryUri = (item.magnet_link || item.resolved_link || "").trim();
  const lowerUri = primaryUri.toLowerCase();

  return {
    item,
    sourceTitle: item.title,
    qualityTier: quality.tier,
    inferredResolution: quality.label,
    inferredCodec: inferCodec(item.title),
    inferredContainer: inferContainer(item.title, primaryUri),
    sizeBytes: parseSizeBytes(item.size),
    seeders,
    leechers,
    isMagnet: lowerUri.startsWith("magnet:"),
    infoHash: extractMagnetInfoHash(primaryUri),
    hostname: extractHostname(item.torrent_link || primaryUri),
    providerName,
  };
}

export function groupItemsToMedia(
  items: ScrapedItem[],
  providerName: string,
): MediaEntry[] {
  const grouped = new Map<string, MediaSource[]>();

  for (const rawItem of items) {
    const source = adaptScrapedItem(rawItem, providerName);
    const titleGroup = cleanDisplayTitle(rawItem.title);
    const key = titleGroup.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || `item-${rawItem.id}`;
    const existing = grouped.get(key) ?? [];
    existing.push(source);
    grouped.set(key, existing);
  }

  const entries: MediaEntry[] = [];
  for (const [key, sources] of grouped.entries()) {
    const primary = sources[0];
    const displayTitle = cleanDisplayTitle(primary.sourceTitle);
    const year = extractYear(primary.sourceTitle);
    const seasonEpisode = extractSeasonEpisode(primary.sourceTitle);

    let bestQuality: QualityTier = "Other";
    let verifiedCount = 0;
    let magnetCount = 0;
    let largestSizeBytes = -1;
    let largestSizeLabel = primary.item.size || "N/A";
    let maxSeeders: number | null = null;
    let latestDate = primary.item.date || "N/A";

    for (const src of sources) {
      if (QUALITY_RANK[src.qualityTier] > QUALITY_RANK[bestQuality]) {
        bestQuality = src.qualityTier;
      }
      if (src.item.is_valid) verifiedCount += 1;
      if (src.isMagnet) magnetCount += 1;
      if (src.sizeBytes > largestSizeBytes) {
        largestSizeBytes = src.sizeBytes;
        largestSizeLabel = src.item.size;
      }
      if (src.seeders !== null && (maxSeeders === null || src.seeders > maxSeeders)) {
        maxSeeders = src.seeders;
      }
      if (src.item.date && src.item.date > latestDate) {
        latestDate = src.item.date;
      }
    }

    entries.push({
      key,
      title: displayTitle,
      year,
      seasonEpisode,
      mediaType: seasonEpisode ? "series" : "release",
      sources,
      bestQuality,
      verifiedCount,
      magnetCount,
      latestDate,
      largestSizeLabel,
      largestSizeBytes,
      maxSeeders,
      providerName,
    });
  }

  return entries;
}

const ARTWORK_PALETTES = [
  { from: "#1B1638", via: "#111526", glow: "rgba(120, 107, 255, 0.34)", accent: "#786BFF" },
  { from: "#0F2233", via: "#0D1321", glow: "rgba(56, 189, 248, 0.30)", accent: "#38BDF8" },
  { from: "#182436", via: "#0F1422", glow: "rgba(129, 140, 248, 0.28)", accent: "#818CF8" },
  { from: "#122726", via: "#0D151D", glow: "rgba(52, 211, 153, 0.24)", accent: "#34D399" },
  { from: "#231934", via: "#121322", glow: "rgba(167, 139, 250, 0.28)", accent: "#A78BFA" },
] as const;

export function getMediaArtPalette(title: string) {
  let hash = 0;
  for (let i = 0; i < title.length; i += 1) {
    hash = (hash * 31 + title.charCodeAt(i)) >>> 0;
  }
  return ARTWORK_PALETTES[hash % ARTWORK_PALETTES.length];
}

export function buildPreviewUrl(cfg: Partial<ScraperConfig>, sampleQuery = "frieren 1080p"): string {
  const base = (cfg.base_url || "").trim().replace(/\/+$/, "");
  const pathRaw = (cfg.search_path || "/").trim();
  const path = pathRaw.startsWith("/") ? pathRaw : `/${pathRaw}`;
  const param = (cfg.query_param || "q").trim() || "q";
  if (!base) return "";
  const encoded = encodeURIComponent(sampleQuery.trim()).replace(/%20/g, "+");
  if (!encoded) return `${base}${path}?${param}=&f=0&c=1_2&s=leechers&o=desc`;
  return `${base}${path}?f=0&c=1_2&${encodeURIComponent(param)}=${encoded}&s=seeders&o=desc`;
}
