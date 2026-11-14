import { Bookmark, Check, Play } from "lucide-react";
import { m } from "motion/react";
import { CARD_LIST_VARIANTS, CARD_SPRING } from "../lib/motion";
import type { MediaEntry } from "../lib/types";
import { PosterArtwork } from "./PosterArtwork";

interface PosterCardProps {
  media: MediaEntry;
  index: number;
  isSaved: boolean;
  onSelect: (media: MediaEntry) => void;
  onToggleLibrary: (media: MediaEntry) => void;
  compactWidth?: boolean;
}

export function PosterCard({
  media,
  index,
  isSaved,
  onSelect,
  onToggleLibrary,
  compactWidth = false,
}: PosterCardProps) {
  return (
    <m.article
      custom={index}
      variants={CARD_LIST_VARIANTS}
      initial="hidden"
      animate="visible"
      whileHover={{ y: -4 }}
      transition={CARD_SPRING}
      className={`group relative flex shrink-0 flex-col ${
        compactWidth ? "w-[168px] sm:w-[184px]" : "w-full"
      }`}
    >
      {/* Poster Frame */}
      <div className="relative aspect-[2/3] w-full overflow-hidden rounded-xl border border-white/[0.08] bg-[#111522] shadow-[0_12px_30px_-14px_rgba(0,0,0,0.85)] transition-colors duration-200 group-hover:border-[#786BFF]/55 group-hover:shadow-[0_18px_38px_-14px_rgba(120,107,255,0.28)]">
        <button
          type="button"
          onClick={() => onSelect(media)}
          aria-label={`Open details for ${media.title}`}
          className="block h-full w-full text-left focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
        >
          <div className="h-full w-full transition-transform duration-300 ease-out group-hover:scale-[1.03] group-hover:brightness-110">
            <PosterArtwork
              title={media.title}
              year={media.year}
              quality={media.bestQuality}
              mediaType={media.mediaType}
            />
          </div>

          {/* Hover Play / Inspect Overlay */}
          <div
            aria-hidden="true"
            className="pointer-events-none absolute inset-0 flex items-center justify-center bg-black/35 opacity-0 transition-opacity duration-200 group-hover:opacity-100"
          >
            <div className="flex h-11 w-11 items-center justify-center rounded-full border border-white/25 bg-[#786BFF]/90 text-[#F8FAFC] shadow-lg shadow-[#786BFF]/30">
              <Play className="ml-0.5 h-4 w-4 fill-current" />
            </div>
          </div>
        </button>

        {/* Bookmark action */}
        <button
          type="button"
          aria-label={isSaved ? `Remove ${media.title} from Library` : `Save ${media.title} to Library`}
          title={isSaved ? "Saved in Library" : "Save to Library"}
          onClick={(e) => {
            e.stopPropagation();
            onToggleLibrary(media);
          }}
          className={`absolute bottom-2.5 right-2.5 z-20 flex h-7 w-7 items-center justify-center rounded-lg border transition-opacity duration-150 focus-visible:opacity-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
            isSaved
              ? "border-[#786BFF]/50 bg-[#786BFF]/85 text-[#F8FAFC] opacity-100"
              : "border-white/15 bg-black/55 text-[#F8FAFC] opacity-0 hover:bg-black/75 group-hover:opacity-100"
          }`}
        >
          {isSaved ? (
            <Check className="h-3.5 w-3.5" aria-hidden="true" />
          ) : (
            <Bookmark className="h-3.5 w-3.5" aria-hidden="true" />
          )}
        </button>
      </div>

      {/* Caption below poster */}
      <button
        type="button"
        onClick={() => onSelect(media)}
        className="mt-2.5 text-left focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] rounded"
      >
        <h3 className="line-clamp-1 text-[13px] font-medium text-[#F8FAFC] transition-colors group-hover:text-white">
          {media.title}
        </h3>
        <div className="mt-0.5 flex items-center gap-1.5 text-[11px] text-[#949CAF]">
          <span>
            {media.sources.length} {media.sources.length === 1 ? "source" : "sources"}
          </span>
          {media.verifiedCount > 0 && (
            <>
              <span aria-hidden="true">·</span>
              <span className="text-emerald-300">{media.verifiedCount} valid</span>
            </>
          )}
          {media.largestSizeLabel && media.largestSizeLabel !== "N/A" && (
            <>
              <span aria-hidden="true">·</span>
              <span className="font-mono text-[10.5px]">{media.largestSizeLabel}</span>
            </>
          )}
        </div>
      </button>
    </m.article>
  );
}
