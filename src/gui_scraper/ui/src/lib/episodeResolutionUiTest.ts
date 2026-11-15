import React from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { EpisodeSourcesPanel } from "../components/EpisodeSourcesPanel";
import { MediaDetailsView } from "../components/MediaDetailsView";
import { buildEpisodeCatalog, buildEpisodeFirstSeasons } from "./episodeCatalog";
import type {
  EpisodeResolutionResult,
  MediaEntry,
  MediaSource,
  SeasonMetadata,
} from "./types";

const assert = {
  ok(condition: unknown, message = "Expected truthy value"): void {
    if (!condition) {
      throw new Error(`Assertion failed: ${message}`);
    }
  },
  equal(actual: unknown, expected: unknown, message?: string): void {
    if (actual !== expected) {
      throw new Error(message ?? `Expected ${String(expected)}, got ${String(actual)}`);
    }
  },
  deepEqual(actual: unknown, expected: unknown, message?: string): void {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    if (a !== e) {
      throw new Error(message ?? `Expected ${e}, got ${a}`);
    }
  },
};

function makeSource(id: number, title: string): MediaSource {
  const ih = id.toString(16).padStart(40, "a");
  const magnet = `magnet:?xt=urn:btih:${ih}&dn=${encodeURIComponent(title)}`;
  return {
    sourceTitle: title,
    qualityTier: "1080p",
    inferredResolution: "1080p",
    inferredCodec: "HEVC / x265",
    inferredContainer: "MKV",
    sizeBytes: 4_831_838_208,
    seeders: 120,
    leechers: 12,
    isMagnet: true,
    infoHash: ih,
    hostname: "nyaa.si",
    providerName: "Nyaa",
    item: {
      id,
      title,
      size: "4.5 GiB",
      date: "2025-01-01",
      stats: "120 S / 12 L",
      raw_link: magnet,
      resolved_link: magnet,
      magnet_link: magnet,
      is_valid: true,
      verification_msg: "ok",
    },
  };
}

// 1. Official episode-first seasons work even when 0 individual torrents exist (batch-only show)
const officialSeasons: SeasonMetadata[] = [
  {
    season_number: 0,
    title: "Specials",
    episode_count: 1,
    episodes: [{ season_number: 0, episode_number: 1, title: "OVA Prologue", is_special: true }],
  },
  {
    season_number: 1,
    title: "Season 1",
    episode_count: 3,
    episodes: [
      { season_number: 1, episode_number: 1, title: "Pilot", absolute_episode: 1 },
      { season_number: 1, episode_number: 2, title: "Cat's in the Bag", absolute_episode: 2 },
      { season_number: 1, episode_number: 3, title: "And the Bag's in the River", absolute_episode: 3 },
    ],
  },
];

const batchOnlySources = [makeSource(1, "Breaking Bad Season 1 Complete Batch [1080p]")];
const rawCatalog = buildEpisodeCatalog(batchOnlySources, officialSeasons, "tv");
assert.equal(rawCatalog.episodes.length, 0);

const epFirst = buildEpisodeFirstSeasons(officialSeasons, rawCatalog, false);
// Regular seasons come BEFORE Season 0 (Specials)
assert.deepEqual(epFirst.seasons, [1, 0]);
assert.equal(epFirst.episodesBySeason.get(1)?.length, 3);
assert.equal(epFirst.episodesBySeason.get(1)?.[2].title, "And the Bag's in the River");

// 2. Render EpisodeSourcesPanel verifying Confirmed / Probable / Possible / Unresolved sections
const mockResolution: EpisodeResolutionResult = {
  show_id: "tmdb:tv:1396",
  show_title: "Breaking Bad",
  season: 1,
  episode: 3,
  episode_title: "And the Bag's in the River",
  absolute_episode: 3,
  resolution_stage: "verified",
  candidates: [
    {
      candidate_id: "hash1:2",
      info_hash: "1".repeat(40),
      magnet_uri: "magnet:?xt=urn:btih:" + "1".repeat(40),
      torrent_url: "https://nyaa.si/download/1.torrent",
      release_title: "Breaking Bad Season 1 Batch [1080p]",
      provider_id: "nyaa",
      provider_name: "Nyaa",
      torrent_file_index: 2,
      file_path: "Season 1/Episode 03.mkv",
      filename: "Episode 03.mkv",
      file_size_bytes: 450_000_000,
      file_size_label: "429.2 MiB",
      confidence: 0.98,
      status: "confirmed",
      can_auto_play: true,
      source_kind: "torrent_url",
      is_batch_source: true,
      resolution: "1080p",
      codec: "HEVC / x265",
      subtitle_lang: "ENG",
      seeders: 210,
      leechers: 14,
      score: 11500,
      evidence: ["canonical show matched", "verified torrent manifest", "season 1 folder matched"],
      contradictions: [],
    },
    {
      candidate_id: "hash2:0",
      info_hash: "2".repeat(40),
      magnet_uri: "magnet:?xt=urn:btih:" + "2".repeat(40),
      torrent_url: "",
      release_title: "Breaking.Bad.S01E03-E04.720p.mkv",
      provider_id: "eztv",
      provider_name: "EZTV",
      torrent_file_index: 0,
      file_path: "Breaking.Bad.S01E03-E04.720p.mkv",
      filename: "Breaking.Bad.S01E03-E04.720p.mkv",
      file_size_bytes: 820_000_000,
      file_size_label: "782.0 MiB",
      confidence: 0.78,
      status: "probable",
      can_auto_play: false,
      source_kind: "torrent_url",
      is_batch_source: false,
      resolution: "720p",
      codec: "H.264 / x264",
      subtitle_lang: "",
      seeders: 45,
      leechers: 2,
      score: 6800,
      evidence: ["canonical show matched", "multi-episode file contains episode 3"],
      contradictions: ["multi-episode range file requires explicit user selection"],
    },
  ],
  possible_matches: [],
  unresolved_candidates: [
    {
      candidate_id: "hash3:unverified",
      info_hash: "3".repeat(40),
      magnet_uri: "magnet:?xt=urn:btih:" + "3".repeat(40),
      torrent_url: "",
      release_title: "Breaking Bad DVD Disc Pack",
      provider_id: "apibay",
      provider_name: "TPB",
      torrent_file_index: null,
      file_path: "Disc 1.iso",
      filename: "Disc 1.iso",
      file_size_bytes: 4_500_000_000,
      file_size_label: "4.19 GiB",
      confidence: 0.25,
      status: "unresolved",
      can_auto_play: false,
      source_kind: "magnet_only",
      is_batch_source: true,
      resolution: "",
      codec: "",
      subtitle_lang: "",
      seeders: 30,
      leechers: 5,
      score: 1200,
      evidence: ["canonical show matched"],
      contradictions: ["disc image (.iso/.img) cannot target an episode stream directly"],
    },
  ],
  rejected_count: 5,
  has_more: false,
  errors: [],
  metrics: {
    ranking_ms: 1.5,
    manifest_fetch_ms: 18.2,
    total_ms: 21.4,
    cache_hits: 2,
    cache_misses: 1,
    manifest_requests: 1,
    deduplicated_torrents: 1,
    candidates_considered: 4,
    candidates_inspected: 3,
    cancelled: false,
  },
  request_id: "req-test-1",
};

const panelHtml = renderToStaticMarkup(
  React.createElement(EpisodeSourcesPanel, {
    season: 1,
    episodeNumber: 3,
    episodeTitle: "And the Bag's in the River",
    isMovie: false,
    resolutionState: { status: "done", result: mockResolution },
    fallbackSources: [],
    engineAvailable: true,
    onResolve: () => {},
    onCancelResolution: () => {},
    onPlayCandidate: () => {},
    onCopyMagnetText: async () => true,
  }),
);

assert.ok(panelHtml.includes("Confirmed Target"));
assert.ok(panelHtml.includes("File #2"));
assert.ok(panelHtml.includes("Season 1/Episode 03.mkv"));
assert.ok(panelHtml.includes("Play Episode"));
assert.ok(panelHtml.includes("Probable — Review"));
assert.ok(panelHtml.includes("Play Selected File"));
assert.ok(panelHtml.includes("Unresolved Torrents (1)"));
assert.ok(panelHtml.includes("Unverified File Target"));

// 3. Render full MediaDetailsView ensuring Episode-First grid + Batch Show Episode List coexist
const mediaEntry: MediaEntry = {
  key: "tmdb:tv:1396",
  title: "Breaking Bad",
  alternateTitles: [],
  year: "2008",
  seasonEpisode: null,
  mediaType: "series",
  mediaKind: "tv",
  synopsis: "Chemistry teacher turned manufacturer.",
  genres: ["Drama", "Crime"],
  posterUrl: "",
  backdropUrl: "",
  providerName: "TMDB",
  metadataProvider: "TMDB",
  bestQuality: "1080p",
  verifiedCount: 1,
  magnetCount: 1,
  latestDate: "2025-01-01",
  largestSizeLabel: "4.5 GiB",
  largestSizeBytes: 4_831_838_208,
  maxSeeders: 120,
  sources: batchOnlySources,
  officialSeasons,
};

const detailsHtml = renderToStaticMarkup(
  React.createElement(MediaDetailsView, {
    media: mediaEntry,
    isSaved: false,
    engineStatus: {
      intezam_binary_available: true,
      intezam_binary_path: "/usr/local/bin/intezam",
      vlc_available: true,
      mpv_available: false,
      rolling_cache_policy: "2.5%",
      stream_active: false,
    },
    streamState: null,
    verifyingId: null,
    strictCacheBudget: true,
    onBack: () => {},
    onToggleLibrary: () => {},
    onVerifySource: () => {},
    onCopyUri: async () => true,
    onLaunchIntezam: () => {},
    onStopIntezam: () => {},
    onOpenExternal: () => {},
    onResolveEpisode: async () => mockResolution,
  }),
);

assert.ok(detailsHtml.includes("Season 1"));
assert.ok(detailsHtml.includes("Specials"));
assert.ok(detailsHtml.includes("And the Bag&#x27;s in the River") || detailsHtml.includes("And the Bag's in the River"));
assert.ok(detailsHtml.includes("Show Episode List"));
assert.ok(detailsHtml.includes("Show magnet URI"));

console.log("ALL_EPISODE_RESOLUTION_UI_TESTS_PASSED");
