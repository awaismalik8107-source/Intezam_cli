import {
  AlertTriangle,
  LayoutGrid,
  RotateCcw,
  Search,
  SlidersHorizontal,
  Table2,
} from "lucide-react";
import { LayoutGroup, m } from "motion/react";
import { useState } from "react";
import { PILL_SPRING, PAGE_VARIANTS } from "../lib/motion";
import type { MediaEntry, ScrapeMode, ScrapedItem, SearchHistoryEntry } from "../lib/types";
import { NyaaLogo } from "./NyaaLogo";
import { PaginationBar } from "./PaginationBar";
import { PosterCard } from "./PosterCard";
import { TorrentDataGrid } from "./TorrentDataGrid";

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
  onSelectPage: (page: number) => void;
  onLoadMore: () => void;
}

export function SearchView({
  query,
  mode,
  rawItems,
  mediaItems,
  rawSourceCount,
  hasSearched,
  isLoading,
  isLoadingMore,
  currentPage,
  totalPages,
  hasNextPage,
  errorMessage,
  libraryKeys,
  searchHistory,
  selectedRowId,
  onSelectMedia,
  onSelectRow,
  onCopyMagnet,
  onOpenTorrent,
  onToggleLibrary,
  onQuickSearch,
  onBrowseDefault,
  onRetry,
  onOpenSettings,
  onSelectPage,
  onLoadMore,
}: SearchViewProps) {
  const [viewMode, setViewMode] = useState<"grid" | "cards">("grid");

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
            <h2 className="text-lg font-semibold tracking-tight text-[#F8FAFC]">
              {hasSearched && query.trim()
                ? `Results for "${query.trim()}"`
                : hasSearched
                  ? "Nyaa Default Browse (c=1_2 · Leechers)"
                  : "Search Nyaa Releases"}
            </h2>
            <p className="mt-0.5 text-xs text-[#949CAF]">
              {isLoading
                ? `Fetching ${mode === "mock" ? "sample Nyaa dataset" : "https://nyaa.si"}...`
                : hasSearched
                  ? `${rawSourceCount} row(s) extracted · ${mediaItems.length} title group(s) · ${mode === "mock" ? "Mock Data" : "Live Scraper"}`
                  : "Enter a query in the top bar or browse Nyaa Category 1_2 directly."}
            </p>
          </div>
        </div>

        {/* Data Grid vs Poster Cards Switcher */}
        <LayoutGroup id="search-layout-toggle">
          <div className="flex items-center rounded-xl border border-white/[0.08] bg-[#111522] p-1">
            {(
              [
                { id: "grid", label: "Torrent Grid", icon: Table2 },
                { id: "cards", label: "Poster Cards", icon: LayoutGrid },
              ] as const
            ).map((opt) => {
              const Icon = opt.icon;
              const active = viewMode === opt.id;
              return (
                <button
                  key={opt.id}
                  type="button"
                  onClick={() => setViewMode(opt.id)}
                  className={`relative inline-flex items-center gap-1.5 rounded-lg px-3 py-1.5 text-xs font-medium transition-colors ${
                    active ? "text-[#F8FAFC]" : "text-[#949CAF] hover:text-[#F8FAFC]"
                  }`}
                >
                  {active && (
                    <m.span
                      layoutId="search-view-pill"
                      transition={PILL_SPRING}
                      aria-hidden="true"
                      className="absolute inset-0 rounded-lg border border-[#786BFF]/35 bg-[#191E2E]"
                    />
                  )}
                  <Icon className="relative z-10 h-3.5 w-3.5" aria-hidden="true" />
                  <span className="relative z-10">{opt.label}</span>
                </button>
              );
            })}
          </div>
        </LayoutGroup>
      </div>

      {/* Recent Queries */}
      {searchHistory.length > 0 && (
        <div className="flex flex-wrap items-center gap-2">
          <span className="text-[11px] font-medium text-[#949CAF]">Recent:</span>
          {searchHistory.slice(0, 8).map((entry) => (
            <button
              key={`${entry.query}-${entry.mode}`}
              type="button"
              onClick={() => onQuickSearch(entry.query)}
              className="rounded-lg border border-white/[0.08] bg-[#111522] px-2.5 py-1 text-xs text-[#F8FAFC] transition-colors hover:border-[#786BFF]/45 hover:bg-[#191E2E]"
            >
              {entry.query}
            </button>
          ))}
        </div>
      )}

      {/* Body States */}
      {isLoading && viewMode === "cards" ? (
        <div
          aria-busy="true"
          className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 xl:grid-cols-6"
        >
          {Array.from({ length: 8 }, (_, idx) => (
            <div key={idx} className="space-y-2">
              <div className="skeleton-shimmer aspect-[2/3] w-full rounded-xl" />
              <div className="skeleton-shimmer h-3.5 w-3/4 rounded" />
              <div className="skeleton-shimmer h-3 w-1/2 rounded" />
            </div>
          ))}
        </div>
      ) : errorMessage ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-rose-400/20 bg-[#111522] px-6 py-16 text-center">
          <div className="mb-3 flex h-11 w-11 items-center justify-center rounded-xl border border-rose-400/25 bg-rose-400/10">
            <AlertTriangle className="h-5 w-5 text-rose-400" aria-hidden="true" />
          </div>
          <h3 className="text-sm font-semibold text-[#F8FAFC]">Unable to fetch sources</h3>
          <p className="mt-1.5 max-w-lg rounded-lg border border-rose-400/20 bg-rose-500/5 px-3 py-2 font-mono text-xs text-rose-300">
            {errorMessage}
          </p>
          <div className="mt-4 flex gap-2.5">
            <button
              type="button"
              onClick={onRetry}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC] hover:bg-[#6A5CFF]"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Retry</span>
            </button>
            <button
              type="button"
              onClick={onOpenSettings}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-4 text-xs font-medium text-[#F8FAFC]"
            >
              <SlidersHorizontal className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Scraper Settings</span>
            </button>
          </div>
        </div>
      ) : hasSearched && !isLoading && rawItems.length === 0 ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522] px-6 py-16 text-center">
          <Search className="mb-3 h-7 w-7 text-[#949CAF]" aria-hidden="true" />
          <h3 className="text-sm font-semibold text-[#F8FAFC]">No streams found</h3>
          <p className="mt-1 max-w-md text-xs text-[#949CAF]">
            No rows matched with the current provider and CSS selectors. Try another term or review
            your selector rules in Settings.
          </p>
        </div>
      ) : !hasSearched ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522]/75 px-6 py-16 text-center">
          <NyaaLogo size={42} className="mb-3 shadow-lg shadow-[#3582F7]/20" />
          <h3 className="text-sm font-semibold text-[#F8FAFC]">Search or Browse Nyaa</h3>
          <p className="mt-1 max-w-md text-xs text-[#949CAF]">
            Enter a title above, browse Category 1_2 directly, or try a sample query.
          </p>
          <div className="mt-4 flex flex-wrap justify-center gap-2">
            <button
              type="button"
              onClick={onBrowseDefault}
              className="rounded-xl bg-[#3582F7] px-3.5 py-1.5 text-xs font-semibold text-white hover:bg-[#2563EB]"
            >
              Browse Nyaa (c=1_2)
            </button>
            {["Frieren 1080p", "Dandadan", "Dungeon Meshi"].map((sample) => (
              <button
                key={sample}
                type="button"
                onClick={() => onQuickSearch(sample)}
                className="rounded-xl border border-white/10 bg-[#191E2E] px-3 py-1.5 text-xs text-[#F8FAFC] hover:border-[#786BFF]/50"
              >
                {sample}
              </button>
            ))}
          </div>
        </div>
      ) : viewMode === "grid" ? (
        <div className="space-y-4">
          <TorrentDataGrid
            items={rawItems}
            isLoading={isLoading}
            selectedId={selectedRowId}
            onSelectRow={onSelectRow}
            onCopyMagnet={onCopyMagnet}
            onOpenTorrent={onOpenTorrent}
          />
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
      ) : (
        <div className="space-y-4">
          <div className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 xl:grid-cols-6">
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
