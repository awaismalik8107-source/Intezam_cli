import { Compass, RefreshCw, Search, ShieldCheck } from "lucide-react";
import { m } from "motion/react";
import { PAGE_VARIANTS } from "../lib/motion";
import type { ScrapeMode, ScrapedItem, ScraperConfig } from "../lib/types";
import { NyaaLogo } from "./NyaaLogo";
import { PaginationBar } from "./PaginationBar";
import { TorrentDataGrid } from "./TorrentDataGrid";

interface ExtensionsViewProps {
  config: ScraperConfig;
  mode: ScrapeMode;
  items: ScrapedItem[];
  isLoading: boolean;
  isLoadingMore: boolean;
  currentPage: number;
  totalPages: number;
  hasNextPage: boolean;
  errorMessage: string | null;
  selectedRowId: number | null;
  onBrowseDefault: () => void;
  onQuickSearch: (query: string) => void;
  onSelectPage: (page: number) => void;
  onLoadMore: () => void;
  onSelectRow: (item: ScrapedItem) => void;
  onCopyMagnet: (item: ScrapedItem) => void;
  onOpenTorrent: (item: ScrapedItem) => void;
}

export function ExtensionsView({
  config,
  mode,
  items,
  isLoading,
  isLoadingMore,
  currentPage,
  totalPages,
  hasNextPage,
  errorMessage,
  selectedRowId,
  onBrowseDefault,
  onQuickSearch,
  onSelectPage,
  onLoadMore,
  onSelectRow,
  onCopyMagnet,
  onOpenTorrent,
}: ExtensionsViewProps) {
  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="space-y-5 pb-10"
    >
      {/* Installed Extension Hero Card: Nyaa */}
      <section className="relative overflow-hidden rounded-2xl border border-[#3582F7]/25 bg-gradient-to-br from-[#10182E] via-[#111522] to-[#0B0D16] p-5 sm:p-6">

        <div className="relative z-10 flex flex-wrap items-center justify-between gap-5">
          <div className="flex items-start gap-4">
            <NyaaLogo size={52} className="shrink-0 shadow-lg shadow-[#3582F7]/20" />
            <div className="space-y-1.5">
              <div className="flex flex-wrap items-center gap-2">
                <h2 className="text-lg font-bold tracking-tight text-[#F8FAFC]">
                  {config.provider_name || "Nyaa"} Extension
                </h2>
                <span className="inline-flex items-center gap-1 rounded-full border border-emerald-400/30 bg-emerald-400/10 px-2.5 py-0.5 text-[11px] font-medium text-emerald-300">
                  <ShieldCheck className="h-3 w-3" aria-hidden="true" />
                  <span>Active Provider</span>
                </span>
                <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[10.5px] text-[#38BDF8]">
                  c=1_2 · Anime (English)
                </span>
              </div>

              <p className="max-w-2xl text-xs leading-relaxed text-[#949CAF]">
                Structured table scraper for{" "}
                <span className="font-mono text-[#F8FAFC]">{config.base_url}</span> extracting
                title anchors, Magnet BTIH URIs, resolved <span className="font-mono">.torrent</span>{" "}
                links, size, relative timestamps, and live swarm peer metrics.
              </p>

              <div className="flex flex-wrap items-center gap-3 pt-1 font-mono text-[10.5px] text-[#949CAF]">
                <span>Browse: /?q=&amp;f=0&amp;c=1_2&amp;s=leechers&amp;o=desc</span>
                <span>·</span>
                <span>Search: /?f=0&amp;c=1_2&amp;q=&#123;query&#125;&amp;s=seeders&amp;o=desc</span>
              </div>
            </div>
          </div>

          <div className="flex flex-wrap items-center gap-2.5">
            <button
              type="button"
              disabled={isLoading}
              onClick={onBrowseDefault}
              className="inline-flex h-10 items-center gap-2 rounded-xl bg-gradient-to-r from-[#3582F7] to-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC] shadow-lg shadow-[#3582F7]/25 transition-transform hover:scale-[1.015] active:scale-[0.985] disabled:opacity-50"
            >
              {isLoading ? (
                <RefreshCw className="h-3.5 w-3.5 animate-spin" aria-hidden="true" />
              ) : (
                <Compass className="h-3.5 w-3.5" aria-hidden="true" />
              )}
              <span>Browse Nyaa ({mode === "mock" ? "Mock" : "Live"})</span>
            </button>

            <button
              type="button"
              onClick={() => onQuickSearch("Frieren 1080p")}
              className="inline-flex h-10 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3.5 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-[#38BDF8]/45"
            >
              <Search className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
              <span>Sample Query</span>
            </button>
          </div>
        </div>
      </section>

      {errorMessage && (
        <div
          role="alert"
          className="rounded-xl border border-rose-400/25 bg-rose-500/10 px-4 py-3 font-mono text-xs text-rose-300"
        >
          {errorMessage}
        </div>
      )}

      {/* Nyaa Torrent Table */}
      <section aria-label="Nyaa Torrent Results Grid" className="space-y-3">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div>
            <h3 className="text-sm font-semibold text-[#F8FAFC]">
              Torrent Index Grid ({items.length})
            </h3>
            <p className="text-[11px] text-[#949CAF]">
              Click any row to open the bottom slide-up inspector drawer with Magnet URI &amp; HTTP HEAD check.
            </p>
          </div>
        </div>

        <TorrentDataGrid
          items={items}
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
          itemCount={items.length}
          isLoading={isLoading}
          isLoadingMore={isLoadingMore}
          onSelectPage={onSelectPage}
          onLoadMore={onLoadMore}
        />
      </section>
    </m.div>
  );
}
