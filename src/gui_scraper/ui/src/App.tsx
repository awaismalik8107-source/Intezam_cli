import { LazyMotion, MotionConfig, domMax } from "motion/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ContinueWatchingView } from "./components/ContinueWatchingView";
import { DiscoverView } from "./components/DiscoverView";
import { ExtensionsView } from "./components/ExtensionsView";
import { HomeView } from "./components/HomeView";
import { LibraryView } from "./components/LibraryView";
import { MediaDetailsView } from "./components/MediaDetailsView";
import { SearchView } from "./components/SearchView";
import { SettingsView } from "./components/SettingsView";
import { Sidebar } from "./components/Sidebar";
import { ToastSystem } from "./components/ToastSystem";
import { TopBar } from "./components/TopBar";
import { TorrentInspectorDrawer } from "./components/TorrentInspectorDrawer";
import { useKeyboardShortcuts } from "./hooks/useKeyboardShortcuts";
import { usePyWebview } from "./hooks/usePyWebview";
import {
  clearWatchHistory,
  copyToClipboard,
  discoverShowSources,
  discoverShows,
  getAppLogs,
  getConfig,
  getEngineStatus,
  getLibrary,
  getSearchHistory,
  getStreamStatus,
  getWatchHistory,
  launchIntezamStream,
  openExternalUrl,
  recordWatchLaunch,
  removeLibraryItem,
  resetConfig,
  saveConfig,
  searchPaginated,
  stopStream,
  toggleLibraryItem,
  verifyUrl,
} from "./lib/bridge";
import {
  adaptMetadataToMediaEntry,
  groupItemsToMedia,
  mergeSourcesIntoMediaEntry,
  stripStatusPrefix,
} from "./lib/mediaAdapter";
import type {
  EngineStatus,
  LibraryEntry,
  MediaEntry,
  MediaIdentity,
  MediaSource,
  NavSection,
  ProviderRetrievalStatus,
  ScrapeMode,
  ScrapedItem,
  ScraperConfig,
  SearchHistoryEntry,
  SearchStatusKind,
  SourceSearchFilters,
  StreamSessionState,
  ToastMessage,
  ToastType,
  UiPreferences,
  VerifyUrlResponse,
  WatchHistoryEntry,
} from "./lib/types";

const FALLBACK_CONFIG: ScraperConfig = {
  provider_name: "Nyaa",
  base_url: "https://nyaa.si",
  search_path: "/",
  query_param: "q",
  row_selector: "table.torrent-list tbody tr",
  title_selector: 'td[colspan="2"] a[href^="/view/"]:not(.comments)',
  link_selector: 'td.text-center a[href^="magnet:?"]',
  size_selector: "td:nth-of-type(4)",
  date_selector: "td:nth-of-type(5)",
  stats_selector: "td:nth-of-type(6)",
  timeout: 10,
  user_agent:
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
};

const PREFS_STORAGE_KEY = "intezam.cinema.prefs.v1";

function loadInitialPreferences(): UiPreferences {
  try {
    const raw = localStorage.getItem(PREFS_STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<UiPreferences>;
      return {
        reducedMotion: parsed.reducedMotion ?? "system",
        ambientLighting: parsed.ambientLighting ?? true,
        strictCacheBudget: parsed.strictCacheBudget ?? false,
        defaultMode: parsed.defaultMode ?? "mock",
      };
    }
  } catch {
    // Ignore storage errors
  }
  return {
    reducedMotion: "system",
    ambientLighting: true,
    strictCacheBudget: false,
    defaultMode: "mock",
  };
}

const SECTION_TITLES: Record<NavSection, string> = {
  home: "Home",
  discover: "Discover",
  extensions: "Extensions",
  search: "Search",
  library: "Library",
  continue: "Continue Watching",
  settings: "Settings",
};

export function App() {
  const bridgeReady = usePyWebview();
  const searchInputRef = useRef<HTMLInputElement | null>(null);
  const activeSearchIdRef = useRef(0);
  const activeDetailIdRef = useRef(0);
  const detailsCacheRef = useRef(
    new Map<
      string,
      {
        entry: MediaEntry;
        expires: number;
        hasMore: boolean;
        nextPage: number;
        cursors?: Record<string, Record<string, unknown>>;
      }
    >(),
  );

  const [preferences, setPreferences] = useState<UiPreferences>(loadInitialPreferences);
  const [activeSection, setActiveSection] = useState<NavSection>("home");
  const [selectedMediaKey, setSelectedMediaKey] = useState<string | null>(null);
  const [detailedMedia, setDetailedMedia] = useState<MediaEntry | null>(null);
  const [detailsLoading, setDetailsLoading] = useState(false);
  const [detailsError, setDetailsError] = useState<string | null>(null);
  const [detailsHasMore, setDetailsHasMore] = useState(false);
  const [detailsNextPage, setDetailsNextPage] = useState(1);
  const [detailsCursors, setDetailsCursors] = useState<
    Record<string, Record<string, unknown>> | undefined
  >(undefined);
  const [inspectedRow, setInspectedRow] = useState<ScrapedItem | null>(null);

  const [mode, setMode] = useState<ScrapeMode>(() => preferences.defaultMode);
  const [query, setQuery] = useState("");
  const [items, setItems] = useState<ScrapedItem[]>([]);
  const [metadataEntries, setMetadataEntries] = useState<MediaEntry[]>([]);
  const [searchModeKind, setSearchModeKind] = useState<"metadata" | "raw">("metadata");
  const [searchStatus, setSearchStatus] = useState<SearchStatusKind>("idle");
  const [providerNotes, setProviderNotes] = useState<string[]>([]);
  const [sourceFilters, setSourceFilters] = useState<SourceSearchFilters>({
    category: "1_0",
    filter_mode: "0",
    sort_by: "seeders",
    sort_order: "desc",
  });
  const [hasSearched, setHasSearched] = useState(false);
  const [isLoading, setIsLoading] = useState(false);
  const [isLoadingMore, setIsLoadingMore] = useState(false);
  const [currentPage, setCurrentPage] = useState(1);
  const [totalPages, setTotalPages] = useState(1);
  const [hasNextPage, setHasNextPage] = useState(false);
  const [searchError, setSearchError] = useState<string | null>(null);
  const [verifyingId, setVerifyingId] = useState<number | null>(null);

  const [config, setConfig] = useState<ScraperConfig>(FALLBACK_CONFIG);
  const [isSavingConfig, setIsSavingConfig] = useState(false);
  const [settingsError, setSettingsError] = useState<string | null>(null);

  const [libraryEntries, setLibraryEntries] = useState<LibraryEntry[]>([]);
  const [discoveryCatalog, setDiscoveryCatalog] = useState<MediaEntry[]>([]);
  const [searchHistory, setSearchHistory] = useState<SearchHistoryEntry[]>([]);
  const [watchHistory, setWatchHistory] = useState<WatchHistoryEntry[]>([]);
  const [engineStatus, setEngineStatus] = useState<EngineStatus | null>(null);
  const [streamState, setStreamState] = useState<StreamSessionState | null>(null);
  const [appLogs, setAppLogs] = useState<string[]>([]);
  const [toasts, setToasts] = useState<ToastMessage[]>([]);

  const pushToast = useCallback((message: string, type: ToastType = "info") => {
    const id = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
    setToasts((prev) => [...prev.slice(-3), { id, message, type }]);
    window.setTimeout(() => {
      setToasts((prev) => prev.filter((t) => t.id !== id));
    }, 3400);
  }, []);

  const dismissToast = useCallback((id: string) => {
    setToasts((prev) => prev.filter((t) => t.id !== id));
  }, []);

  const updatePreferences = useCallback((patch: Partial<UiPreferences>) => {
    setPreferences((prev) => {
      const next = { ...prev, ...patch };
      try {
        localStorage.setItem(PREFS_STORAGE_KEY, JSON.stringify(next));
      } catch {
        // Ignore storage restrictions
      }
      return next;
    });
  }, []);

  const refreshBridgeData = useCallback(async () => {
    if (!bridgeReady) return;
    try {
      const [cfg, lib, hist, watch, eng, logs] = await Promise.all([
        getConfig(),
        getLibrary(),
        getSearchHistory(),
        getWatchHistory(),
        getEngineStatus(),
        getAppLogs(),
      ]);
      if (cfg) setConfig(cfg);
      setLibraryEntries(lib);
      setSearchHistory(hist);
      setWatchHistory(watch);
      setEngineStatus(eng);
      setAppLogs(logs);
    } catch {
      // Non-fatal startup sync errors
    }
  }, [bridgeReady]);

  useEffect(() => {
    refreshBridgeData();
  }, [refreshBridgeData]);

  // Poll real Intezam C++ process telemetry while a stream session is active
  useEffect(() => {
    if (!bridgeReady || !streamState?.active) return;
    const timer = window.setInterval(async () => {
      const next = await getStreamStatus();
      if (next) {
        setStreamState(next);
        if (!next.active) {
          const eng = await getEngineStatus();
          setEngineStatus(eng);
          if (next.stage === "error" && next.message) {
            pushToast(next.message, "error");
          }
        }
      }
    }, 650);
    return () => window.clearInterval(timer);
  }, [bridgeReady, pushToast, streamState?.active]);

  const discoveredMedia = useMemo(() => {
    if (searchModeKind === "raw") {
      return groupItemsToMedia(items, config.provider_name);
    }
    return metadataEntries;
  }, [searchModeKind, items, metadataEntries, config.provider_name]);

  const libraryMedia = useMemo<MediaEntry[]>(
    () =>
      libraryEntries.map((entry) =>
        entry.mediaSnapshot
          ? entry.mediaSnapshot
          : {
              key: entry.key,
              title: entry.title,
              year: entry.year,
              seasonEpisode: null,
              mediaType: entry.mediaType,
              sources: [],
              bestQuality: entry.bestQuality,
              verifiedCount: 0,
              magnetCount: 0,
              latestDate: "N/A",
              largestSizeLabel: "N/A",
              largestSizeBytes: -1,
              maxSeeders: null,
              providerName: entry.providerName,
            },
      ),
    [libraryEntries],
  );

  const libraryKeys = useMemo(
    () => new Set(libraryEntries.map((entry) => entry.key)),
    [libraryEntries],
  );

  const selectedMedia = useMemo(() => {
    if (!selectedMediaKey) return null;
    return (
      (detailedMedia?.key === selectedMediaKey ? detailedMedia : null) ??
      discoveredMedia.find((mEntry) => mEntry.key === selectedMediaKey) ??
      discoveryCatalog.find((mEntry) => mEntry.key === selectedMediaKey) ??
      libraryMedia.find((mEntry) => mEntry.key === selectedMediaKey) ??
      null
    );
  }, [detailedMedia, discoveredMedia, discoveryCatalog, libraryMedia, selectedMediaKey]);

  const executeSearch = useCallback(
    async (
      explicitQuery?: string,
      explicitMode?: ScrapeMode,
      navigateToSearch = false,
      page = 1,
      append = false,
      forceRaw = false,
    ) => {
      const targetQuery = (explicitQuery ?? query).trim();
      const targetMode = explicitMode ?? mode;
      const safePage = Math.max(1, page);
      const useRawMode = forceRaw || activeSection === "extensions" || safePage > 1 || append;

      if (explicitQuery !== undefined) setQuery(explicitQuery);
      if (!append) {
        setInspectedRow(null);
        setMetadataEntries([]);
        setItems([]);
        setProviderNotes([]);
      }
      if (navigateToSearch) {
        setSelectedMediaKey(null);
        setActiveSection("search");
      }

      const requestId = ++activeSearchIdRef.current;
      if (append) {
        setIsLoadingMore(true);
      } else {
        setIsLoading(true);
      }
      setHasSearched(true);
      setSearchError(null);
      setSearchModeKind(useRawMode ? "raw" : "metadata");

      try {
        // Metadata-first show discovery (no automatic raw fallback masking no_matches)
        if (!useRawMode && safePage === 1 && !append) {
          const discoverRes = await discoverShows(targetQuery, targetMode);
          if (requestId !== activeSearchIdRef.current) return;

          const statusKind: SearchStatusKind =
            discoverRes.search_status ||
            (discoverRes.shows && discoverRes.shows.length > 0
              ? targetMode === "mock"
                ? "mock"
                : "ok"
              : "no_matches");
          setSearchStatus(statusKind);
          setProviderNotes(discoverRes.provider_notes || []);
          setCurrentPage(1);
          setTotalPages(1);
          setHasNextPage(false);

          if (discoverRes.shows && discoverRes.shows.length > 0) {
            const metaShows = discoverRes.shows.map((identity: MediaIdentity) =>
              adaptMetadataToMediaEntry(identity, [], config.provider_name),
            );
            setMetadataEntries(metaShows);
            setDiscoveryCatalog((prev) => {
              const incomingKeys = new Set(metaShows.map((g) => g.key));
              return [...metaShows, ...prev.filter((g) => !incomingKeys.has(g.key))].slice(0, 36);
            });
          } else {
            setMetadataEntries([]);
          }

          // Optional history refresh must never wipe out search results on transient failure
          try {
            const hist = await getSearchHistory();
            if (requestId === activeSearchIdRef.current) {
              setSearchHistory(hist);
            }
          } catch {
            // Non-fatal history read error
          }
          return;
        }

        // Explicit raw / paginated torrent search (Extensions view or user-requested raw Nyaa fallback)
        const rawCategory =
          activeSection === "extensions" ? sourceFilters.category || "0_0" : "0_0";
        const res = await searchPaginated(targetQuery, targetMode, safePage, false, {
          ...sourceFilters,
          category: rawCategory,
        });
        if (requestId !== activeSearchIdRef.current) return;

        const incomingRows = res.items;
        let combinedRows: ScrapedItem[] = incomingRows;
        if (append) {
          setItems((prev) => {
            const offset = prev.length;
            const remapped = incomingRows.map((row, idx) => ({
              ...row,
              id: offset + idx + 1,
            }));
            combinedRows = [...prev, ...remapped];
            return combinedRows;
          });
        } else {
          setItems(incomingRows);
        }

        setSearchStatus(incomingRows.length > 0 ? "ok" : "no_matches");
        setCurrentPage(res.page || safePage);
        setTotalPages(Math.max(res.total_pages || 1, res.page || safePage));
        setHasNextPage(Boolean(res.has_next_page));

        const grouped = groupItemsToMedia(combinedRows, config.provider_name);
        if (grouped.length > 0) {
          setDiscoveryCatalog((prev) => {
            const incomingKeys = new Set(grouped.map((g) => g.key));
            return [...grouped, ...prev.filter((g) => !incomingKeys.has(g.key))].slice(0, 36);
          });
        }
        if (safePage === 1 && !append) {
          try {
            const hist = await getSearchHistory();
            if (requestId === activeSearchIdRef.current) {
              setSearchHistory(hist);
            }
          } catch {
            // Non-fatal history read error
          }
        }
      } catch (err) {
        if (requestId !== activeSearchIdRef.current) return;
        const msg = err instanceof Error ? err.message : String(err);
        if (!append) {
          setMetadataEntries([]);
          setItems([]);
          setSearchStatus("provider_unavailable");
          setSearchError(msg);
        }
        pushToast(msg, "error");
      } finally {
        if (requestId === activeSearchIdRef.current) {
          setIsLoading(false);
          setIsLoadingMore(false);
        }
      }
    },
    [activeSection, config.provider_name, mode, pushToast, query, sourceFilters],
  );

  // Titles are discovered cheaply via canonical metadata; multi-provider torrent sources
  // are fetched only when a show is opened.
  const loadTitleEpisodes = useCallback(
    async (
      media: MediaEntry,
      force = false,
      startPage = 1,
      overrideFilters?: SourceSearchFilters,
      cursorsOverride?: Record<string, Record<string, unknown>>,
    ) => {
      const defaultCategory = media.mediaKind === "anime" || !media.mediaKind ? "1_0" : "0_0";
      const effectiveFilters: SourceSearchFilters = overrideFilters ?? {
        ...sourceFilters,
        category:
          media.mediaKind && media.mediaKind !== "anime" && sourceFilters.category === "1_0"
            ? defaultCategory
            : sourceFilters.category,
      };
      const filterSig = `${effectiveFilters.category || defaultCategory}:${effectiveFilters.sort_by || "seeders"}`;
      const cacheKey = `${mode}:${filterSig}:${media.key}`;
      const cached = detailsCacheRef.current.get(cacheKey);
      if (startPage === 1 && !force && cached && cached.expires > Date.now()) {
        setDetailedMedia(cached.entry);
        setDetailsHasMore(cached.hasMore);
        setDetailsNextPage(cached.nextPage);
        setDetailsCursors(cached.cursors);
        setDetailsError(null);
        setDetailsLoading(false);
        return;
      }

      const requestId = ++activeDetailIdRef.current;
      setDetailsLoading(true);
      setDetailsError(null);
      const collected: ScrapedItem[] =
        startPage > 1 ? media.sources.map((source) => source.item) : [];
      let hasMore = false;
      let nextPage = startPage;
      let providerStatuses: ProviderRetrievalStatus[] | undefined = media.providerStatuses;
      let nextCursors: Record<string, Record<string, unknown>> | undefined = cursorsOverride;
      try {
        const identityPayload: Partial<MediaIdentity> & {
          canonical_id: string;
          official_title: string;
        } = media.metadata ?? {
          canonical_id: media.key,
          official_title: media.title,
          media_type: media.mediaKind || "anime",
          release_year: media.year ? Number(media.year) : null,
          alternate_titles: media.alternateTitles ?? [],
          genres: media.genres ?? [],
          seasons: media.officialSeasons ?? [],
        };

        const response = await discoverShowSources(
          identityPayload,
          mode,
          startPage,
          force,
          effectiveFilters,
          startPage > 1 ? (cursorsOverride ?? detailsCursors) : undefined,
        );
        if (requestId !== activeDetailIdRef.current) return;
        collected.push(...response.items);
        hasMore = response.has_next_page;
        nextPage = startPage + 1;
        providerStatuses = response.provider_statuses;
        nextCursors = response.pagination_cursors ?? undefined;

        const baseEntry = response.show
          ? adaptMetadataToMediaEntry(
              response.show,
              [],
              config.provider_name,
              providerStatuses,
              nextCursors,
            )
          : media;
        const entry = mergeSourcesIntoMediaEntry(
          {
            ...media,
            ...baseEntry,
            posterUrl: baseEntry.posterUrl || media.posterUrl,
            backdropUrl: baseEntry.backdropUrl || media.backdropUrl,
            officialSeasons:
              baseEntry.officialSeasons && baseEntry.officialSeasons.length > 0
                ? baseEntry.officialSeasons
                : media.officialSeasons,
          },
          collected,
          config.provider_name,
          providerStatuses,
          nextCursors,
        );

        if (requestId !== activeDetailIdRef.current) return;
        detailsCacheRef.current.set(cacheKey, {
          entry,
          hasMore,
          nextPage,
          cursors: nextCursors,
          expires: Date.now() + 90_000,
        });
        setDetailedMedia(entry);
        setDetailsHasMore(hasMore);
        setDetailsNextPage(nextPage);
        setDetailsCursors(nextCursors);
      } catch (err) {
        if (requestId !== activeDetailIdRef.current) return;
        setDetailsError(err instanceof Error ? err.message : String(err));
        const fallbackEntry = mergeSourcesIntoMediaEntry(
          media,
          collected,
          config.provider_name,
          providerStatuses,
          nextCursors,
        );
        setDetailedMedia((previous) =>
          collected.length > 0
            ? fallbackEntry
            : previous?.key === media.key
              ? previous
              : media,
        );
      } finally {
        if (requestId === activeDetailIdRef.current) setDetailsLoading(false);
      }
    },
    [mode, config.provider_name, sourceFilters, detailsCursors],
  );

  const handleSelectMedia = useCallback((media: MediaEntry) => {
    activeDetailIdRef.current += 1;
    setSelectedMediaKey(media.key);
    setDetailedMedia(media);
    setDetailsHasMore(false);
    setDetailsNextPage(1);
    setDetailsCursors(undefined);
    setDetailsError(null);
    void loadTitleEpisodes(media);
  }, [loadTitleEpisodes]);

  const handleModeChange = useCallback(
    (nextMode: ScrapeMode) => {
      if (nextMode === mode) return;
      activeSearchIdRef.current += 1;
      activeDetailIdRef.current += 1;
      detailsCacheRef.current.clear();
      setMode(nextMode);
      setMetadataEntries([]);
      setItems([]);
      setSearchStatus("idle");
      setProviderNotes([]);
      setDetailsCursors(undefined);
      setSelectedMediaKey(null);
      setDetailedMedia(null);
      if (query.trim()) {
        void executeSearch(query, nextMode, activeSection !== "extensions", 1, false);
      }
    },
    [mode, query, activeSection, executeSearch],
  );

  const handleToggleLibrary = useCallback(
    async (media: MediaEntry) => {
      try {
        const res = await toggleLibraryItem({
          key: media.key,
          title: media.title,
          year: media.year,
          mediaType: media.mediaType,
          bestQuality: media.bestQuality,
          providerName: media.providerName,
          mediaSnapshot: media,
        });
        if (!res.ok) {
          pushToast(res.error || "Unable to update library.", "error");
          return;
        }
        if (res.items) setLibraryEntries(res.items);
        pushToast(
          res.saved ? `Saved "${media.title}" to Library.` : `Removed "${media.title}" from Library.`,
          "success",
        );
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      }
    },
    [pushToast],
  );

  const handleRemoveLibraryKey = useCallback(
    async (key: string) => {
      try {
        const next = await removeLibraryItem(key);
        setLibraryEntries(next);
        pushToast("Removed item from Library.", "info");
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      }
    },
    [pushToast],
  );

  const handleVerifySource = useCallback(
    async (source: MediaSource) => {
      if (!source.item.resolved_link || verifyingId !== null) return;
      setVerifyingId(source.item.id);
      try {
        const res = await verifyUrl(source.item.resolved_link);
        const cleanMsg = stripStatusPrefix(res.message) || (res.is_valid ? "Verified" : "Failed");
        const decorated = res.is_valid ? `✅ ${cleanMsg}` : `❌ ${cleanMsg}`;

        setItems((prev) =>
          prev.map((row) =>
            row.id === source.item.id
              ? { ...row, is_valid: res.is_valid, verification_msg: decorated }
              : row,
          ),
        );
        setDiscoveryCatalog((prev) =>
          prev.map((entry) => ({
            ...entry,
            sources: entry.sources.map((src) =>
              src.item.id === source.item.id && src.item.resolved_link === source.item.resolved_link
                ? {
                    ...src,
                    item: { ...src.item, is_valid: res.is_valid, verification_msg: decorated },
                  }
                : src,
            ),
          })),
        );
        pushToast(
          res.is_valid ? `Verified: ${cleanMsg}` : `Validation failed: ${cleanMsg}`,
          res.is_valid ? "success" : "error",
        );
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      } finally {
        setVerifyingId(null);
      }
    },
    [pushToast, verifyingId],
  );

  const handleCopyUri = useCallback(
    async (source: MediaSource): Promise<boolean> => {
      const res = await copyToClipboard(source.item.magnet_link || source.item.resolved_link);
      if (res.ok) {
        pushToast("Link copied successfully.", "success");
        return true;
      }
      pushToast(res.error || "Failed to copy URI.", "error");
      return false;
    },
    [pushToast],
  );

  const handleCopyRawText = useCallback(
    async (value: string, label: string): Promise<boolean> => {
      if (!value) {
        pushToast(`No ${label} available to copy.`, "warning");
        return false;
      }
      const res = await copyToClipboard(value);
      if (res.ok) {
        pushToast(`${label} copied to clipboard.`, "success");
        return true;
      }
      pushToast(res.error || `Failed to copy ${label}.`, "error");
      return false;
    },
    [pushToast],
  );

  const handleVerifyTorrentHead = useCallback(
    async (url: string): Promise<VerifyUrlResponse | null> => {
      try {
        const res = await verifyUrl(url);
        const cleanMsg = stripStatusPrefix(res.message) || (res.is_valid ? "HTTP 200 OK" : "Failed");
        pushToast(
          res.is_valid ? `HEAD verified: ${cleanMsg}` : `HEAD check: ${cleanMsg}`,
          res.is_valid ? "success" : "error",
        );
        return res;
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
        return null;
      }
    },
    [pushToast],
  );

  const handleOpenRawExternal = useCallback(
    async (url: string, label: string) => {
      if (!url) return;
      try {
        const ok = await openExternalUrl(url);
        if (ok) {
          pushToast(`Opened ${label} in system handler.`, "info");
        } else {
          pushToast(`Unable to open ${label}.`, "error");
        }
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      }
    },
    [pushToast],
  );

  const handleLaunchIntezamRow = useCallback(
    async (item: ScrapedItem) => {
      const targetUri = item.magnet_link || item.resolved_link;
      if (!targetUri) return;
      try {
        const res = await launchIntezamStream(
          targetUri,
          item.title,
          preferences.strictCacheBudget,
        );
        if (!res.ok) {
          pushToast(res.error || "Unable to launch Intezam stream.", "error");
          return;
        }
        if (res.state) setStreamState(res.state);
        const [watch, eng] = await Promise.all([getWatchHistory(), getEngineStatus()]);
        setWatchHistory(watch);
        setEngineStatus(eng);
        pushToast("Launching Intezam C++ + LibVLC stream...", "info");
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      }
    },
    [preferences.strictCacheBudget, pushToast],
  );

  const handleLaunchIntezam = useCallback(
    async (source: MediaSource) => {
      try {
        const res = await launchIntezamStream(
          source.item.resolved_link,
          source.sourceTitle,
          preferences.strictCacheBudget,
        );
        if (!res.ok) {
          pushToast(res.error || "Unable to launch Intezam stream.", "error");
          return;
        }
        if (res.state) setStreamState(res.state);
        const [watch, eng] = await Promise.all([getWatchHistory(), getEngineStatus()]);
        setWatchHistory(watch);
        setEngineStatus(eng);
        pushToast("Launching Intezam C++ + LibVLC stream...", "info");
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      }
    },
    [preferences.strictCacheBudget, pushToast],
  );

  const handleStopIntezam = useCallback(async () => {
    const next = await stopStream();
    if (next) setStreamState(next);
    const eng = await getEngineStatus();
    setEngineStatus(eng);
    pushToast("Stopped active stream session.", "info");
  }, [pushToast]);

  const handleOpenExternal = useCallback(
    async (source: MediaSource) => {
      try {
        const ok = await openExternalUrl(source.item.resolved_link);
        if (ok) {
          const updated = await recordWatchLaunch({
            key: source.item.resolved_link,
            title: source.sourceTitle,
            resolved_link: source.item.resolved_link,
            engine: "external",
          });
          setWatchHistory(updated);
          pushToast("Opened URI in default OS handler.", "info");
        } else {
          pushToast("Unable to open external URI.", "error");
        }
      } catch (err) {
        pushToast(err instanceof Error ? err.message : String(err), "error");
      }
    },
    [pushToast],
  );

  const handleLaunchWatchEntry = useCallback(
    async (entry: WatchHistoryEntry) => {
      if (entry.engine === "intezam-cpp" && engineStatus?.intezam_binary_available) {
        const res = await launchIntezamStream(
          entry.resolved_link,
          entry.title,
          preferences.strictCacheBudget,
        );
        if (res.ok) {
          if (res.state) setStreamState(res.state);
          pushToast("Re-launching Intezam stream...", "info");
          return;
        }
      }
      const ok = await openExternalUrl(entry.resolved_link);
      pushToast(
        ok ? "Opened URI in default OS handler." : "Unable to open external URI.",
        ok ? "info" : "error",
      );
    },
    [engineStatus?.intezam_binary_available, preferences.strictCacheBudget, pushToast],
  );

  const handleSaveConfig = useCallback(
    async (draft: ScraperConfig) => {
      setIsSavingConfig(true);
      setSettingsError(null);
      try {
        const res = await saveConfig(draft);
        if (!res.ok) {
          setSettingsError(res.error || "Failed to save configuration.");
          return;
        }
        if (res.config) setConfig(res.config);
        pushToast("Settings saved successfully.", "success");
      } catch (err) {
        setSettingsError(err instanceof Error ? err.message : String(err));
      } finally {
        setIsSavingConfig(false);
      }
    },
    [pushToast],
  );

  const handleResetConfig = useCallback(async (): Promise<ScraperConfig | null> => {
    setSettingsError(null);
    try {
      const res = await resetConfig();
      if (res.ok && res.config) {
        setConfig(res.config);
        pushToast("Restored default scraper configuration.", "info");
        return res.config;
      }
      return null;
    } catch {
      setConfig(FALLBACK_CONFIG);
      return FALLBACK_CONFIG;
    }
  }, [pushToast]);

  useKeyboardShortcuts({
    hasDetailsOpen: selectedMedia !== null,
    onFocusSearch: () => {
      searchInputRef.current?.focus();
      searchInputRef.current?.select();
    },
    onOpenSettings: () => {
      setSelectedMediaKey(null);
      setActiveSection("settings");
    },
    onCloseDetails: () => setSelectedMediaKey(null),
  });

  const motionPolicy =
    preferences.reducedMotion === "reduce"
      ? "always"
      : preferences.reducedMotion === "full"
        ? "never"
        : "user";

  const topBarTitle = selectedMedia ? selectedMedia.title : SECTION_TITLES[activeSection];

  return (
    <MotionConfig reducedMotion={motionPolicy}>
      <LazyMotion features={domMax} strict>
        <ToastSystem toasts={toasts} onDismiss={dismissToast} />

        <div className="flex h-screen w-screen overflow-hidden bg-[#090B12] text-[#F8FAFC]">
          <Sidebar
            activeSection={activeSection}
            onSelectSection={(sec) => {
              setSelectedMediaKey(null);
              setActiveSection(sec);
            }}
            libraryCount={libraryEntries.length}
            watchCount={watchHistory.length}
            engineStatus={engineStatus}
          />

          <div className="flex min-w-0 flex-1 flex-col overflow-hidden">
            <TopBar
              pageTitle={topBarTitle}
              canGoBack={selectedMedia !== null}
              onGoBack={() => setSelectedMediaKey(null)}
              searchInputRef={searchInputRef}
              query={query}
              mode={mode}
              isLoading={isLoading}
              bridgeReady={bridgeReady}
              onQueryChange={setQuery}
              onModeChange={handleModeChange}
              onSearch={() =>
                executeSearch(undefined, undefined, activeSection !== "extensions")
              }
              onOpenSettings={() => {
                setSelectedMediaKey(null);
                setActiveSection("settings");
              }}
            />

            <main className="min-h-0 flex-1 overflow-y-auto px-5 pt-5">
              {selectedMedia ? (
                <MediaDetailsView
                  key={`details-${selectedMedia.key}`}
                  media={selectedMedia}
                  isSaved={libraryKeys.has(selectedMedia.key)}
                  engineStatus={engineStatus}
                  streamState={streamState}
                  verifyingId={verifyingId}
                  strictCacheBudget={preferences.strictCacheBudget}
                  onBack={() => setSelectedMediaKey(null)}
                  onToggleLibrary={handleToggleLibrary}
                  onVerifySource={handleVerifySource}
                  onCopyUri={handleCopyUri}
                  onLaunchIntezam={handleLaunchIntezam}
                  onStopIntezam={handleStopIntezam}
                  onOpenExternal={handleOpenExternal}
                  loadingEpisodes={detailsLoading}
                  episodeError={detailsError}
                  moreEpisodesAvailable={detailsHasMore}
                  onLoadEpisodes={() => void loadTitleEpisodes(selectedMedia, true, 1)}
                  onLoadMoreEpisodes={() =>
                    void loadTitleEpisodes(
                      selectedMedia,
                      true,
                      detailsNextPage,
                      undefined,
                      detailsCursors,
                    )
                  }
                  sourceFilters={sourceFilters}
                  onChangeSourceFilters={(nextFilters) => {
                    setSourceFilters(nextFilters);
                    setDetailsCursors(undefined);
                    void loadTitleEpisodes(selectedMedia, true, 1, nextFilters);
                  }}
                />
              ) : activeSection === "home" ? (
                <HomeView
                  key="view-home"
                  discoveredMedia={discoveryCatalog}
                  libraryMedia={libraryMedia}
                  libraryKeys={libraryKeys}
                  searchHistory={searchHistory}
                  watchHistory={watchHistory}
                  onSelectMedia={handleSelectMedia}
                  onToggleLibrary={handleToggleLibrary}
                  onQuickSearch={(q) => executeSearch(q, undefined, true)}
                  onNavigate={(sec) => setActiveSection(sec)}
                  onLaunchWatchEntry={handleLaunchWatchEntry}
                />
              ) : activeSection === "discover" ? (
                <DiscoverView
                  key="view-discover"
                  mediaItems={discoveryCatalog}
                  libraryKeys={libraryKeys}
                  isLoading={isLoading}
                  onSelectMedia={handleSelectMedia}
                  onToggleLibrary={handleToggleLibrary}
                  onQuickSearch={(q) => executeSearch(q, undefined, false)}
                />
              ) : activeSection === "extensions" ? (
                <ExtensionsView
                  key="view-extensions"
                  config={config}
                  mode={mode}
                  items={items}
                  isLoading={isLoading}
                  isLoadingMore={isLoadingMore}
                  currentPage={currentPage}
                  totalPages={totalPages}
                  hasNextPage={hasNextPage}
                  errorMessage={searchError}
                  selectedRowId={inspectedRow?.id ?? null}
                  onBrowseDefault={() => executeSearch("", undefined, false, 1, false)}
                  onQuickSearch={(q) => executeSearch(q, undefined, false, 1, false)}
                  onSelectPage={(nextPage) =>
                    executeSearch(undefined, undefined, false, nextPage, false)
                  }
                  onLoadMore={() =>
                    executeSearch(undefined, undefined, false, currentPage + 1, true)
                  }
                  onSelectRow={setInspectedRow}
                  onCopyMagnet={(row) =>
                    handleCopyRawText(row.magnet_link || "", "Magnet URI")
                  }
                  onOpenTorrent={(row) =>
                    handleOpenRawExternal(row.torrent_link || "", ".torrent download")
                  }
                />
              ) : activeSection === "search" ? (
                <SearchView
                  key="view-search"
                  query={query}
                  mode={mode}
                  rawItems={items}
                  mediaItems={discoveredMedia}
                  rawSourceCount={items.length}
                  hasSearched={hasSearched}
                  isLoading={isLoading}
                  isLoadingMore={isLoadingMore}
                  currentPage={currentPage}
                  totalPages={totalPages}
                  hasNextPage={hasNextPage}
                  errorMessage={searchError}
                  searchStatus={searchStatus}
                  providerNotes={providerNotes}
                  libraryKeys={libraryKeys}
                  searchHistory={searchHistory}
                  selectedRowId={inspectedRow?.id ?? null}
                  onSelectMedia={handleSelectMedia}
                  onSelectRow={setInspectedRow}
                  onCopyMagnet={(row) =>
                    handleCopyRawText(row.magnet_link || "", "Magnet URI")
                  }
                  onOpenTorrent={(row) =>
                    handleOpenRawExternal(row.torrent_link || "", ".torrent download")
                  }
                  onToggleLibrary={handleToggleLibrary}
                  onQuickSearch={(q) => executeSearch(q, undefined, false, 1, false)}
                  onBrowseDefault={() => executeSearch("", undefined, false, 1, false)}
                  onRetry={() =>
                    executeSearch(
                      undefined,
                      undefined,
                      false,
                      currentPage,
                      false,
                      searchModeKind === "raw",
                    )
                  }
                  onSelectPage={(nextPage) =>
                    executeSearch(undefined, undefined, false, nextPage, false, true)
                  }
                  onLoadMore={() =>
                    executeSearch(undefined, undefined, false, currentPage + 1, true, true)
                  }
                  onOpenSettings={() => setActiveSection("settings")}
                  onOpenAdvancedBrowser={() => {
                    setSelectedMediaKey(null);
                    setActiveSection("extensions");
                  }}
                  onSearchRawFallback={() =>
                    executeSearch(query, undefined, false, 1, false, true)
                  }
                />
              ) : activeSection === "library" ? (
                <LibraryView
                  key="view-library"
                  libraryEntries={libraryEntries}
                  libraryKeys={libraryKeys}
                  onSelectMedia={handleSelectMedia}
                  onToggleLibrary={handleToggleLibrary}
                  onRemoveKey={handleRemoveLibraryKey}
                  onQuickSearch={(q) => executeSearch(q, undefined, true)}
                />
              ) : activeSection === "continue" ? (
                <ContinueWatchingView
                  key="view-continue"
                  watchHistory={watchHistory}
                  onLaunchEntry={handleLaunchWatchEntry}
                  onClearHistory={async () => {
                    const next = await clearWatchHistory();
                    setWatchHistory(next);
                    pushToast("Playback history cleared.", "info");
                  }}
                  onExplore={() => setActiveSection("discover")}
                />
              ) : (
                <SettingsView
                  key="view-settings"
                  config={config}
                  preferences={preferences}
                  engineStatus={engineStatus}
                  appLogs={appLogs}
                  isSaving={isSavingConfig}
                  errorMessage={settingsError}
                  onUpdatePreferences={updatePreferences}
                  onSaveConfig={handleSaveConfig}
                  onResetConfig={handleResetConfig}
                  onRefreshDiagnostics={refreshBridgeData}
                />
              )}
            </main>
          </div>
        </div>

        <TorrentInspectorDrawer
          item={inspectedRow}
          engineStatus={engineStatus}
          onClose={() => setInspectedRow(null)}
          onCopyText={handleCopyRawText}
          onVerifyTorrentHead={handleVerifyTorrentHead}
          onOpenExternal={handleOpenRawExternal}
          onLaunchIntezam={handleLaunchIntezamRow}
        />
      </LazyMotion>
    </MotionConfig>
  );
}
export default App;
