import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { BatchEpisodeList, type BatchInspectionState } from "../components/BatchEpisodeList";
import { MediaDetailsView } from "../components/MediaDetailsView";
import {
  type BatchEntry,
  buildBatchOfficialCatalog,
  buildEpisodeCatalog,
  formatSeasonLabel,
  matchManifestFilesToEpisodes,
  selectDefaultBatchSeason,
} from "./episodeCatalog";
import { adaptMetadataToMediaEntry } from "./mediaAdapter";
import type {
  MediaIdentity,
  ScrapedItem,
  SeasonMetadata,
  TorrentManifestFile,
} from "./types";

function assert(condition: unknown, message: string): void {
  if (!condition) {
    throw new Error(`Assertion failed: ${message}`);
  }
}

function makeBatchSource(
  id: number,
  title: string,
  btih: string,
  seeders = 150,
  torrentLink?: string,
): ScrapedItem {
  const magnet = `magnet:?xt=urn:btih:${btih}&dn=${encodeURIComponent(title)}`;
  return {
    id,
    title,
    size: "42.5 GiB",
    date: "2026-11-15 08:00",
    stats: `${seeders}S / 10L`,
    raw_link: magnet,
    resolved_link: magnet,
    is_valid: true,
    verification_msg: "Valid BTIH",
    view_url: `https://nyaa.si/view/${2100000 + id}`,
    magnet_link: magnet,
    torrent_link: torrentLink,
    info_hash: btih,
    seeders,
    leechers: 10,
    downloads: 1000,
    provider_id: "nyaa",
    provider_name: "Nyaa",
  };
}

const AOT_SEASONS: SeasonMetadata[] = [
  {
    season_number: 0,
    title: "Specials",
    episode_count: 2,
    air_year: 2013,
    episodes: [
      { season_number: 0, episode_number: 1, title: "Ilse's Notebook", air_date: "2013-12-09" },
      { season_number: 0, episode_number: 2, title: "" }, // tests Title unavailable
    ],
  },
  {
    season_number: 1,
    title: "Season 1",
    episode_count: 25,
    air_year: 2013,
    episodes: [
      {
        season_number: 1,
        episode_number: 1,
        title: "To You, in 2000 Years",
        air_date: "2013-04-07",
        overview: "The Colossal Titan breaches Wall Maria.",
        still_url: "https://example.invalid/aot_s1e1.jpg",
      },
      { season_number: 1, episode_number: 2, title: "That Day", air_date: "2013-04-14" },
      {
        season_number: 1,
        episode_number: 3,
        title: "A Dim Light Amid Despair",
        air_date: "2013-04-21",
      },
      ...Array.from({ length: 22 }, (_, i) => ({
        season_number: 1,
        episode_number: i + 4,
        title: `Episode ${i + 4}`,
      })),
    ],
  },
  {
    season_number: 2,
    title: "Season 2",
    episode_count: 12,
    air_year: 2017,
    episodes: Array.from({ length: 12 }, (_, i) => ({
      season_number: 2,
      episode_number: i + 1,
      title: i === 0 ? "Beast Titan" : `Season 2 Episode ${i + 1}`,
    })),
  },
];

const AOT_IDENTITY: MediaIdentity = {
  canonical_id: "tmdb:tv:1429",
  official_title: "Attack on Titan",
  original_title: "進撃の巨人",
  romaji_title: "Shingeki no Kyojin",
  alternate_titles: ["AoT"],
  release_year: 2013,
  media_type: "anime",
  synopsis: "Humanity fights for survival behind giant walls.",
  genres: ["Action", "Drama"],
  status: "FINISHED",
  poster_url: "",
  backdrop_url: "",
  seasons: AOT_SEASONS,
  metadata_provider: "TMDB",
  confidence: 1.0,
};

export function runBatchEpisodeListTests(): void {
  // Scenario A: Batch-only Attack on Titan series (0 individual episode torrents)
  const completeItem = makeBatchSource(
    1,
    "[SubsPlease] Attack on Titan - Complete Collection (1080p) [HEVC]",
    "1111111111111111111111111111111111111111",
    400,
    "https://nyaa.si/download/2100001.torrent",
  );
  const s2PackItem = makeBatchSource(
    2,
    "[Ember] Attack on Titan - Season 2 Batch (1080p)",
    "2222222222222222222222222222222222222222",
    200,
  );
  const entry = adaptMetadataToMediaEntry(AOT_IDENTITY, [completeItem, s2PackItem], "Nyaa");
  const catalog = buildEpisodeCatalog(entry.sources, entry.officialSeasons, entry.mediaKind);

  assert(catalog.episodes.length === 0, "Batch-only series has zero individual episode torrents");
  assert(catalog.batches.length === 2, "Expected 2 batch packs");

  // Complete-series pack includes all seasons (1, 2, 0=Specials) & defaults to Season 1
  const completeBatch = catalog.batches[0];
  const completeOfficial = buildBatchOfficialCatalog(completeBatch, AOT_SEASONS, "TMDB");
  assert(
    JSON.stringify(completeOfficial.seasonNumbers) === JSON.stringify([1, 2, 0]),
    `Expected all official seasons [1, 2, 0], got ${JSON.stringify(completeOfficial.seasonNumbers)}`,
  );
  assert(completeOfficial.defaultSeason === 1, "Complete series must select Season 1 by default");
  assert(formatSeasonLabel(0) === "Specials", "Season 0 must be labeled Specials");
  assert(
    completeOfficial.seasons[0].episodes[0].title === "To You, in 2000 Years",
    "Official episode title should be preserved independently of individual torrents",
  );
  const specialsGroup = completeOfficial.seasons.find((s) => s.seasonNumber === 0);
  assert(
    specialsGroup && specialsGroup.episodes[1].title === "Title unavailable",
    "Missing episode name must be labeled Title unavailable",
  );

  // Even if Season 0 comes first in input list, selectDefaultBatchSeason picks Season 1
  assert(
    selectDefaultBatchSeason({ batchKind: "complete-series", season: null }, [0, 1, 2]) === 1,
    "Season 1 must win default selection over Season 0",
  );

  // Season 2 pack defaults to Season 2
  const s2Batch = catalog.batches[1];
  const s2Official = buildBatchOfficialCatalog(s2Batch, AOT_SEASONS, "TMDB");
  assert(s2Official.defaultSeason === 2, "Season 2 pack must default to Season 2");

  // Scenario B: Single-season batch + reliable multi-episode pack
  const singleSeason: SeasonMetadata[] = [
    {
      season_number: 1,
      title: "Season 1",
      episode_count: 12,
      episodes: Array.from({ length: 12 }, (_, i) => ({
        season_number: 1,
        episode_number: i + 1,
        title: `Bocchi Ep ${i + 1}`,
      })),
    },
  ];
  const rangeBatch: BatchEntry = {
    season: null,
    firstEpisode: 1,
    lastEpisode: 6,
    batchKind: "multi-episode",
    batchLabel: "Episodes 01–06 Range",
    source: entry.sources[0],
  };
  const singleRangeOfficial = buildBatchOfficialCatalog(rangeBatch, singleSeason, "AniList");
  assert(
    singleRangeOfficial.coverageMode === "inferred-range" &&
      singleRangeOfficial.seasons[0].episodes.length === 6,
    "Single-season multi-episode pack should filter reliably to E01-E06",
  );

  // Scenario C: Ambiguous multi-episode pack on multi-season show -> unverified coverage notice
  const ambigMultiSeasonOfficial = buildBatchOfficialCatalog(rangeBatch, AOT_SEASONS, "TMDB");
  assert(
    ambigMultiSeasonOfficial.coverageUnverified === true &&
      ambigMultiSeasonOfficial.seasons[0].episodes.length === 25,
    "Ambiguous multi-season range pack without season must keep full catalog with unverified notice",
  );

  // Scenario D: Unavailable official metadata
  const unavailableOfficial = buildBatchOfficialCatalog(completeBatch, [], "Nyaa");
  assert(
    unavailableOfficial.coverageUnverified && unavailableOfficial.seasons.length === 0,
    "Empty officialSeasons produces honest unverified state",
  );

  // Scenario E: Conservative filename-to-episode matching (confirmed, possible, unmatched)
  const manifestFiles: TorrentManifestFile[] = [
    {
      relative_path: "Season 1/[SubsPlease] Attack on Titan S01E01 (1080p).mkv",
      filename: "[SubsPlease] Attack on Titan S01E01 (1080p).mkv",
      size_bytes: 1480000000,
      size_label: "1.38 GiB",
      file_type: "video",
      extension: ".mkv",
    },
    {
      relative_path: "Season 1/[SubsPlease] Attack on Titan S01E01.eng.ass",
      filename: "[SubsPlease] Attack on Titan S01E01.eng.ass",
      size_bytes: 95400,
      size_label: "93.2 KiB",
      file_type: "subtitle",
      extension: ".ass",
    },
    {
      relative_path: "[Group] Attack on Titan - 03 [1080p].mkv",
      filename: "[Group] Attack on Titan - 03 [1080p].mkv",
      size_bytes: 1400000000,
      size_label: "1.3 GiB",
      file_type: "video",
      extension: ".mkv",
    },
    {
      relative_path: "Season 2/Attack on Titan S01E05 (1080p).mkv",
      filename: "Attack on Titan S01E05 (1080p).mkv",
      size_bytes: 1350000000,
      size_label: "1.26 GiB",
      file_type: "video",
      extension: ".mkv",
    },
    {
      relative_path: "Extras/Bonus Recap 01.mkv",
      filename: "Bonus Recap 01.mkv",
      size_bytes: 320000000,
      size_label: "305.2 MiB",
      file_type: "video",
      extension: ".mkv",
    },
    {
      relative_path: "Extras/NCOP1.mkv",
      filename: "NCOP1.mkv",
      size_bytes: 120000000,
      size_label: "114.4 MiB",
      file_type: "video",
      extension: ".mkv",
    },
    {
      relative_path: "ReadMe.txt",
      filename: "ReadMe.txt",
      size_bytes: 1024,
      size_label: "1 KiB",
      file_type: "auxiliary",
      extension: ".txt",
    },
  ];

  const matched = matchManifestFilesToEpisodes(manifestFiles, completeBatch, AOT_SEASONS);
  assert(matched[0].confidence === "confirmed" && matched[0].matchedEpisode === 1, "Explicit S01E01 video must confirm");
  assert(matched[0].officialEpisodeTitle === "To You, in 2000 Years", "Matched episode displays official title context");
  assert(matched[1].fileType === "subtitle" && matched[1].confidence === "confirmed", "Subtitle matches independently while typed as subtitle");
  assert(matched[2].confidence === "possible", "Ambiguous root `- 03.mkv` in multi-season complete pack must NOT be confirmed");
  assert(matched[3].confidence === "possible", "Conflicting Season 2 folder vs S01E05 filename must be possible, not confirmed");
  assert(matched[4].confidence === "unmatched", "Bonus Recap 01 must NOT confirm as Episode 1");
  assert(matched[5].confidence === "unmatched", "NCOP1 must NOT match Episode 1");
  assert(matched[6].fileType === "auxiliary" && matched[6].confidence === "unmatched", "ReadMe.txt must remain auxiliary/unmatched");

  // Scenario F: Render MediaDetailsView and verify Show Episode List sits beside Show magnet URI
  const detailsHtml = renderToStaticMarkup(
    createElement(MediaDetailsView, {
      media: entry,
      isSaved: false,
      engineStatus: null,
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
    }),
  );
  assert(detailsHtml.includes("Show magnet URI"), "Batch card renders Show magnet URI");
  assert(detailsHtml.includes("Show Episode List"), "Batch card renders Show Episode List");

  // Scenario G: Render BatchEpisodeList in Official, Verified, Unavailable, Loading, Error states
  const idleState: BatchInspectionState = { status: "idle" };
  const officialHtml = renderToStaticMarkup(
    createElement(BatchEpisodeList, {
      panelId: "p1",
      batch: completeBatch,
      officialSeasons: AOT_SEASONS,
      metadataProvider: "TMDB",
      activeTab: "official",
      onChangeTab: () => {},
      selectedSeason: null,
      onSelectSeason: () => {},
      inspectionState: idleState,
      onInspectManifest: () => {},
    }),
  );
  assert(officialHtml.includes("To You, in 2000 Years"), "Official panel renders official episode title");
  assert(officialHtml.includes("Specials"), "Official panel renders Season 0 as Specials");
  assert(officialHtml.includes("Torrent file inspection: Not performed"), "Official panel separates inspection status");

  const verifiedHtml = renderToStaticMarkup(
    createElement(BatchEpisodeList, {
      panelId: "p2",
      batch: completeBatch,
      officialSeasons: AOT_SEASONS,
      metadataProvider: "TMDB",
      activeTab: "verified",
      onChangeTab: () => {},
      selectedSeason: 0,
      onSelectSeason: () => {},
      inspectionState: {
        status: "done",
        result: {
          status: "verified",
          info_hash: "1111111111111111111111111111111111111111",
          source_kind: "torrent_url",
          cached: false,
          torrent_name: "[SubsPlease] Attack on Titan - Batch (1080p)",
          total_files: manifestFiles.length,
          total_size_bytes: 3415496424,
          total_size_label: "3.18 GiB",
          video_count: 5,
          subtitle_count: 1,
          auxiliary_count: 1,
          error: null,
          files: manifestFiles,
        },
      },
      onInspectManifest: () => {},
    }),
  );
  assert(verifiedHtml.includes("Confirmed · S01E01"), "Verified panel shows confirmed match badge");
  assert(verifiedHtml.includes("Possible · S01E03") || verifiedHtml.includes("Possible · S01E05"), "Verified panel shows possible match badge");
  assert(verifiedHtml.includes("Unmatched"), "Verified panel shows unmatched files distinctly");
  assert(!verifiedHtml.includes("Copy magnet link"), "Verified panel must NOT generate per-file magnet links");

  const loadingHtml = renderToStaticMarkup(
    createElement(BatchEpisodeList, {
      panelId: "p3",
      batch: completeBatch,
      officialSeasons: AOT_SEASONS,
      metadataProvider: "TMDB",
      activeTab: "verified",
      onChangeTab: () => {},
      selectedSeason: null,
      onSelectSeason: () => {},
      inspectionState: { status: "loading" },
      onInspectManifest: () => {},
    }),
  );
  assert(loadingHtml.includes("Fetching and validating"), "Loading state renders cleanly");

  const unavailableHtml = renderToStaticMarkup(
    createElement(BatchEpisodeList, {
      panelId: "p4",
      batch: completeBatch,
      officialSeasons: AOT_SEASONS,
      metadataProvider: "TMDB",
      activeTab: "verified",
      onChangeTab: () => {},
      selectedSeason: null,
      onSelectSeason: () => {},
      inspectionState: {
        status: "done",
        result: {
          status: "unavailable",
          info_hash: "2222222222222222222222222222222222222222",
          source_kind: "magnet_only",
          cached: false,
          torrent_name: "",
          total_files: 0,
          total_size_bytes: 0,
          total_size_label: "0 B",
          video_count: 0,
          subtitle_count: 0,
          auxiliary_count: 0,
          error: null,
          inspection_note: "Magnet-only release without .torrent URL.",
          files: [],
        },
      },
      onInspectManifest: () => {},
    }),
  );
  assert(unavailableHtml.includes("Verified file manifest unavailable"), "Unavailable state renders clearly");

  const errorHtml = renderToStaticMarkup(
    createElement(BatchEpisodeList, {
      panelId: "p5",
      batch: completeBatch,
      officialSeasons: AOT_SEASONS,
      metadataProvider: "TMDB",
      activeTab: "verified",
      onChangeTab: () => {},
      selectedSeason: null,
      onSelectSeason: () => {},
      inspectionState: {
        status: "done",
        error: "Malformed bencode header",
      },
      onInspectManifest: () => {},
    }),
  );
  assert(errorHtml.includes("Malformed bencode header") && errorHtml.includes("Retry"), "Error state renders error text and Retry");

  console.log("ALL_BATCH_EPISODE_LIST_TS_TESTS_PASSED");
}

runBatchEpisodeListTests();
