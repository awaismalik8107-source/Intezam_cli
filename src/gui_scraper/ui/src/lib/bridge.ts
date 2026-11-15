import type {
  CopyClipboardResponse,
  DiscoverShowSourcesResponse,
  DiscoverShowsResponse,
  EngineStatus,
  LibraryEntry,
  MediaIdentity,
  PaginatedSearchResponse,
  PyWebViewApi,
  SaveConfigResponse,
  ScrapeMode,
  ScrapedItem,
  ScraperConfig,
  SearchHistoryEntry,
  SourceSearchFilters,
  StreamSessionState,
  TorrentManifestInspectionResult,
  VerifyUrlResponse,
  WatchHistoryEntry,
} from "./types";

function getApi(): PyWebViewApi {
  const api = window.pywebview?.api;
  if (!api) {
    throw new Error("Desktop bridge is initializing. Please wait a moment and retry.");
  }
  return api;
}

export function isBridgeReady(): boolean {
  return Boolean(window.pywebview?.api);
}

export async function searchItems(
  query: string,
  mode: ScrapeMode,
  page = 1,
): Promise<ScrapedItem[]> {
  const rows = await getApi().search(query, mode, page);
  if (!Array.isArray(rows)) {
    throw new Error("Unexpected response format from scraper bridge.");
  }
  return rows;
}

export async function searchPaginated(
  query: string,
  mode: ScrapeMode,
  page = 1,
  forceRefresh = false,
  filters?: SourceSearchFilters,
): Promise<PaginatedSearchResponse> {
  const api = getApi();
  if (typeof api.search_paginated === "function") {
    const res = await api.search_paginated(query, mode, page, forceRefresh, filters);
    if (res && Array.isArray(res.items)) {
      return res;
    }
  }
  const items = await searchItems(query, mode, page);
  return {
    items,
    page,
    has_next_page: false,
    total_pages: page,
  };
}

export async function discoverShows(
  query: string,
  mode: ScrapeMode,
): Promise<DiscoverShowsResponse> {
  const api = getApi();
  if (typeof api.discover_shows === "function") {
    const res = await api.discover_shows(query, mode);
    if (res && Array.isArray(res.shows)) {
      return res;
    }
  }
  return {
    shows: [],
    query,
    mode,
  };
}

export async function discoverShowSources(
  show: Partial<MediaIdentity> & { canonical_id: string; official_title: string },
  mode: ScrapeMode,
  page = 1,
  forceRefresh = false,
  filters?: SourceSearchFilters,
  cursors?: Record<string, Record<string, unknown>>,
): Promise<DiscoverShowSourcesResponse> {
  const api = getApi();
  if (typeof api.discover_show_sources === "function") {
    const res = await api.discover_show_sources(show, mode, page, forceRefresh, filters, cursors);
    if (res && Array.isArray(res.items)) {
      return res;
    }
  }
  const fallback = await searchPaginated(show.official_title, mode, page, forceRefresh, filters);
  return {
    canonical_id: show.canonical_id,
    items: fallback.items,
    page: fallback.page,
    has_next_page: fallback.has_next_page,
    total_pages: fallback.total_pages,
    provider_statuses: fallback.provider_statuses ?? [],
  };
}

export async function inspectTorrentManifest(
  torrentUrl = "",
  magnetUri = "",
  infoHash = "",
  forceRefresh = false,
): Promise<TorrentManifestInspectionResult> {
  const api = window.pywebview?.api;
  if (!api?.inspect_torrent_manifest) {
    return {
      status: "unavailable",
      info_hash: infoHash,
      source_kind: torrentUrl ? "torrent_url" : "magnet_only",
      cached: false,
      torrent_name: "",
      total_files: 0,
      total_size_bytes: 0,
      total_size_label: "0 B",
      video_count: 0,
      subtitle_count: 0,
      auxiliary_count: 0,
      error: null,
      inspection_note: "Metadata inspection bridge is unavailable in this runtime.",
      files: [],
    };
  }
  return api.inspect_torrent_manifest(torrentUrl, magnetUri, infoHash, forceRefresh);
}

export async function verifyUrl(url: string): Promise<VerifyUrlResponse> {
  return getApi().verify_url(url);
}

export async function getConfig(): Promise<ScraperConfig> {
  return getApi().get_config();
}

export async function saveConfig(newConfig: Partial<ScraperConfig>): Promise<SaveConfigResponse> {
  return getApi().save_config(newConfig);
}

export async function resetConfig(): Promise<SaveConfigResponse> {
  const api = getApi();
  if (typeof api.reset_config === "function") {
    return api.reset_config();
  }
  throw new Error("Reset configuration is unavailable on this bridge.");
}

export async function openExternalUrl(url: string): Promise<boolean> {
  return Boolean(await getApi().open_external(url));
}

export async function copyToClipboard(text: string): Promise<CopyClipboardResponse> {
  if (!text) return { ok: false, error: "Nothing to copy." };

  const api = window.pywebview?.api;
  if (api?.copy_to_clipboard) {
    try {
      const res = await api.copy_to_clipboard(text);
      if (res?.ok) return { ok: true };
    } catch {
      // Fallback to browser clipboard
    }
  }

  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return { ok: true };
    } catch (err) {
      return { ok: false, error: err instanceof Error ? err.message : String(err) };
    }
  }

  return { ok: false, error: "No system clipboard mechanism available." };
}

export async function getLibrary(): Promise<LibraryEntry[]> {
  const api = window.pywebview?.api;
  if (!api?.get_library) return [];
  return api.get_library();
}

export async function toggleLibraryItem(
  item: LibraryEntry,
): Promise<{ ok: boolean; saved?: boolean; items?: LibraryEntry[]; error?: string }> {
  const api = getApi();
  if (!api.toggle_library_item) {
    return { ok: false, error: "Library persistence is unavailable." };
  }
  return api.toggle_library_item(item);
}

export async function removeLibraryItem(key: string): Promise<LibraryEntry[]> {
  const api = getApi();
  if (!api.remove_library_item) return [];
  const res = await api.remove_library_item(key);
  return res.items ?? [];
}

export async function getSearchHistory(): Promise<SearchHistoryEntry[]> {
  const api = window.pywebview?.api;
  if (!api?.get_search_history) return [];
  return api.get_search_history();
}

export async function clearSearchHistory(): Promise<SearchHistoryEntry[]> {
  const api = window.pywebview?.api;
  if (!api?.clear_search_history) return [];
  return api.clear_search_history();
}

export async function getWatchHistory(): Promise<WatchHistoryEntry[]> {
  const api = window.pywebview?.api;
  if (!api?.get_watch_history) return [];
  return api.get_watch_history();
}

export async function recordWatchLaunch(
  entry: Partial<WatchHistoryEntry>,
): Promise<WatchHistoryEntry[]> {
  const api = window.pywebview?.api;
  if (!api?.record_watch_launch) return [];
  return api.record_watch_launch(entry);
}

export async function clearWatchHistory(): Promise<WatchHistoryEntry[]> {
  const api = window.pywebview?.api;
  if (!api?.clear_watch_history) return [];
  return api.clear_watch_history();
}

export async function getEngineStatus(): Promise<EngineStatus | null> {
  const api = window.pywebview?.api;
  if (!api?.get_engine_status) return null;
  return api.get_engine_status();
}

export async function launchIntezamStream(
  url: string,
  title: string,
  strictCache: boolean,
): Promise<{ ok: boolean; error?: string; state?: StreamSessionState }> {
  const api = getApi();
  if (!api.launch_intezam_stream) {
    return { ok: false, error: "Intezam stream handoff is unavailable." };
  }
  return api.launch_intezam_stream(url, title, strictCache, 120);
}

export async function getStreamStatus(): Promise<StreamSessionState | null> {
  const api = window.pywebview?.api;
  if (!api?.get_stream_status) return null;
  return api.get_stream_status();
}

export async function stopStream(): Promise<StreamSessionState | null> {
  const api = window.pywebview?.api;
  if (!api?.stop_stream) return null;
  return api.stop_stream();
}

export async function getAppLogs(): Promise<string[]> {
  const api = window.pywebview?.api;
  if (!api?.get_app_logs) return [];
  return api.get_app_logs();
}
