import { Bookmark, Search, Trash2 } from "lucide-react";
import { m } from "motion/react";
import { useMemo, useState } from "react";
import { PAGE_VARIANTS } from "../lib/motion";
import type { LibraryEntry, MediaEntry } from "../lib/types";
import { PosterCard } from "./PosterCard";

interface LibraryViewProps {
  libraryEntries: LibraryEntry[];
  libraryKeys: Set<string>;
  onSelectMedia: (media: MediaEntry) => void;
  onToggleLibrary: (media: MediaEntry) => void;
  onRemoveKey: (key: string) => void;
  onQuickSearch: (query: string) => void;
}

export function LibraryView({
  libraryEntries,
  libraryKeys,
  onSelectMedia,
  onToggleLibrary,
  onRemoveKey,
  onQuickSearch,
}: LibraryViewProps) {
  const [filterText, setFilterText] = useState("");

  const resolvedMedia = useMemo(() => {
    const q = filterText.trim().toLowerCase();
    return libraryEntries
      .map((entry): MediaEntry => {
        if (entry.mediaSnapshot) return entry.mediaSnapshot;
        return {
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
        };
      })
      .filter((entry) => !q || entry.title.toLowerCase().includes(q));
  }, [libraryEntries, filterText]);

  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="space-y-6 pb-10"
    >
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 className="text-lg font-semibold text-[#F8FAFC]">Your Library</h2>
          <p className="mt-0.5 text-xs text-[#949CAF]">
            Locally persisted inside <span className="font-mono">src/gui_scraper/.data</span>
          </p>
        </div>

        {libraryEntries.length > 0 && (
          <div className="relative w-full max-w-xs">
            <Search
              className="pointer-events-none absolute left-3 top-1/2 h-3.5 w-3.5 -translate-y-1/2 text-[#949CAF]"
              aria-hidden="true"
            />
            <input
              type="text"
              value={filterText}
              onChange={(e) => setFilterText(e.target.value)}
              placeholder="Filter saved titles..."
              aria-label="Filter library"
              className="h-9 w-full rounded-xl border border-white/10 bg-[#111522] pl-9 pr-3 text-xs text-[#F8FAFC] placeholder-[#949CAF]/60 focus:border-[#786BFF] focus:outline-none"
            />
          </div>
        )}
      </div>

      {resolvedMedia.length === 0 ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522]/75 px-6 py-20 text-center">
          <div className="mb-4 flex h-12 w-12 items-center justify-center rounded-2xl border border-[#786BFF]/25 bg-[#786BFF]/10">
            <Bookmark className="h-5 w-5 text-[#786BFF]" aria-hidden="true" />
          </div>
          <h3 className="text-base font-semibold text-[#F8FAFC]">
            {libraryEntries.length === 0 ? "Your library is empty" : "No saved titles match"}
          </h3>
          <p className="mt-1.5 max-w-md text-xs text-[#949CAF]">
            {libraryEntries.length === 0
              ? "Bookmark any discovered release or movie to keep its sources close at hand."
              : "Clear your local filter to see all saved items."}
          </p>
          {libraryEntries.length === 0 && (
            <button
              type="button"
              onClick={() => onQuickSearch("Ubuntu 24.04")}
              className="mt-5 inline-flex h-9 items-center gap-2 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC]"
            >
              <Search className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Discover Releases</span>
            </button>
          )}
        </div>
      ) : (
        <div className="grid grid-cols-2 gap-4 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 xl:grid-cols-6">
          {resolvedMedia.map((media, idx) => (
            <div key={media.key} className="space-y-1.5">
              <PosterCard
                media={media}
                index={idx}
                isSaved={libraryKeys.has(media.key)}
                onSelect={(item) => {
                  if (item.sources.length > 0) {
                    onSelectMedia(item);
                  } else {
                    onQuickSearch(item.title);
                  }
                }}
                onToggleLibrary={onToggleLibrary}
              />
              <button
                type="button"
                onClick={() => onRemoveKey(media.key)}
                className="inline-flex items-center gap-1 text-[11px] text-[#949CAF] hover:text-rose-300"
              >
                <Trash2 className="h-3 w-3" aria-hidden="true" />
                <span>Remove</span>
              </button>
            </div>
          ))}
        </div>
      )}
    </m.div>
  );
}
