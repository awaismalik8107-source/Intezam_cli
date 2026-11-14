import { ArrowLeft, Bookmark, Check, Copy, Layers3, Magnet, RefreshCw, Search } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { buildEpisodeCatalog } from "../lib/episodeCatalog";
import { QUALITY_RANK } from "../lib/mediaAdapter";
import type { EngineStatus, MediaEntry, MediaSource, StreamSessionState } from "../lib/types";
import { PosterArtwork } from "./PosterArtwork";

interface MediaDetailsViewProps {
  media: MediaEntry;
  isSaved: boolean;
  engineStatus: EngineStatus | null;
  streamState: StreamSessionState | null;
  verifyingId: number | null;
  strictCacheBudget: boolean;
  onBack: () => void;
  onToggleLibrary: (media: MediaEntry) => void;
  onVerifySource: (source: MediaSource) => void;
  onCopyUri: (source: MediaSource) => Promise<boolean>;
  onLaunchIntezam: (source: MediaSource) => void;
  onStopIntezam: () => void;
  onOpenExternal: (source: MediaSource) => void;
  loadingEpisodes?: boolean;
  episodeError?: string | null;
  moreEpisodesAvailable?: boolean;
  onLoadEpisodes?: () => void;
}

function magnetOf(source: MediaSource): string | null {
  const candidate = (source.item.magnet_link || source.item.resolved_link || "").trim();
  return candidate.toLowerCase().startsWith("magnet:?") ? candidate : null;
}

function sourceKey(source: MediaSource): string {
  return source.infoHash || magnetOf(source) || source.item.view_url || source.sourceTitle;
}

export function MediaDetailsView({
  media, isSaved, onBack, onToggleLibrary, onCopyUri,
  loadingEpisodes = false, episodeError = null,
  moreEpisodesAvailable = false, onLoadEpisodes,
}: MediaDetailsViewProps) {
  const catalog = useMemo(() => buildEpisodeCatalog(media.sources), [media.sources]);
  const [season, setSeason] = useState<number | null>(null);
  const [episode, setEpisode] = useState<number | null>(null);
  const [expandedSource, setExpandedSource] = useState<string | null>(null);

  useEffect(() => {
    setSeason(null);
    setEpisode(null);
    setExpandedSource(null);
  }, [media.key]);

  const currentSeason = season !== null && catalog.seasons.includes(season)
    ? season
    : catalog.seasons[0] ?? 1;
  const visibleEpisodes = catalog.episodes.filter((entry) => entry.season === currentSeason);
  const selectedEpisode = visibleEpisodes.find((entry) => entry.number === episode) ?? null;
  const availableBatches = catalog.batches.filter(
    (batch) => batch.season === null || batch.season === currentSeason,
  );

  const sortedSources = useMemo(
    () => [...(selectedEpisode?.sources ?? [])].sort((a, b) =>
      Number(Boolean(magnetOf(b))) - Number(Boolean(magnetOf(a))) ||
      (b.seeders ?? -1) - (a.seeders ?? -1) ||
      QUALITY_RANK[b.qualityTier] - QUALITY_RANK[a.qualityTier],
    ),
    [selectedEpisode],
  );

  const handleSelectEpisode = (number: number) => {
    setEpisode(number);
    setExpandedSource(null);
  };

  const renderMagnet = (source: MediaSource, showInitially = false) => {
    const magnet = magnetOf(source);
    const isOpen = showInitially || expandedSource === sourceKey(source);
    if (!magnet) {
      return <p className="text-xs text-amber-200/80">No magnet URI in this release. A torrent-file link is not a magnet link.</p>;
    }
    return (
      <div className="space-y-2">
        {!isOpen && (
          <button
            type="button"
            onClick={() => setExpandedSource(sourceKey(source))}
            className="text-xs font-medium text-[#AFA6FF] hover:text-white"
          >
            Show magnet URI
          </button>
        )}
        {isOpen && (
          <div className="space-y-2">
            <code className="block max-h-28 overflow-auto break-all rounded-lg border border-white/10 bg-[#090B12] p-3 font-mono text-[11px] leading-relaxed text-[#C8D0E2] select-all">{magnet}</code>
            <button
              type="button"
              onClick={() => void onCopyUri(source)}
              className="inline-flex items-center gap-2 rounded-lg border border-[#786BFF]/35 bg-[#786BFF]/10 px-3 py-2 text-xs font-semibold text-[#DAD6FF] hover:bg-[#786BFF]/20 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <Copy className="h-3.5 w-3.5" aria-hidden="true" /> Copy magnet link
            </button>
          </div>
        )}
      </div>
    );
  };

  return (
    <div className="space-y-6 pb-12">
      <section className="relative overflow-hidden rounded-2xl border border-white/10 bg-[#111522] p-5 sm:p-7">
        <PosterArtwork
          title={media.title} year={media.year} quality={media.bestQuality}
          mediaType={media.mediaType} variant="backdrop"
        />
        <div className="relative z-10 flex flex-col gap-5 sm:flex-row sm:items-start">
          <div className="hidden w-28 shrink-0 overflow-hidden rounded-xl border border-white/15 shadow-xl sm:block">
            <PosterArtwork title={media.title} year={media.year} quality={media.bestQuality} mediaType={media.mediaType} />
          </div>
          <div className="min-w-0 flex-1 space-y-3">
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-[#AAB5CA]">
              <span className="rounded-md border border-white/10 bg-black/40 px-2 py-1">{media.providerName}</span>
              <span className="rounded-md border border-white/10 bg-black/40 px-2 py-1">{media.mediaType === "series" ? "Series" : "Release"}</span>
              <span className="rounded-md border border-white/10 bg-black/40 px-2 py-1">{media.sources.length} collected releases</span>
            </div>
            <h2 className="break-words text-2xl font-bold tracking-tight text-white sm:text-3xl">{media.title}</h2>
            <p className="max-w-xl text-sm leading-relaxed text-[#AAB5CA]">
              Browse available seasons, choose an episode, then inspect and copy its magnet links.
              Intezam playback is not connected to this screen.
            </p>
            <div className="flex flex-wrap gap-2 pt-2">
              <button
                type="button" onClick={onBack}
                className="inline-flex items-center gap-2 rounded-lg border border-white/15 bg-[#191E2E] px-3 py-2 text-xs font-medium text-white hover:bg-[#232A40]"
              ><ArrowLeft className="h-4 w-4" />Back to results</button>
              <button
                type="button" onClick={() => onToggleLibrary(media)}
                className="inline-flex items-center gap-2 rounded-lg border border-white/15 bg-[#191E2E] px-3 py-2 text-xs font-medium text-white hover:bg-[#232A40]"
              >{isSaved ? <Check className="h-4 w-4" /> : <Bookmark className="h-4 w-4" />}
                {isSaved ? "Saved to Library" : "Add to Library"}</button>
            </div>
          </div>
        </div>
      </section>

      <section className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="text-lg font-semibold text-white">Episodes</h3>
            <p className="mt-1 text-xs text-[#AAB5CA]">
              {catalog.episodes.length} distinct episodes found in {catalog.seasons.length} identified season(s). Counts reflect releases found, not an official episode list.
            </p>
          </div>
          {loadingEpisodes ? (
            <span role="status" className="inline-flex items-center gap-2 text-xs text-[#B3A8FF]"><RefreshCw className="h-3.5 w-3.5 animate-spin" />Finding episodes...</span>
          ) : onLoadEpisodes ? (
            <button
              type="button" onClick={onLoadEpisodes}
              className="inline-flex items-center gap-2 rounded-lg border border-white/10 px-3 py-2 text-xs text-[#D5D7E3] hover:bg-white/10"
            ><RefreshCw className="h-3.5 w-3.5" />Refresh sources</button>
          ) : null}
        </div>
        {episodeError && <p role="alert" className="rounded-lg border border-amber-400/20 bg-amber-400/5 p-3 text-xs text-amber-100">{episodeError}</p>}
        {moreEpisodesAvailable && (
          <p className="text-xs text-[#AAB5CA]">Additional search pages are available. The current list is not necessarily complete.</p>
        )}
        {catalog.seasons.length > 0 && (
          <div role="group" aria-label="Choose season" className="flex flex-wrap gap-2">
            {catalog.seasons.map((value) => (
              <button
                key={value} type="button" onClick={() => { setSeason(value); setEpisode(null); setExpandedSource(null); }}
                aria-pressed={currentSeason === value}
                className={`rounded-lg px-4 py-2 text-xs font-semibold focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${currentSeason === value ? "border border-[#786BFF]/60 bg-[#786BFF]/20 text-white" : "border border-white/10 bg-[#191E2E] text-[#AAB5CA] hover:text-white"}`}
              >Season {value}</button>
            ))}
          </div>
        )}
        {visibleEpisodes.length > 0 ? (
          <div role="group" aria-label="Choose episode" className="grid grid-cols-4 gap-2 sm:grid-cols-6 md:grid-cols-8 xl:grid-cols-10">
            {visibleEpisodes.map((entry) => (
              <button
                type="button" key={entry.number} onClick={() => handleSelectEpisode(entry.number)}
                aria-label={`Episode ${entry.number}`} aria-pressed={episode === entry.number}
                className={`flex min-h-16 flex-col items-center justify-center gap-1 rounded-xl border px-2 py-2 text-xs transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${episode === entry.number ? "border-[#786BFF] bg-[#786BFF]/20 text-white" : "border-white/10 bg-[#191E2E] text-[#D2D7E4] hover:border-white/25 hover:bg-[#232A40]"}`}
              >
                <span className="text-[10px] text-[#AAB5CA]">EP</span>
                <strong className="text-base">{String(entry.number).padStart(2, "0")}</strong>
                <span className="text-[10px] text-[#AAB5CA]">{entry.sources.length} source{entry.sources.length === 1 ? "" : "s"}</span>
              </button>
            ))}
          </div>
        ) : (
          <div className="flex items-center gap-3 rounded-xl border border-dashed border-white/10 bg-[#0C101C] p-5 text-xs text-[#AAB5CA]">
            <Search className="h-5 w-5 shrink-0" />No individually numbered episodes were identified for this season. Check batch releases below or refresh sources.
          </div>
        )}
      </section>

      <section aria-live="polite" className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6">
        <div>
          <h3 className="text-lg font-semibold text-white">
            {selectedEpisode ? `Season ${selectedEpisode.season} · Episode ${selectedEpisode.number} — Magnet sources` : "Select an episode"}
          </h3>
          <p className="mt-1 text-xs text-[#AAB5CA]">
            {selectedEpisode ? "Compare available releases and copy the magnet you want." : "Source links appear here after you choose an episode above."}
          </p>
        </div>
        {selectedEpisode && (
          <div className="space-y-3">
            {sortedSources.map((source) => (
              <article key={sourceKey(source)} className="space-y-3 rounded-xl border border-white/[0.08] bg-[#191E2E] p-4">
                <div className="flex flex-wrap items-center gap-3">
                  <Magnet className="h-4 w-4 shrink-0 text-[#AFA6FF]" />
                  <div className="min-w-0 flex-1">
                    <p className="break-words text-sm font-medium text-white">{source.sourceTitle}</p>
                    <p className="mt-1 text-[11px] text-[#AAB5CA]">
                      {source.qualityTier} (inferred) · {source.item.size} · {source.seeders ?? "?"} seeders · {source.providerName}
                    </p>
                  </div>
                </div>
                {renderMagnet(source, true)}
              </article>
            ))}
          </div>
        )}
      </section>

      {availableBatches.length > 0 && (
        <section className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6">
          <div className="flex items-center gap-3">
            <Layers3 className="h-5 w-5 text-[#AFA6FF]" />
            <div>
              <h3 className="text-lg font-semibold text-white">Batch & season packs</h3>
              <p className="mt-1 text-xs text-[#AAB5CA]">Full-season torrents remain separate from individual episodes. Contents are not assumed until inspected.</p>
            </div>
          </div>
          <div className="space-y-3">
            {availableBatches.map((batch) => (
              <article key={sourceKey(batch.source)} className="space-y-3 rounded-xl border border-white/10 bg-[#191E2E] p-4">
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <div className="min-w-0">
                    <p className="break-words text-sm font-medium text-white">{batch.source.sourceTitle}</p>
                    <p className="mt-1 text-xs text-[#AAB5CA]">{batch.firstEpisode !== null && batch.lastEpisode !== null ? `Episodes ${batch.firstEpisode}–${batch.lastEpisode} (inferred from title)` : "Episode range unverified"} · {batch.source.item.size}</p>
                  </div>
                  <span className="shrink-0 rounded-full border border-[#786BFF]/35 px-2 py-1 text-[10px] text-[#AFA6FF]">Batch</span>
                </div>
                {renderMagnet(batch.source)}
              </article>
            ))}
          </div>
        </section>
      )}

      {catalog.other.length > 0 && (
        <details className="rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6">
          <summary className="cursor-pointer text-sm font-semibold text-white">
            Unclassified releases ({catalog.other.length}) — show raw sources
          </summary>
          <div className="mt-4 space-y-3">
            {catalog.other.map((source) => (
              <article key={sourceKey(source)} className="space-y-2 rounded-xl border border-white/10 bg-[#191E2E] p-3">
                <p className="break-words text-xs text-white">{source.sourceTitle}</p>
                {renderMagnet(source)}
              </article>
            ))}
          </div>
        </details>
      )}
    </div>
  );
}
