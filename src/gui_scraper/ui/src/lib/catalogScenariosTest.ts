import {
  buildEpisodeCatalog,
  parseReleaseTitle,
} from "./episodeCatalog";
import {
  adaptMetadataToMediaEntry,
  extractMagnetInfoHash,
  groupItemsToMedia,
  mergeSourcesIntoMediaEntry,
} from "./mediaAdapter";
import type { MediaIdentity, ScrapedItem } from "./types";

function assert(condition: unknown, message: string): void {
  if (!condition) {
    throw new Error(`Assertion failed: ${message}`);
  }
}

function makeItem(
  id: number,
  title: string,
  magnetHash: string | null,
  seeders = 100,
  size = "1.4 GiB",
): ScrapedItem {
  const magnet = magnetHash
    ? `magnet:?xt=urn:btih:${magnetHash}&dn=${encodeURIComponent(title)}`
    : undefined;
  const torrent = `https://nyaa.si/download/${1900000 + id}.torrent`;
  return {
    id,
    title,
    size,
    date: "2026-11-14 12:00",
    stats: `${seeders}S / 12L`,
    raw_link: magnet || torrent,
    resolved_link: magnet || torrent,
    is_valid: true,
    verification_msg: "Verified",
    view_url: `https://nyaa.si/view/${1900000 + id}`,
    magnet_link: magnet,
    torrent_link: torrent,
    seeders,
    leechers: 12,
    downloads: 500,
  };
}

export function runAllCatalogScenarioTests(): void {
  // 1 & 2. Multiple resolutions under one episode + multiple episodes under one show
  const items1 = [
    makeItem(
      1,
      "[SubsPlease] Sousou no Frieren - 01 (1080p) [1111AAAA].mkv",
      "1111111111111111111111111111111111111111",
      500,
    ),
    makeItem(
      2,
      "[Erai-raws] Sousou no Frieren - 01 [720p][AAC].mkv",
      "2222222222222222222222222222222222222222",
      200,
    ),
    makeItem(
      3,
      "[SubsPlease] Sousou no Frieren - 02 (1080p) [3333CCCC].mkv",
      "3333333333333333333333333333333333333333",
      410,
    ),
  ];
  const shows1 = groupItemsToMedia(items1, "Nyaa");
  assert(shows1.length === 1, `Expected 1 grouped show, got ${shows1.length}`);
  const catalog1 = buildEpisodeCatalog(shows1[0].sources);
  assert(catalog1.seasons.length === 1 && catalog1.seasons[0] === 1, "Expected Season 1");
  assert(
    catalog1.episodes.length === 2,
    `Expected 2 episodes, got ${catalog1.episodes.length}`,
  );
  assert(
    catalog1.episodes[0].sources.length === 2,
    "Expected 2 resolution sources under Episode 1",
  );

  // 3. Season 1 and Season 2 are correctly separated across naming conventions
  const seasonItems = [
    makeItem(
      10,
      "[SubsPlease] Frieren - 01 (1080p).mkv",
      "aaaa111111111111111111111111111111111111",
    ),
    makeItem(
      11,
      "[SubsPlease] Frieren S02E01 (1080p).mkv",
      "aaaa222222222222222222222222222222222222",
    ),
    makeItem(
      12,
      "[Erai-raws] Frieren Season 2 Episode 3 [1080p].mkv",
      "aaaa333333333333333333333333333333333333",
    ),
    makeItem(
      13,
      "[SubsPlease] Frieren Season 2 - 04 (1080p).mkv",
      "aaaa444444444444444444444444444444444444",
    ),
    makeItem(
      14,
      "[ToonsHub] Frieren 2nd Season - 05 (2160p HEVC).mkv",
      "aaaa555555555555555555555555555555555555",
    ),
  ];
  const seasonShows = groupItemsToMedia(seasonItems, "Nyaa");
  assert(
    seasonShows.length === 1,
    `Expected all Frieren season items under 1 show, got ${seasonShows.length}`,
  );
  const seasonCatalog = buildEpisodeCatalog(seasonShows[0].sources);
  assert(
    JSON.stringify(seasonCatalog.seasons) === JSON.stringify([1, 2]),
    `Expected seasons [1, 2], got ${JSON.stringify(seasonCatalog.seasons)}`,
  );
  const s2Numbers = seasonCatalog.episodes
    .filter((e) => e.season === 2)
    .map((e) => e.number);
  assert(
    JSON.stringify(s2Numbers) === JSON.stringify([1, 3, 4, 5]),
    `Expected S2 episodes [1,3,4,5], got ${JSON.stringify(s2Numbers)}`,
  );

  // 4. Batch releases appear separately from individual episodes with distinct subtypes
  const batchItems = [
    ...items1,
    makeItem(
      20,
      "[Judas] Sousou no Frieren - 01-12 [1080p][HEVC x265 10bit].mkv",
      "bbbb111111111111111111111111111111111111",
    ),
    makeItem(
      21,
      "[Ember] Sousou no Frieren - Season 1 Batch (1080p) [HEVC]",
      "bbbb222222222222222222222222222222222222",
    ),
    makeItem(
      22,
      "[ Beatrice-Raws ] Sousou no Frieren (Complete Series) [BDRip 1920x1080 HEVC FLAC]",
      "bbbb333333333333333333333333333333333333",
    ),
  ];
  const batchShows = groupItemsToMedia(batchItems, "Nyaa");
  assert(batchShows.length === 1, "Batch + episode items should group into 1 show");
  const batchCatalog = buildEpisodeCatalog(batchShows[0].sources);
  assert(
    batchCatalog.episodes.length === 2,
    "Batch releases must not fabricate individual episodes",
  );
  assert(
    batchCatalog.batches.length === 3,
    `Expected 3 batch releases, got ${batchCatalog.batches.length}`,
  );
  const batchKinds = batchCatalog.batches.map((b) => b.batchKind);
  assert(batchKinds.includes("multi-episode"), "Expected multi-episode batchKind");
  assert(batchKinds.includes("season-pack"), "Expected season-pack batchKind");
  assert(batchKinds.includes("complete-series"), "Expected complete-series batchKind");

  // 5. A show containing only batch releases remains navigable
  const batchOnlyItems = [
    makeItem(
      30,
      "[Judas] Monster (2004) (Batch 01-74) [1080p][HEVC]",
      "cccc111111111111111111111111111111111111",
    ),
  ];
  const batchOnlyShow = groupItemsToMedia(batchOnlyItems, "Nyaa");
  assert(batchOnlyShow.length === 1, "Batch-only show should create 1 MediaEntry");
  const batchOnlyCatalog = buildEpisodeCatalog(batchOnlyShow[0].sources);
  assert(batchOnlyCatalog.episodes.length === 0, "Batch-only show has 0 individual episodes");
  assert(batchOnlyCatalog.batches.length === 1, "Batch-only show has 1 navigable batch");
  assert(
    batchOnlyCatalog.batches[0].season === null,
    "Unspecified season on batch should preserve null season",
  );

  // 6. Missing or ambiguous episode identifiers do not produce fabricated episodes
  const ambiguousItems = [
    ...items1,
    makeItem(
      40,
      "[SubsPlease] Sousou no Frieren - Original Soundtrack & PV Collection [FLAC]",
      "dddd111111111111111111111111111111111111",
    ),
    makeItem(
      41,
      "[Erai-raws] Sousou no Frieren - Special Preview [1080p].mkv",
      "dddd222222222222222222222222222222222222",
    ),
  ];
  const ambShows = groupItemsToMedia(ambiguousItems, "Nyaa");
  assert(ambShows.length === 1, "Specials/OST should stay grouped under parent show");
  const ambCatalog = buildEpisodeCatalog(ambShows[0].sources);
  assert(
    ambCatalog.episodes.length === 2,
    "Ambiguous specials must not fabricate episodes",
  );
  assert(
    ambCatalog.other.length === 2,
    `Expected 2 unclassified releases, got ${ambCatalog.other.length}`,
  );

  // 7. Duplicate magnets (case-insensitive BTIH hash) are removed
  const dupItems = [
    makeItem(
      50,
      "[SubsPlease] Dandadan - 01 (1080p).mkv",
      "ABCDEF0123456789ABCDEF0123456789ABCDEF01",
      150,
    ),
    makeItem(
      51,
      "[SubsPlease] Dandadan - 01 (1080p) [Mirror].mkv",
      "abcdef0123456789abcdef0123456789abcdef01",
      420,
    ),
  ];
  assert(
    extractMagnetInfoHash(dupItems[0].magnet_link || "") ===
      extractMagnetInfoHash(dupItems[1].magnet_link || ""),
    "Normalized magnet info-hash must match case-insensitively",
  );
  const dupShow = groupItemsToMedia(dupItems, "Nyaa");
  assert(dupShow[0].sources.length === 1, "Duplicate magnet must be deduplicated in MediaEntry");
  const dupCatalog = buildEpisodeCatalog(dupShow[0].sources);
  assert(
    dupCatalog.episodes[0].sources.length === 1,
    "Duplicate magnet must be deduplicated in EpisodeEntry",
  );

  // 8 & 9. Loading additional pages and re-grouping preserves existing context & does not duplicate results
  const page1 = items1;
  const page2 = [
    ...items1, // overlapping items from previous page
    makeItem(
      60,
      "[SubsPlease] Sousou no Frieren - 03 (1080p).mkv",
      "6666666666666666666666666666666666666666",
      390,
    ),
  ];
  const mergedShow = groupItemsToMedia([...page1, ...page2], "Nyaa");
  const mergedCatalog = buildEpisodeCatalog(mergedShow[0].sources);
  assert(
    mergedCatalog.episodes.length === 3,
    "Merged pages must contain 3 unique episodes without duplicates",
  );

  // 10 & 11. Long show names & release names parse cleanly
  const longParsed = parseReleaseTitle(
    "[SubsPlease][EMBER] Maou 2099: Cyberpunk Demon Lord in Shinjuku City - 07v2 (1080p) [HEVC 10bit x265 AAC][Multi-Sub][A1B2C3D4].mkv",
  );
  assert(
    longParsed.showTitle === "Maou 2099: Cyberpunk Demon Lord in Shinjuku City",
    `Unexpected parsed long show title: ${longParsed.showTitle}`,
  );
  assert(longParsed.firstEpisode === 7, "Expected episode 7 from 07v2");

  // 12. Selecting an episode returns the correct magnet URI; torrent-only item has null magnet_link
  const torrentOnlyItem = makeItem(
    70,
    "[Anon] Sousou no Frieren - 01 (720p) [Torrent-Only].mkv",
    null,
    45,
  );
  const mixedShow = groupItemsToMedia([...items1, torrentOnlyItem], "Nyaa");
  const mixedCatalog = buildEpisodeCatalog(mixedShow[0].sources);
  const ep1Sources = mixedCatalog.episodes[0].sources;
  const magnetSources = ep1Sources.filter((s) => Boolean(s.item.magnet_link));
  const missingMagnetSources = ep1Sources.filter((s) => !s.item.magnet_link);
  assert(magnetSources.length === 2, "Expected 2 valid magnet sources");
  assert(
    magnetSources[0].item.magnet_link?.startsWith("magnet:?xt=urn:btih:"),
    "Selected episode source must provide valid magnet URI",
  );
  assert(
    missingMagnetSources.length === 1 &&
      !missingMagnetSources[0].item.magnet_link &&
      Boolean(missingMagnetSources[0].item.torrent_link?.endsWith(".torrent")),
    "Torrent-only source must keep magnet_link empty and never fabricate a magnet",
  );

  // 13. Canonical metadata adaptation + multi-provider mirror merging + official episode titles & absolute numbering
  const canonicalIdentity: MediaIdentity = {
    canonical_id: "anilist:154587",
    official_title: "Frieren: Beyond Journey's End",
    original_title: "葬送のフリーレン",
    romaji_title: "Sousou no Frieren",
    alternate_titles: ["Frieren"],
    release_year: 2023,
    media_type: "anime",
    synopsis: "An elven mage reflects on her party's journey.",
    genres: ["Adventure", "Drama", "Fantasy"],
    status: "FINISHED",
    poster_url: "https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/frieren.jpg",
    backdrop_url: "https://s4.anilist.co/file/anilistcdn/media/anime/banner/frieren.jpg",
    metadata_provider: "AniList",
    confidence: 1.0,
    seasons: [
      {
        season_number: 1,
        title: "Season 1",
        episode_count: 28,
        season_aliases: ["Sousou no Frieren"],
        episodes: [
          {
            episode_number: 1,
            season_number: 1,
            title: "The Journey's End",
            air_date: "2023-09-29",
          },
        ],
      },
      {
        season_number: 2,
        title: "Season 2",
        episode_count: 24,
        season_aliases: ["Sousou no Frieren 2nd Season"],
        episodes: [
          {
            episode_number: 1,
            season_number: 2,
            title: "A New Departure",
            air_date: "2026-01-16",
          },
        ],
      },
    ],
  };

  const baseCard = adaptMetadataToMediaEntry(canonicalIdentity);
  assert(baseCard.posterUrl === canonicalIdentity.poster_url, "Poster URL must be preserved");
  assert(baseCard.officialSeasons?.length === 2, "Official seasons must be preserved");

  const mirrorItems: ScrapedItem[] = [
    {
      ...makeItem(
        80,
        "[SubsPlease] Sousou no Frieren - 01 (1080p) [F02B964D].mkv",
        "aaaa999999999999999999999999999999999999",
        500,
      ),
      provider_name: "Nyaa",
    },
    {
      ...makeItem(
        81,
        "[SubsPlease] Sousou no Frieren - 01 (1080p) [F02B964D].mkv",
        "AAAA999999999999999999999999999999999999",
        550,
      ),
      provider_name: "SubsPlease",
    },
    {
      ...makeItem(
        82,
        "[SubsPlease] Sousou no Frieren - 29 (1080p) [A1B2C3D4].mkv",
        "bbbb999999999999999999999999999999999999",
        410,
      ),
      provider_name: "Nyaa",
    },
  ];

  const enrichedEntry = mergeSourcesIntoMediaEntry(baseCard, mirrorItems, "Nyaa");
  assert(
    enrichedEntry.sources.length === 2,
    `Expected 2 deduplicated sources after mirror merge, got ${enrichedEntry.sources.length}`,
  );
  assert(
    enrichedEntry.sources[0].providerName === "Nyaa + SubsPlease",
    `Expected merged providerName 'Nyaa + SubsPlease', got ${enrichedEntry.sources[0].providerName}`,
  );
  const officialCatalog = buildEpisodeCatalog(
    enrichedEntry.sources,
    enrichedEntry.officialSeasons,
  );
  assert(
    officialCatalog.episodes.length === 2,
    "Expected S1E1 and absolute ep 29 -> S2E1",
  );
  assert(
    officialCatalog.episodes[0].title === "The Journey's End",
    `Expected official episode title 'The Journey's End', got ${officialCatalog.episodes[0].title}`,
  );
  assert(
    officialCatalog.episodes[1].season === 2 &&
      officialCatalog.episodes[1].number === 1 &&
      officialCatalog.episodes[1].title === "A New Departure",
    "Absolute episode 29 must map to Season 2 Episode 1 with official title",
  );
}

runAllCatalogScenarioTests();
console.log("ALL_12_CATALOG_SCENARIO_TESTS_PASSED");
