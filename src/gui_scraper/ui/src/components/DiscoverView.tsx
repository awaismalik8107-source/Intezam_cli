import { Compass, Filter, Search } from "lucide-react";
import { m } from "motion/react";
import { useMemo, useState } from "react";
import { QUALITY_RANK } from "../lib/mediaAdapter";
import { PAGE_VARIANTS } from "../lib/motion";
import type { MediaEntry, QualityTier } from "../lib/types";
import { PosterCard } from "./PosterCard";

interface DiscoverViewProps {
  mediaItems: MediaEntry[];
  libraryKeys: Set<string>;
  isLoading: boolean;
  onSelectMedia: (media: MediaEntry) => void;
  onToggleLibrary: (media: MediaEntry) => void;
  onQuickSearch: (query: string) => void;
}

type TypeFilter = "all" | "release" | "series";
type QualityFilter = "all" | QualityTier;
type SortOrder = "discovered" | "sources" | "size" | "peers" | "date";

export function DiscoverView({
  mediaItems,
  libraryKeys,
  isLoading,
  onSelectMedia,
  onToggleLibrary,
  onQuickSearch,
}: DiscoverViewProps) {
  const [typeFilter, setTypeFilter] = useState<TypeFilter>("all");
  const [qualityFilter, setQualityFilter] = useState<QualityFilter>("all");
  const [verifiedOnly, setVerifiedOnly] = useState(false);
  const [sortOrder, setSortOrder] = useState<SortOrder>("discovered");

  const hasSeries = useMemo(
    () => mediaItems.some((entry) => entry.mediaType === "series"),
    [mediaItems],
  );

  const filtered = useMemo(() => {
    const list = mediaItems.filter((entry) => {
      if (typeFilter !== "all" && entry.mediaType !== typeFilter) return false;
      if (qualityFilter !== "all" && entry.bestQuality !== qualityFilter) return false;
      if (verifiedOnly && entry.verifiedCount === 0) return false;
      return true;
    });

    if (sortOrder === "discovered") return list;
    return [...list].sort((a, b) => {
      if (sortOrder === "sources") return b.sources.length - a.sources.length;
      if (sortOrder === "size") return b.largestSizeBytes - a.largestSizeBytes;
      if (sortOrder === "peers") return (b.maxSeeders ?? -1) - (a.maxSeeders ?? -1);
      if (sortOrder === "date") return b.latestDate.localeCompare(a.latestDate);
      return QUALITY_RANK[b.bestQuality] - QUALITY_RANK[a.bestQuality];
    });
  }, [mediaItems, typeFilter, qualityFilter, verifiedOnly, sortOrder]);

  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="space-y-6 pb-10"
    >
      {/* Filter Bar */}
      <div className="flex flex-wrap items-center justify-between gap-3 rounded-2xl border border-white/[0.07] bg-[#111522] p-3.5">
        <div className="flex flex-wrap items-center gap-2">
          <span className="inline-flex items-center gap-1.5 pr-1 text-xs font-medium text-[#949CAF]">
            <Filter className="h-3.5 w-3.5 text-[#786BFF]" aria-hidden="true" />
            Filters
          </span>

          <select
            aria-label="Filter by media type"
            value={typeFilter}
            disabled={!hasSeries}
            onChange={(e) => setTypeFilter(e.target.value as TypeFilter)}
            className="h-8 rounded-lg border border-white/10 bg-[#090B12] px-2.5 text-xs text-[#F8FAFC] focus:border-[#786BFF] focus:outline-none disabled:opacity-45"
          >
            <option value="all">All Types</option>
            <option value="release">Movies / Releases</option>
            <option value="series">TV Series</option>
          </select>

          <select
            aria-label="Filter by quality"
            value={qualityFilter}
            onChange={(e) => setQualityFilter(e.target.value as QualityFilter)}
            className="h-8 rounded-lg border border-white/10 bg-[#090B12] px-2.5 text-xs text-[#F8FAFC] focus:border-[#786BFF] focus:outline-none"
          >
            <option value="all">All Qualities</option>
            <option value="4K">4K / 2160p (inferred)</option>
            <option value="1080p">1080p (inferred)</option>
            <option value="720p">720p (inferred)</option>
            <option value="Other">Other / Unspecified</option>
          </select>

          <button
            type="button"
            onClick={() => setVerifiedOnly((v) => !v)}
            className={`h-8 rounded-lg border px-3 text-xs font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
              verifiedOnly
                ? "border-emerald-400/40 bg-emerald-400/15 text-emerald-300"
                : "border-white/10 bg-[#090B12] text-[#949CAF] hover:text-[#F8FAFC]"
            }`}
          >
            Verified Sources Only
          </button>
        </div>

        <div className="flex items-center gap-2">
          <label htmlFor="discover-sort" className="text-xs text-[#949CAF]">
            Sort
          </label>
          <select
            id="discover-sort"
            value={sortOrder}
            onChange={(e) => setSortOrder(e.target.value as SortOrder)}
            className="h-8 rounded-lg border border-white/10 bg-[#090B12] px-2.5 text-xs text-[#F8FAFC] focus:border-[#786BFF] focus:outline-none"
          >
            <option value="discovered">Discovered Order</option>
            <option value="sources">Most Sources</option>
            <option value="size">Largest Size</option>
            <option value="peers">Most Seeders</option>
            <option value="date">Newest Date</option>
          </select>
        </div>
      </div>

      {/* Loading Skeleton Poster Grid */}
      {isLoading ? (
        <div
          aria-busy="true"
          className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 xl:grid-cols-6"
        >
          {Array.from({ length: 12 }, (_, i) => (
            <div key={i} className="space-y-2">
              <div className="skeleton-shimmer aspect-[2/3] w-full rounded-xl" />
              <div className="skeleton-shimmer h-3.5 w-3/4 rounded" />
              <div className="skeleton-shimmer h-3 w-1/2 rounded" />
            </div>
          ))}
        </div>
      ) : filtered.length === 0 ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522]/70 px-6 py-20 text-center">
          <div className="mb-4 flex h-12 w-12 items-center justify-center rounded-2xl border border-[#786BFF]/25 bg-[#786BFF]/10">
            <Compass className="h-5 w-5 text-[#786BFF]" aria-hidden="true" />
          </div>
          <h2 className="text-base font-semibold text-[#F8FAFC]">
            {mediaItems.length === 0
              ? "Discover catalog is waiting for a query"
              : "No titles match these filters"}
          </h2>
          <p className="mt-1.5 max-w-md text-xs leading-relaxed text-[#949CAF]">
            {mediaItems.length === 0
              ? "Run a search using Mock Data or your configured Live Scraper to populate the Discover poster grid."
              : "Reset your active quality or verification filters to inspect all discovered releases."}
          </p>
          <div className="mt-5 flex flex-wrap justify-center gap-2">
            {mediaItems.length === 0 ? (
              <button
                type="button"
                onClick={() => onQuickSearch("Ubuntu 24.04")}
                className="inline-flex h-9 items-center gap-2 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC] hover:bg-[#6A5CFF]"
              >
                <Search className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Discover Sample Catalog</span>
              </button>
            ) : (
              <button
                type="button"
                onClick={() => {
                  setTypeFilter("all");
                  setQualityFilter("all");
                  setVerifiedOnly(false);
                }}
                className="h-9 rounded-xl border border-white/10 bg-[#191E2E] px-4 text-xs font-medium text-[#F8FAFC]"
              >
                Reset Filters
              </button>
            )}
          </div>
        </div>
      ) : (
        <div className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 xl:grid-cols-6">
          {filtered.map((media, idx) => (
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
      )}
    </m.div>
  );
}
