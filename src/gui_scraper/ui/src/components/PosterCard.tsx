import { ArrowRight, Bookmark, Check, Layers, Tv } from "lucide-react";
import { m } from "motion/react";
import { useMemo } from "react";
import { buildEpisodeCatalog } from "../lib/episodeCatalog";
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
  const catalog = useMemo(
    () => buildEpisodeCatalog(media.sources, media.officialSeasons),
    [media.sources, media.officialSeasons],
  );
  const officialSeasonCount = media.officialSeasons?.length ?? 0;
  const officialEpisodeTotal = useMemo(
    () =>
      (media.officialSeasons ?? []).reduce(
        (acc, s) => acc + (s.episode_count || s.episodes?.length || 0),
        0,
      ),
    [media.officialSeasons],
  );
  const seasonCount = catalog.seasons.length || officialSeasonCount;
  const episodeCount = catalog.episodes.length || officialEpisodeTotal;
  const batchCount = catalog.batches.length;

  const seasonSummary =
    seasonCount > 1
      ? `${seasonCount} Seasons`
      : seasonCount === 1
        ? `Season ${catalog.seasons[0] ?? media.officialSeasons?.[0]?.season_number ?? 1}`
        : batchCount > 0
          ? "Batch Pack"
          : "Series";

  const episodeSummary =
    episodeCount > 0
      ? `${episodeCount} ${episodeCount === 1 ? "Episode" : "Episodes"}`
      : batchCount > 0
        ? `${batchCount} ${batchCount === 1 ? "Batch" : "Batches"}`
        : media.sources.length > 0
          ? `${media.sources.length} ${media.sources.length === 1 ? "Release" : "Releases"}`
          : media.metadataProvider || "Multi-Source";

  return (
    <m.article
      custom={index}
      variants={CARD_LIST_VARIANTS}
      initial="hidden"
      animate="visible"
      whileHover={{ y: -3 }}
      transition={CARD_SPRING}
      className={`group relative flex shrink-0 flex-col rounded-2xl border border-white/[0.08] bg-[#111522] p-2.5 shadow-[0_12px_30px_-14px_rgba(0,0,0,0.85)] transition-colors duration-200 hover:border-[#786BFF]/55 ${
        compactWidth ? "w-[184px] sm:w-[200px]" : "w-full"
      }`}
    >
      {/* Poster Frame */}
      <div className="relative aspect-[2/3] w-full overflow-hidden rounded-xl border border-white/[0.07] bg-[#0B0D16]">
        <button
          type="button"
          onClick={() => onSelect(media)}
          aria-label={`Open details for ${media.title}`}
          className="block h-full w-full text-left focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
        >
          <div className="h-full w-full transition-transform duration-300 ease-out group-hover:scale-[1.02]">
            <PosterArtwork
              title={media.title}
              year={media.year}
              mediaType={media.mediaType}
              posterUrl={media.posterUrl}
            />
          </div>

          {/* Hover Open Show Overlay */}
          <div
            aria-hidden="true"
            className="pointer-events-none absolute inset-0 flex items-center justify-center bg-black/45 opacity-0 transition-opacity duration-200 group-hover:opacity-100"
          >
            <span className="inline-flex items-center gap-1.5 rounded-full border border-white/25 bg-[#786BFF] px-3.5 py-1.5 text-xs font-semibold text-white shadow-lg">
              <span>Open Show</span>
              <ArrowRight className="h-3.5 w-3.5" />
            </span>
          </div>
        </button>

        {/* Bookmark action */}
        <button
          type="button"
          aria-label={
            isSaved ? `Remove ${media.title} from Library` : `Save ${media.title} to Library`
          }
          title={isSaved ? "Saved in Library" : "Save to Library"}
          onClick={(e) => {
            e.stopPropagation();
            onToggleLibrary(media);
          }}
          className={`absolute right-2.5 top-2.5 z-20 flex h-7 w-7 items-center justify-center rounded-lg border transition-opacity duration-150 focus-visible:opacity-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
            isSaved
              ? "border-[#786BFF]/50 bg-[#786BFF]/90 text-[#F8FAFC] opacity-100"
              : "border-white/15 bg-black/60 text-[#F8FAFC] opacity-0 hover:bg-black/80 group-hover:opacity-100"
          }`}
        >
          {isSaved ? (
            <Check className="h-3.5 w-3.5" aria-hidden="true" />
          ) : (
            <Bookmark className="h-3.5 w-3.5" aria-hidden="true" />
          )}
        </button>
      </div>

      {/* Show Metadata & Explicit Action */}
      <div className="mt-3 flex flex-1 flex-col justify-between gap-2.5 px-0.5">
        <div className="space-y-1.5">
          <h3
            title={media.title}
            className="line-clamp-2 min-h-[2.5rem] break-words text-[13.5px] font-semibold leading-snug text-[#F8FAFC] transition-colors group-hover:text-white"
          >
            {media.title}
          </h3>

          <div className="flex flex-wrap items-center gap-1.5 text-[11px] text-[#AAB5CA]">
            <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#191E2E] px-1.5 py-0.5 font-medium text-[#D6DDF0]">
              <Tv className="h-3 w-3 text-[#786BFF]" aria-hidden="true" />
              {seasonSummary}
            </span>
            <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#0B0D16] px-1.5 py-0.5 text-[#949CAF]">
              <Layers className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
              {episodeSummary}
            </span>
          </div>
        </div>

        <button
          type="button"
          onClick={() => onSelect(media)}
          className="inline-flex w-full items-center justify-between rounded-xl border border-white/[0.08] bg-[#191E2E]/85 px-3 py-1.5 text-xs font-medium text-[#E2E8F0] transition-colors hover:border-[#786BFF]/50 hover:bg-[#232A40] hover:text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
        >
          <span>Browse Episodes</span>
          <ArrowRight className="h-3.5 w-3.5 text-[#AFA6FF]" aria-hidden="true" />
        </button>
      </div>
    </m.article>
  );
}
