import { Bookmark, Check, Clock, Compass, Play, Search, Sparkles } from "lucide-react";
import { AnimatePresence, m } from "motion/react";
import { useState } from "react";
import { HERO_VARIANTS, PAGE_VARIANTS } from "../lib/motion";
import type {
  MediaEntry,
  SearchHistoryEntry,
  WatchHistoryEntry,
} from "../lib/types";
import { ContentShelf } from "./ContentShelf";
import { PosterArtwork } from "./PosterArtwork";

interface HomeViewProps {
  discoveredMedia: MediaEntry[];
  libraryMedia: MediaEntry[];
  libraryKeys: Set<string>;
  searchHistory: SearchHistoryEntry[];
  watchHistory: WatchHistoryEntry[];
  onSelectMedia: (media: MediaEntry) => void;
  onToggleLibrary: (media: MediaEntry) => void;
  onQuickSearch: (query: string) => void;
  onNavigate: (section: "discover" | "search" | "library" | "continue") => void;
  onLaunchWatchEntry: (entry: WatchHistoryEntry) => void;
}

const STARTER_QUERIES = ["Frieren 1080p", "Dandadan", "Dungeon Meshi", "Orb 1080p"] as const;

export function HomeView({
  discoveredMedia,
  libraryMedia,
  libraryKeys,
  searchHistory,
  watchHistory,
  onSelectMedia,
  onToggleLibrary,
  onQuickSearch,
  onNavigate,
  onLaunchWatchEntry,
}: HomeViewProps) {
  const featuredCandidates = discoveredMedia.length > 0 ? discoveredMedia.slice(0, 4) : libraryMedia.slice(0, 4);
  const [heroIndex, setHeroIndex] = useState(0);
  const safeHeroIndex = featuredCandidates.length > 0 ? heroIndex % featuredCandidates.length : 0;
  const featured = featuredCandidates[safeHeroIndex] ?? null;

  const seriesMedia = discoveredMedia.filter((mEntry) => mEntry.mediaType === "series");
  const releaseMedia = discoveredMedia.filter((mEntry) => mEntry.mediaType === "release");

  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="space-y-8 pb-10"
    >
      {/* Featured Hero Banner */}
      <section className="relative overflow-hidden rounded-2xl border border-white/[0.08] bg-[#111522] shadow-[0_24px_60px_-24px_rgba(0,0,0,0.9)]">
        <AnimatePresence mode="wait">
          {featured ? (
            <m.div
              key={featured.key}
              variants={HERO_VARIANTS}
              initial="initial"
              animate="animate"
              exit="exit"
              className="relative flex min-h-[280px] flex-col justify-end p-6 sm:min-h-[320px] sm:p-8"
            >
              <PosterArtwork
                title={featured.title}
                year={featured.year}
                quality={featured.bestQuality}
                mediaType={featured.mediaType}
                variant="backdrop"
              />

              <div className="relative z-10 max-w-2xl space-y-3.5">
                <div className="flex flex-wrap items-center gap-2 text-[11px]">
                  <span className="inline-flex items-center gap-1.5 rounded-full border border-[#786BFF]/35 bg-[#786BFF]/15 px-2.5 py-0.5 font-medium text-[#F8FAFC]">
                    <Sparkles className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
                    Featured Discovery
                  </span>
                  {featured.year && (
                    <span className="rounded-md border border-white/10 bg-black/35 px-2 py-0.5 font-mono text-[#F8FAFC]">
                      {featured.year}
                    </span>
                  )}
                  {featured.bestQuality !== "Other" && (
                    <span className="rounded-md border border-white/10 bg-black/35 px-2 py-0.5 font-mono text-[#38BDF8]">
                      {featured.bestQuality} (inferred)
                    </span>
                  )}
                  <span className="font-mono text-[#949CAF]">
                    {featured.sources.length} {featured.sources.length === 1 ? "source" : "sources"} ·{" "}
                    {featured.providerName}
                  </span>
                </div>

                <h2 className="text-2xl font-bold tracking-tight text-[#F8FAFC] sm:text-3xl">
                  {featured.title}
                </h2>

                <p className="max-w-xl text-xs leading-relaxed text-[#949CAF] sm:text-[13px]">
                  {featured.verifiedCount} verified{" "}
                  {featured.verifiedCount === 1 ? "link" : "links"} available
                  {featured.magnetCount > 0 ? ` · ${featured.magnetCount} magnet stream(s)` : ""}
                  {featured.largestSizeLabel !== "N/A"
                    ? ` · Largest asset ${featured.largestSizeLabel}`
                    : ""}
                  . Select to inspect streams and prepare Intezam Flow playback.
                </p>

                <div className="flex flex-wrap items-center gap-3 pt-1">
                  <button
                    type="button"
                    onClick={() => onSelectMedia(featured)}
                    className="inline-flex h-10 items-center gap-2 rounded-xl bg-gradient-to-r from-[#786BFF] to-[#5B8DEF] px-5 text-xs font-semibold text-[#F8FAFC] shadow-lg shadow-[#786BFF]/25 transition-transform duration-150 hover:scale-[1.015] active:scale-[0.985] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
                  >
                    <Play className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
                    <span>View Sources</span>
                  </button>

                  <button
                    type="button"
                    onClick={() => onToggleLibrary(featured)}
                    className="inline-flex h-10 items-center gap-2 rounded-xl border border-white/15 bg-[#191E2E] px-4 text-xs font-medium text-[#F8FAFC] transition-colors hover:bg-[#232A40] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
                  >
                    {libraryKeys.has(featured.key) ? (
                      <>
                        <Check className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
                        <span>In Library</span>
                      </>
                    ) : (
                      <>
                        <Bookmark className="h-3.5 w-3.5" aria-hidden="true" />
                        <span>Add to Library</span>
                      </>
                    )}
                  </button>
                </div>
              </div>

              {featuredCandidates.length > 1 && (
                <div className="relative z-10 mt-6 flex items-center gap-2">
                  {featuredCandidates.map((candidate, idx) => (
                    <button
                      key={candidate.key}
                      type="button"
                      aria-label={`Feature ${candidate.title}`}
                      onClick={() => setHeroIndex(idx)}
                      className={`h-1.5 rounded-full transition-all duration-200 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                        idx === safeHeroIndex
                          ? "w-7 bg-[#786BFF]"
                          : "w-2 bg-white/25 hover:bg-white/45"
                      }`}
                    />
                  ))}
                </div>
              )}
            </m.div>
          ) : (
            <m.div
              key="empty-hero"
              variants={HERO_VARIANTS}
              initial="initial"
              animate="animate"
              exit="exit"
              className="relative flex min-h-[290px] flex-col justify-end p-6 sm:min-h-[320px] sm:p-8"
            >
              <PosterArtwork title="Intezam Cinema" variant="backdrop" />
              <div className="relative z-10 max-w-2xl space-y-4">
                <span className="inline-flex items-center gap-1.5 rounded-full border border-[#786BFF]/35 bg-[#786BFF]/15 px-3 py-1 text-[11px] font-medium text-[#F8FAFC]">
                  <Compass className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
                  Intezam Cinema Discovery
                </span>
                <h2 className="text-2xl font-bold tracking-tight text-[#F8FAFC] sm:text-3xl">
                  Your next watch starts here.
                </h2>
                <p className="max-w-xl text-xs leading-relaxed text-[#949CAF] sm:text-[13px]">
                  Search configured providers or preview with Mock Data to organize releases into
                  media cards, verify stream URIs, and launch playback through Intezam Flow.
                </p>

                <div className="flex flex-wrap items-center gap-2 pt-1">
                  {STARTER_QUERIES.map((sample) => (
                    <button
                      key={sample}
                      type="button"
                      onClick={() => onQuickSearch(sample)}
                      className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/12 bg-[#191E2E]/85 px-3.5 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-[#786BFF]/50 hover:bg-[#191E2E] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
                    >
                      <Search className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
                      <span>{sample}</span>
                    </button>
                  ))}
                </div>
              </div>
            </m.div>
          )}
        </AnimatePresence>
      </section>

      {/* Continue Watching (only when genuine stream launches exist) */}
      {watchHistory.length > 0 && (
        <section className="space-y-3">
          <div className="flex items-center justify-between">
            <div>
              <h2 className="text-base font-semibold tracking-tight text-[#F8FAFC]">
                Continue Watching
              </h2>
              <p className="mt-0.5 text-xs text-[#949CAF]">
                Recently launched streams from your local session history
              </p>
            </div>
            <button
              type="button"
              onClick={() => onNavigate("continue")}
              className="rounded-lg px-2.5 py-1 text-xs font-medium text-[#949CAF] hover:bg-[#191E2E] hover:text-[#F8FAFC]"
            >
              View All
            </button>
          </div>

          <div className="no-scrollbar flex gap-3.5 overflow-x-auto pb-1">
            {watchHistory.slice(0, 8).map((entry) => (
              <button
                key={entry.key}
                type="button"
                onClick={() => onLaunchWatchEntry(entry)}
                className="group relative flex w-[260px] shrink-0 flex-col justify-between overflow-hidden rounded-xl border border-white/[0.08] bg-[#111522] p-3.5 text-left transition-colors hover:border-[#786BFF]/50 hover:bg-[#191E2E] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <div className="flex items-center justify-between gap-2">
                  <span className="rounded bg-[#786BFF]/15 px-2 py-0.5 font-mono text-[10px] text-[#786BFF]">
                    {entry.engine}
                  </span>
                  <Play className="h-3.5 w-3.5 text-[#38BDF8] opacity-75 group-hover:opacity-100" aria-hidden="true" />
                </div>
                <div className="mt-3 line-clamp-2 text-xs font-semibold text-[#F8FAFC]">
                  {entry.title}
                </div>
                <div className="mt-2 font-mono text-[10px] text-[#949CAF]">
                  Launched {new Date(entry.launched_at).toLocaleDateString()}
                </div>
              </button>
            ))}
          </div>
        </section>
      )}

      {/* Horizontal Shelves from Real Discovered & Library Data */}
      <ContentShelf
        title="Recently Discovered"
        subtitle="Media groups built from your active search results"
        items={discoveredMedia}
        libraryKeys={libraryKeys}
        onSelectMedia={onSelectMedia}
        onToggleLibrary={onToggleLibrary}
        onViewAll={() => onNavigate("discover")}
      />

      {seriesMedia.length > 0 && (
        <ContentShelf
          title="TV Series"
          subtitle="Entries containing season or episode identifiers"
          items={seriesMedia}
          libraryKeys={libraryKeys}
          onSelectMedia={onSelectMedia}
          onToggleLibrary={onToggleLibrary}
        />
      )}

      {releaseMedia.length > 0 && seriesMedia.length > 0 && (
        <ContentShelf
          title="Movies & Releases"
          items={releaseMedia}
          libraryKeys={libraryKeys}
          onSelectMedia={onSelectMedia}
          onToggleLibrary={onToggleLibrary}
        />
      )}

      <ContentShelf
        title="Your Library"
        subtitle="Saved locally inside src/gui_scraper/.data"
        items={libraryMedia}
        libraryKeys={libraryKeys}
        onSelectMedia={onSelectMedia}
        onToggleLibrary={onToggleLibrary}
        onViewAll={() => onNavigate("library")}
      />

      {/* Recently Searched */}
      {searchHistory.length > 0 && (
        <section className="space-y-3">
          <div className="flex items-center gap-2">
            <Clock className="h-4 w-4 text-[#949CAF]" aria-hidden="true" />
            <h2 className="text-sm font-semibold text-[#F8FAFC]">Recently Searched</h2>
          </div>
          <div className="flex flex-wrap gap-2">
            {searchHistory.slice(0, 10).map((item) => (
              <button
                key={`${item.query}-${item.mode}`}
                type="button"
                onClick={() => onQuickSearch(item.query)}
                className="inline-flex items-center gap-2 rounded-xl border border-white/[0.08] bg-[#111522] px-3 py-1.5 text-xs text-[#F8FAFC] transition-colors hover:border-[#786BFF]/45 hover:bg-[#191E2E] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <span>{item.query}</span>
                <span className="rounded bg-white/[0.06] px-1.5 py-0.5 font-mono text-[10px] text-[#949CAF]">
                  {item.result_count}
                </span>
              </button>
            ))}
          </div>
        </section>
      )}
    </m.div>
  );
}
