import {
  AlertTriangle,
  RotateCcw,
  Search,
  SlidersHorizontal,
  Table2,
} from "lucide-react";
import { m } from "motion/react";
import { PAGE_VARIANTS } from "../lib/motion";
import type {
  MediaEntry,
  ScrapeMode,
  ScrapedItem,
  SearchHistoryEntry,
  SearchStatusKind,
} from "../lib/types";
import { NyaaLogo } from "./NyaaLogo";
import { PaginationBar } from "./PaginationBar";
import { PosterCard } from "./PosterCard";

interface SearchViewProps {
  query: string;
  mode: ScrapeMode;
  rawItems: ScrapedItem[];
  mediaItems: MediaEntry[];
  rawSourceCount: number;
  hasSearched: boolean;
  isLoading: boolean;
  isLoadingMore: boolean;
  currentPage: number;
  totalPages: number;
  hasNextPage: boolean;
  errorMessage: string | null;
  searchStatus?: SearchStatusKind;
  providerNotes?: string[];
  libraryKeys: Set<string>;
  searchHistory: SearchHistoryEntry[];
  selectedRowId: number | null;
  onSelectMedia: (media: MediaEntry) => void;
  onSelectRow: (item: ScrapedItem) => void;
  onCopyMagnet: (item: ScrapedItem) => void;
  onOpenTorrent: (item: ScrapedItem) => void;
  onToggleLibrary: (media: MediaEntry) => void;
  onQuickSearch: (query: string) => void;
  onBrowseDefault: () => void;
  onRetry: () => void;
  onOpenSettings: () => void;
  onOpenAdvancedBrowser?: () => void;
  onSearchRawFallback?: () => void;
  onSelectPage: (page: number) => void;
  onLoadMore: () => void;
}

export function SearchView({
  query,
  mode,
  rawItems,
  mediaItems,
  hasSearched,
  isLoading,
  isLoadingMore,
  currentPage,
  totalPages,
  hasNextPage,
  errorMessage,
  searchStatus = "idle",
  providerNotes = [],
  libraryKeys,
  searchHistory,
  onSelectMedia,
  onToggleLibrary,
  onQuickSearch,
  onBrowseDefault,
  onRetry,
  onOpenSettings,
  onOpenAdvancedBrowser,
  onSearchRawFallback,
  onSelectPage,
  onLoadMore,
}: SearchViewProps) {
  const uniqueHistory = searchHistory.filter((entry, idx, arr) => {
    const q = entry.query.trim().toLowerCase();
    return Boolean(q) && arr.findIndex((e) => e.query.trim().toLowerCase() === q) === idx;
  });

  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="space-y-6 pb-10"
    >
      {/* Search Context Header */}
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div className="flex items-center gap-3">
          <NyaaLogo size={34} className="shrink-0 shadow-md shadow-[#3582F7]/20" />
          <div>
            <div className="flex flex-wrap items-center gap-2">
              <h2 className="text-lg font-semibold tracking-tight text-[#F8FAFC]">
                {hasSearched && query.trim()
                  ? `Shows matching "${query.trim()}"`
                  : hasSearched
                    ? "Featured Catalog"
                    : "Discover Anime, Movies & TV Shows"}
              </h2>
              <span
                className={`rounded-full border px-2 py-0.5 text-[10.5px] font-medium ${
                  mode === "mock"
                    ? "border-amber-400/30 bg-amber-400/10 text-amber-200"
                    : "border-emerald-400/30 bg-emerald-400/10 text-emerald-200"
                }`}
              >
                {mode === "mock" ? "Offline Demo Mode" : "Live Discovery"}
              </span>
            </div>
            <p className="mt-0.5 text-xs text-[#949CAF]">
              {isLoading
                ? `Searching ${mode === "mock" ? "offline demo catalog" : "AniList, TMDB & TVMaze metadata"}...`
                : hasSearched
                  ? `${mediaItems.length} ${mediaItems.length === 1 ? "title" : "titles"} identified · Select a card to inspect seasons, episodes, and multi-provider magnet links`
                  : "Search for any anime, movie, or TV series above to inspect canonical metadata and verified magnet sources."}
            </p>
          </div>
        </div>

        {onOpenAdvancedBrowser && (
          <button
            type="button"
            onClick={onOpenAdvancedBrowser}
            className="inline-flex items-center gap-1.5 rounded-xl border border-white/10 bg-[#111522] px-3 py-1.5 text-xs font-medium text-[#AAB5CA] transition-colors hover:border-[#786BFF]/45 hover:bg-[#191E2E] hover:text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
          >
            <Table2 className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
            <span>Advanced Torrent Browser</span>
          </button>
        )}
      </div>

      {/* Recent Queries */}
      {uniqueHistory.length > 0 && (
        <div className="flex flex-wrap items-center gap-2">
          <span className="text-[11px] font-medium text-[#949CAF]">Recent:</span>
          {uniqueHistory.slice(0, 8).map((entry) => (
            <button
              key={entry.query.trim().toLowerCase()}
              type="button"
              onClick={() => onQuickSearch(entry.query)}
              className="rounded-lg border border-white/[0.08] bg-[#111522] px-2.5 py-1 text-xs text-[#F8FAFC] transition-colors hover:border-[#786BFF]/45 hover:bg-[#191E2E] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              {entry.query}
            </button>
          ))}
        </div>
      )}

      {/* Body States */}
      {isLoading && mediaItems.length === 0 ? (
        <div
          aria-busy="true"
          className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5"
        >
          {Array.from({ length: 8 }, (_, idx) => (
            <div
              key={idx}
              className="space-y-2.5 rounded-2xl border border-white/[0.06] bg-[#111522] p-2.5"
            >
              <div className="skeleton-shimmer aspect-[2/3] w-full rounded-xl" />
              <div className="skeleton-shimmer h-4 w-4/5 rounded" />
              <div className="skeleton-shimmer h-3.5 w-2/3 rounded" />
              <div className="skeleton-shimmer h-7 w-full rounded-xl" />
            </div>
          ))}
        </div>
      ) : errorMessage && mediaItems.length === 0 ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-rose-400/20 bg-[#111522] px-6 py-16 text-center">
          <div className="mb-3 flex h-11 w-11 items-center justify-center rounded-xl border border-rose-400/25 bg-rose-400/10">
            <AlertTriangle className="h-5 w-5 text-rose-400" aria-hidden="true" />
          </div>
          <h3 className="text-sm font-semibold text-[#F8FAFC]">Unable to fetch show results</h3>
          <p className="mt-1.5 max-w-lg rounded-lg border border-rose-400/20 bg-rose-500/5 px-3 py-2 font-mono text-xs text-rose-300">
            {errorMessage}
          </p>
          <div className="mt-4 flex gap-2.5">
            <button
              type="button"
              onClick={onRetry}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC] hover:bg-[#6A5CFF] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Retry</span>
            </button>
            <button
              type="button"
              onClick={onOpenSettings}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-4 text-xs font-medium text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <SlidersHorizontal className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Scraper Settings</span>
            </button>
          </div>
        </div>
      ) : hasSearched && !isLoading && mediaItems.length === 0 && rawItems.length === 0 ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522] px-6 py-16 text-center">
          <Search className="mb-3 h-7 w-7 text-[#949CAF]" aria-hidden="true" />
          <h3 className="text-sm font-semibold text-[#F8FAFC]">
            {searchStatus === "provider_unavailable"
              ? "Metadata Providers Temporarily Unavailable"
              : "No matching shows found"}
          </h3>
          <p className="mt-1 max-w-md text-xs text-[#949CAF]">
            {searchStatus === "provider_unavailable"
              ? "AniList / TVMaze / TMDB could not be reached right now. You can retry or search raw torrents directly on Nyaa."
              : "No anime, movie, or TV show matched your query. Try another title or search raw torrent releases directly."}
          </p>
          {providerNotes.length > 0 && (
            <p className="mt-2 max-w-lg rounded-lg border border-white/[0.07] bg-[#0B0D16] px-3 py-1.5 text-[11px] text-[#949CAF]">
              {providerNotes.join(" · ")}
            </p>
          )}
          <div className="mt-4 flex flex-wrap justify-center gap-2.5">
            <button
              type="button"
              onClick={onRetry}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC] hover:bg-[#6A5CFF] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Retry Search</span>
            </button>
            {onSearchRawFallback && query.trim() && (
              <button
                type="button"
                onClick={onSearchRawFallback}
                className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-4 text-xs font-medium text-[#F8FAFC] hover:border-[#38BDF8]/50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#38BDF8]"
              >
                <Table2 className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
                <span>Search Raw Torrents on Nyaa</span>
              </button>
            )}
          </div>
        </div>
      ) : !hasSearched ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522]/75 px-6 py-16 text-center">
          <NyaaLogo size={42} className="mb-3 shadow-lg shadow-[#3582F7]/20" />
          <h3 className="text-sm font-semibold text-[#F8FAFC]">Search or Browse Anime, Movies & TV</h3>
          <p className="mt-1 max-w-md text-xs text-[#949CAF]">
            Enter a title in the top bar, browse featured shows, or pick a sample title below.
          </p>
          <div className="mt-4 flex flex-wrap justify-center gap-2">
            <button
              type="button"
              onClick={onBrowseDefault}
              className="rounded-xl bg-[#3582F7] px-3.5 py-1.5 text-xs font-semibold text-white hover:bg-[#2563EB] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#38BDF8]"
            >
              Browse Featured Catalog
            </button>
            {["Frieren", "Dan Da Dan", "Your Name", "Breaking Bad", "The Office"].map((sample) => (
              <button
                key={sample}
                type="button"
                onClick={() => onQuickSearch(sample)}
                className="rounded-xl border border-white/10 bg-[#191E2E] px-3 py-1.5 text-xs text-[#F8FAFC] hover:border-[#786BFF]/50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                {sample}
              </button>
            ))}
          </div>
        </div>
      ) : (
        <div className="space-y-4">
          {searchStatus === "partial_failure" && providerNotes.length > 0 && (
            <div
              role="status"
              className="flex flex-wrap items-center justify-between gap-2 rounded-xl border border-amber-400/25 bg-amber-400/10 px-4 py-2.5 text-xs text-amber-100"
            >
              <span>Partial metadata results: {providerNotes.join(" · ")}</span>
              {onSearchRawFallback && query.trim() && (
                <button
                  type="button"
                  onClick={onSearchRawFallback}
                  className="rounded-lg border border-amber-300/30 px-2.5 py-1 font-medium text-amber-100 hover:bg-amber-400/15"
                >
                  Search Raw Torrents
                </button>
              )}
            </div>
          )}
          {errorMessage && (
            <div
              role="alert"
              className="flex flex-wrap items-center justify-between gap-2 rounded-xl border border-amber-400/25 bg-amber-400/10 px-4 py-2.5 text-xs text-amber-100"
            >
              <span>{errorMessage} (Previously loaded show results are preserved below.)</span>
              <button
                type="button"
                onClick={onRetry}
                className="rounded-lg border border-amber-300/30 px-2.5 py-1 font-medium text-amber-100 hover:bg-amber-400/15"
              >
                Retry
              </button>
            </div>
          )}

          <div className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5">
            {mediaItems.map((media, idx) => (
              <PosterCard
                key={media.key}
                media={media}
                index={idx}
                isSaved={libraryKeys.has(media.key)}
                onSelect={onSelectMedia}
                onToggleLibrary={onToggleLibrary}
              />
            ))}
          </div>

          <PaginationBar
            currentPage={currentPage}
            totalPages={totalPages}
            hasNextPage={hasNextPage}
            itemCount={rawItems.length}
            isLoading={isLoading}
            isLoadingMore={isLoadingMore}
            onSelectPage={onSelectPage}
            onLoadMore={onLoadMore}
          />
        </div>
      )}
    </m.div>
  );
}
