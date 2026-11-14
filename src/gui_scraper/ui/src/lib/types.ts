export type ScrapeMode = "mock" | "live";

export type NavSection =
  | "home"
  | "discover"
  | "extensions"
  | "search"
  | "library"
  | "continue"
  | "settings";

export type QualityTier = "4K" | "1080p" | "720p" | "Other";

export type SourceSortKey = "quality" | "size" | "peers" | "verified";

export interface ScrapedItem {
  id: number;
  title: string;
  size: string;
  date: string;
  stats: string;
  raw_link: string;
  resolved_link: string;
  is_valid: boolean;
  verification_msg: string;
  view_url?: string;
  magnet_link?: string;
  torrent_link?: string;
  relative_date?: string;
  seeders?: number;
  leechers?: number;
  downloads?: number;
}

export interface MediaSource {
  item: ScrapedItem;
  sourceTitle: string;
  qualityTier: QualityTier;
  inferredResolution: string | null;
  inferredCodec: string | null;
  inferredContainer: string | null;
  sizeBytes: number;
  seeders: number | null;
  leechers: number | null;
  isMagnet: boolean;
  infoHash: string | null;
  hostname: string | null;
  providerName: string;
}

export interface MediaEntry {
  key: string;
  title: string;
  year: string | null;
  seasonEpisode: string | null;
  mediaType: "series" | "release";
  sources: MediaSource[];
  bestQuality: QualityTier;
  verifiedCount: number;
  magnetCount: number;
  latestDate: string;
  largestSizeLabel: string;
  largestSizeBytes: number;
  maxSeeders: number | null;
  providerName: string;
}

export interface LibraryEntry {
  key: string;
  title: string;
  year: string | null;
  mediaType: "series" | "release";
  bestQuality: QualityTier;
  providerName: string;
  saved_at?: string;
  mediaSnapshot?: MediaEntry;
}

export interface SearchHistoryEntry {
  query: string;
  mode: ScrapeMode;
  result_count: number;
  searched_at: string;
}

export interface WatchHistoryEntry {
  key: string;
  title: string;
  resolved_link: string;
  engine: string;
  launched_at: string;
}

export interface EngineStatus {
  intezam_binary_available: boolean;
  intezam_binary_path: string;
  vlc_available: boolean;
  mpv_available: boolean;
  rolling_cache_policy: string;
  stream_active: boolean;
}

export interface StreamSessionState {
  active: boolean;
  stage:
    | "idle"
    | "connecting"
    | "metadata"
    | "selecting"
    | "buffering"
    | "playing"
    | "completed"
    | "stopped"
    | "error";
  message: string;
  url: string;
  torrent_name: string;
  selected_video: string;
  video_size: string;
  detected_videos: number;
  strict_cache: boolean;
  returncode: number | null;
  lines: string[];
}

export interface ScraperConfig {
  provider_name: string;
  base_url: string;
  search_path: string;
  query_param: string;
  row_selector: string;
  title_selector: string;
  link_selector: string;
  size_selector: string;
  date_selector: string;
  stats_selector: string;
  timeout: number | string;
  user_agent: string;
}

export interface VerifyUrlResponse {
  url: string;
  is_valid: boolean;
  status: "valid" | "invalid";
  message: string;
}

export interface SaveConfigResponse {
  ok: boolean;
  error?: string;
  config?: ScraperConfig;
}

export interface CopyClipboardResponse {
  ok: boolean;
  error?: string;
}

export type ToastType = "success" | "error" | "warning" | "info";

export interface ToastMessage {
  id: string;
  message: string;
  type: ToastType;
}

export interface UiPreferences {
  reducedMotion: "system" | "reduce" | "full";
  ambientLighting: boolean;
  strictCacheBudget: boolean;
  defaultMode: ScrapeMode;
}

export interface PaginatedSearchResponse {
  items: ScrapedItem[];
  page: number;
  has_next_page: boolean;
  total_pages: number;
  request_url?: string;
}

export interface PyWebViewApi {
  search: (query: string, mode?: ScrapeMode, page?: number) => Promise<ScrapedItem[]>;
  search_paginated?: (
    query: string,
    mode?: ScrapeMode,
    page?: number,
  ) => Promise<PaginatedSearchResponse>;
  verify_url: (url: string) => Promise<VerifyUrlResponse>;
  get_config: () => Promise<ScraperConfig>;
  save_config: (newConfig: Partial<ScraperConfig>) => Promise<SaveConfigResponse>;
  reset_config?: () => Promise<SaveConfigResponse>;
  open_external: (url: string) => Promise<boolean>;
  copy_to_clipboard: (text: string) => Promise<CopyClipboardResponse>;
  get_library?: () => Promise<LibraryEntry[]>;
  toggle_library_item?: (
    item: LibraryEntry,
  ) => Promise<{ ok: boolean; saved?: boolean; items?: LibraryEntry[]; error?: string }>;
  remove_library_item?: (key: string) => Promise<{ ok: boolean; items?: LibraryEntry[] }>;
  get_search_history?: () => Promise<SearchHistoryEntry[]>;
  clear_search_history?: () => Promise<SearchHistoryEntry[]>;
  get_watch_history?: () => Promise<WatchHistoryEntry[]>;
  record_watch_launch?: (entry: Partial<WatchHistoryEntry>) => Promise<WatchHistoryEntry[]>;
  clear_watch_history?: () => Promise<WatchHistoryEntry[]>;
  get_engine_status?: () => Promise<EngineStatus>;
  launch_intezam_stream?: (
    url: string,
    title?: string,
    strictCache?: boolean,
    timeoutSeconds?: number,
  ) => Promise<{ ok: boolean; error?: string; state?: StreamSessionState }>;
  get_stream_status?: () => Promise<StreamSessionState>;
  stop_stream?: () => Promise<StreamSessionState>;
  get_app_logs?: () => Promise<string[]>;
}

declare global {
  interface Window {
    pywebview?: {
      api?: PyWebViewApi;
    };
  }
}
