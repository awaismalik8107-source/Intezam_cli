import {
  AlertTriangle,
  ArrowLeft,
  ArrowUpDown,
  Bookmark,
  Check,
  Copy,
  Filter,
  Globe,
  HardDrive,
  Layers3,
  Magnet,
  RefreshCw,
  Search,
  Users,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { type BatchKind, buildEpisodeCatalog } from "../lib/episodeCatalog";
import { QUALITY_RANK } from "../lib/mediaAdapter";
import type {
  EngineStatus,
  MediaEntry,
  MediaSource,
  SourceSearchFilters,
  StreamSessionState,
} from "../lib/types";
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
  onLoadMoreEpisodes?: () => void;
  sourceFilters?: SourceSearchFilters;
  onChangeSourceFilters?: (filters: SourceSearchFilters) => void;
}

type EpisodeSourceSort = "seeders" | "quality" | "size";

function magnetOf(source: MediaSource): string | null {
  const candidate = (source.item.magnet_link || source.item.resolved_link || "").trim();
  return candidate.toLowerCase().startsWith("magnet:?") ? candidate : null;
}

function sourceKey(source: MediaSource): string {
  return (
    source.infoHash ||
    magnetOf(source) ||
    source.item.torrent_link ||
    source.item.view_url ||
    source.sourceTitle
  );
}

function batchBadgeStyle(kind: BatchKind): { label: string; className: string } {
  if (kind === "multi-episode") {
    return {
      label: "Multi-Episode Range",
      className: "border-[#38BDF8]/35 bg-[#38BDF8]/10 text-[#7DD3FC]",
    };
  }
  if (kind === "complete-series") {
    return {
      label: "Complete-Series Pack",
      className: "border-emerald-400/35 bg-emerald-400/10 text-emerald-300",
    };
  }
  return {
    label: "Full-Season Batch",
    className: "border-[#786BFF]/40 bg-[#786BFF]/15 text-[#CFC9FF]",
  };
}

export function MediaDetailsView({
  media,
  isSaved,
  onBack,
  onToggleLibrary,
  onCopyUri,
  loadingEpisodes = false,
  episodeError = null,
  moreEpisodesAvailable = false,
  onLoadEpisodes,
  onLoadMoreEpisodes,
  sourceFilters,
  onChangeSourceFilters,
}: MediaDetailsViewProps) {
  const batchSectionRef = useRef<HTMLElement | null>(null);
  const catalog = useMemo(
    () => buildEpisodeCatalog(media.sources, media.officialSeasons),
    [media.sources, media.officialSeasons],
  );
  const [season, setSeason] = useState<number | null>(null);
  const [episode, setEpisode] = useState<number | null>(null);
  const [sortBy, setSortBy] = useState<EpisodeSourceSort>("seeders");
  const [expandedSource, setExpandedSource] = useState<string | null>(null);
  const [copiedKey, setCopiedKey] = useState<string | null>(null);

  useEffect(() => {
    setSeason(null);
    setEpisode(null);
    setExpandedSource(null);
    setCopiedKey(null);
  }, [media.key]);

  const currentSeason: number | null =
    season !== null && catalog.seasons.includes(season)
      ? season
      : catalog.seasons.length > 0
        ? catalog.seasons[0]
        : null;

  const visibleEpisodes = useMemo(
    () =>
      currentSeason !== null
        ? catalog.episodes.filter((entry) => entry.season === currentSeason)
        : [],
    [catalog.episodes, currentSeason],
  );

  const selectedEpisode =
    visibleEpisodes.find((entry) => entry.number === episode) ?? visibleEpisodes[0] ?? null;

  const availableBatches = useMemo(
    () =>
      catalog.batches.filter(
        (batch) =>
          currentSeason === null ||
          batch.season === null ||
          batch.season === currentSeason,
      ),
    [catalog.batches, currentSeason],
  );

  const isBatchOnlyShow =
    catalog.episodes.length === 0 && catalog.batches.length > 0;

  const sortedSources = useMemo(() => {
    const list = [...(selectedEpisode?.sources ?? [])];
    return list.sort((a, b) => {
      const magDiff = Number(Boolean(magnetOf(b))) - Number(Boolean(magnetOf(a)));
      if (magDiff !== 0) return magDiff;

      if (sortBy === "quality") {
        const qDiff = QUALITY_RANK[b.qualityTier] - QUALITY_RANK[a.qualityTier];
        if (qDiff !== 0) return qDiff;
        return (b.seeders ?? -1) - (a.seeders ?? -1);
      }
      if (sortBy === "size") {
        if (b.sizeBytes !== a.sizeBytes) return b.sizeBytes - a.sizeBytes;
        return (b.seeders ?? -1) - (a.seeders ?? -1);
      }
      const sDiff = (b.seeders ?? -1) - (a.seeders ?? -1);
      if (sDiff !== 0) return sDiff;
      return QUALITY_RANK[b.qualityTier] - QUALITY_RANK[a.qualityTier];
    });
  }, [selectedEpisode, sortBy]);

  const handleSelectEpisode = (number: number) => {
    setEpisode(number);
    setExpandedSource(null);
  };

  const handleCopyMagnet = async (source: MediaSource) => {
    const ok = await onCopyUri(source);
    if (ok) {
      const key = sourceKey(source);
      setCopiedKey(key);
      window.setTimeout(() => {
        setCopiedKey((prev) => (prev === key ? null : prev));
      }, 2000);
    }
  };

  const renderSourceMetadataBadges = (source: MediaSource) => {
    const hasMagnet = Boolean(magnetOf(source));
    const isMultiMirror = source.providerName.includes(" + ");
    return (
      <div className="mt-1.5 flex flex-wrap items-center gap-1.5 text-[11px]">
        <span
          className={`inline-flex items-center gap-1 rounded-md border px-2 py-0.5 font-medium ${
            hasMagnet
              ? "border-emerald-400/35 bg-emerald-400/10 text-emerald-300"
              : "border-amber-400/35 bg-amber-400/10 text-amber-200"
          }`}
        >
          <Magnet className="h-3 w-3" aria-hidden="true" />
          {hasMagnet ? "Magnet Available" : "Magnet Unavailable (.torrent only)"}
        </span>

        {source.releaseGroup && (
          <span className="rounded-md border border-[#38BDF8]/35 bg-[#38BDF8]/10 px-2 py-0.5 font-mono text-[#7DD3FC]">
            [{source.releaseGroup}]
          </span>
        )}

        <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#E2E8F0]">
          {source.inferredResolution || source.qualityTier} (inferred)
        </span>

        {source.inferredCodec && (
          <span className="rounded-md border border-[#786BFF]/30 bg-[#786BFF]/10 px-2 py-0.5 font-mono text-[#DAD6FF]">
            {source.inferredCodec}
          </span>
        )}

        {source.inferredSubtitles && (
          <span className="rounded-md border border-emerald-400/30 bg-emerald-400/10 px-2 py-0.5 text-emerald-200">
            {source.inferredSubtitles}
          </span>
        )}

        {source.inferredContainer && (
          <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#949CAF]">
            {source.inferredContainer}
          </span>
        )}

        <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#E2E8F0]">
          <HardDrive className="h-3 w-3 text-[#786BFF]" aria-hidden="true" />
          {source.item.size}
        </span>

        <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#AAB5CA]">
          <Users className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
          <span className="text-emerald-300">{source.seeders ?? 0} seeders</span>
          <span>·</span>
          <span className="text-amber-200">{source.leechers ?? 0} leechers</span>
        </span>

        <span
          className={`inline-flex items-center gap-1 rounded-md border px-2 py-0.5 font-medium ${
            isMultiMirror
              ? "border-[#786BFF]/40 bg-[#786BFF]/15 text-[#DAD6FF]"
              : "border-white/10 bg-[#090B12] text-[#C8D0E2]"
          }`}
        >
          <Globe className="h-3 w-3 text-[#AFA6FF]" aria-hidden="true" />
          {source.providerName}
        </span>
      </div>
    );
  };

  const renderMagnet = (source: MediaSource, showInitially = false) => {
    const magnet = magnetOf(source);
    const key = sourceKey(source);
    const isOpen = showInitially || expandedSource === key;
    const isCopied = copiedKey === key;

    if (!magnet) {
      return (
        <div className="rounded-lg border border-amber-400/25 bg-amber-400/5 px-3 py-2 text-xs text-amber-200/90">
          Magnet link unavailable for this release. A torrent-file URL is not a magnet link.
        </div>
      );
    }

    return (
      <div className="space-y-2.5">
        {!isOpen && (
          <button
            type="button"
            onClick={() => setExpandedSource(key)}
            className="inline-flex items-center gap-1.5 rounded-lg border border-[#786BFF]/30 bg-[#786BFF]/10 px-3 py-1.5 text-xs font-medium text-[#CFC9FF] transition-colors hover:bg-[#786BFF]/20 hover:text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
          >
            <Magnet className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Show magnet URI</span>
          </button>
        )}
        {isOpen && (
          <div className="space-y-2.5">
            <code className="block max-h-28 overflow-auto break-all rounded-lg border border-white/10 bg-[#090B12] p-3 font-mono text-[11px] leading-relaxed text-[#C8D0E2] select-all">
              {magnet}
            </code>
            <div className="flex flex-wrap items-center gap-2">
              <button
                type="button"
                onClick={() => void handleCopyMagnet(source)}
                className="inline-flex items-center gap-2 rounded-lg border border-[#786BFF]/40 bg-[#786BFF]/15 px-3.5 py-2 text-xs font-semibold text-[#F8FAFC] transition-colors hover:bg-[#786BFF]/25 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                {isCopied ? (
                  <>
                    <Check className="h-3.5 w-3.5 text-emerald-300" aria-hidden="true" />
                    <span>Copied magnet link</span>
                  </>
                ) : (
                  <>
                    <Copy className="h-3.5 w-3.5 text-[#CFC9FF]" aria-hidden="true" />
                    <span>Copy magnet link</span>
                  </>
                )}
              </button>
            </div>
          </div>
        )}
      </div>
    );
  };

  return (
    <div className="space-y-6 pb-12">
      {/* Show Header */}
      <section className="relative overflow-hidden rounded-2xl border border-white/10 bg-[#111522] p-5 sm:p-7">
        <PosterArtwork
          title={media.title}
          year={media.year}
          mediaType={media.mediaType}
          variant="backdrop"
          posterUrl={media.posterUrl}
          backdropUrl={media.backdropUrl}
        />
        <div className="relative z-10 flex flex-col gap-5 sm:flex-row sm:items-start">
          <div className="hidden aspect-[2/3] w-28 shrink-0 overflow-hidden rounded-xl border border-white/15 bg-[#090B12] shadow-xl sm:block md:w-32">
            <PosterArtwork
              title={media.title}
              year={media.year}
              mediaType={media.mediaType}
              posterUrl={media.posterUrl}
            />
          </div>

          <div className="min-w-0 flex-1 space-y-3">
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-[#AAB5CA]">
              <span className="rounded-md border border-[#786BFF]/35 bg-[#786BFF]/15 px-2.5 py-1 font-medium text-[#DAD6FF]">
                {media.metadataProvider || media.providerName}
              </span>
              {media.year && (
                <span className="rounded-md border border-white/10 bg-black/45 px-2.5 py-1 font-mono text-[#E2E8F0]">
                  {media.year}
                </span>
              )}
              {media.metadata?.imdb_id && (
                <span className="rounded-md border border-amber-400/30 bg-amber-400/10 px-2.5 py-1 font-mono text-amber-200">
                  IMDb {media.metadata.imdb_id}
                </span>
              )}
              <span className="rounded-md border border-white/10 bg-black/45 px-2.5 py-1">
                {media.metadata?.media_type === "tv"
                  ? "TV Series"
                  : media.mediaType === "series"
                    ? "Anime Series"
                    : "Release"}
              </span>
              <span className="rounded-md border border-[#786BFF]/35 bg-[#786BFF]/15 px-2.5 py-1 font-medium text-[#DAD6FF]">
                {(catalog.seasons.length || media.officialSeasons?.length || 0) > 0
                  ? `${catalog.seasons.length || media.officialSeasons?.length} ${
                      (catalog.seasons.length || media.officialSeasons?.length) === 1
                        ? "Season"
                        : "Seasons"
                    }`
                  : "Season Unspecified"}
              </span>
              <span className="rounded-md border border-white/10 bg-black/45 px-2.5 py-1">
                {catalog.episodes.length} {catalog.episodes.length === 1 ? "Episode" : "Episodes"}
              </span>
              {catalog.batches.length > 0 && (
                <span className="rounded-md border border-[#38BDF8]/30 bg-[#38BDF8]/10 px-2.5 py-1 text-[#7DD3FC]">
                  {catalog.batches.length} {catalog.batches.length === 1 ? "Batch Pack" : "Batch Packs"}
                </span>
              )}
            </div>

            <div className="space-y-1">
              <h2 className="break-words text-2xl font-bold tracking-tight text-white sm:text-3xl">
                {media.title}
              </h2>
              {media.alternateTitles && media.alternateTitles.length > 0 && (
                <p className="line-clamp-1 text-xs text-[#949CAF]">
                  Also known as:{" "}
                  <span className="text-[#C8D0E2]">
                    {media.alternateTitles.slice(0, 3).join(" · ")}
                  </span>
                </p>
              )}
            </div>

            {media.genres && media.genres.length > 0 && (
              <div className="flex flex-wrap items-center gap-1.5">
                {media.genres.slice(0, 6).map((genre) => (
                  <span
                    key={genre}
                    className="rounded-full border border-white/10 bg-black/40 px-2.5 py-0.5 text-[11px] text-[#C8D0E2]"
                  >
                    {genre}
                  </span>
                ))}
              </div>
            )}

            <p className="max-w-3xl line-clamp-3 text-xs leading-relaxed text-[#AAB5CA] sm:text-sm">
              {media.synopsis ||
                "Select a season and episode below to inspect matching releases across multiple providers and copy a verified magnet URI."}
            </p>

            <div className="flex flex-wrap gap-2.5 pt-1">
              <button
                type="button"
                onClick={onBack}
                className="inline-flex items-center gap-2 rounded-xl border border-white/15 bg-[#191E2E] px-3.5 py-2 text-xs font-medium text-white transition-colors hover:bg-[#232A40] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <ArrowLeft className="h-4 w-4" aria-hidden="true" />
                <span>Back to results</span>
              </button>

              <button
                type="button"
                onClick={() => onToggleLibrary(media)}
                className="inline-flex items-center gap-2 rounded-xl border border-white/15 bg-[#191E2E] px-3.5 py-2 text-xs font-medium text-white transition-colors hover:bg-[#232A40] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                {isSaved ? (
                  <Check className="h-4 w-4 text-emerald-400" aria-hidden="true" />
                ) : (
                  <Bookmark className="h-4 w-4" aria-hidden="true" />
                )}
                <span>{isSaved ? "Saved to Library" : "Add to Library"}</span>
              </button>

              {isBatchOnlyShow && (
                <button
                  type="button"
                  onClick={() =>
                    batchSectionRef.current?.scrollIntoView({ behavior: "smooth", block: "start" })
                  }
                  className="inline-flex items-center gap-2 rounded-xl border border-[#786BFF]/40 bg-[#786BFF]/15 px-3.5 py-2 text-xs font-semibold text-[#DAD6FF] transition-colors hover:bg-[#786BFF]/25 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
                >
                  <Layers3 className="h-4 w-4" aria-hidden="true" />
                  <span>View Batch Packs ({catalog.batches.length})</span>
                </button>
              )}
            </div>
          </div>
        </div>
      </section>

      {/* Season & Episode Browser */}
      <section className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="text-lg font-semibold text-white">Episodes & Multi-Provider Discovery</h3>
            <p className="mt-1 text-xs text-[#AAB5CA]">
              {catalog.episodes.length} distinct{" "}
              {catalog.episodes.length === 1 ? "episode" : "episodes"} across{" "}
              {catalog.seasons.length} identified{" "}
              {catalog.seasons.length === 1 ? "season" : "seasons"} ({media.sources.length} deduplicated releases).
            </p>
          </div>

          <div className="flex flex-wrap items-center gap-2">
            {onChangeSourceFilters && (
              <div className="flex items-center gap-1.5 rounded-xl border border-white/10 bg-[#090B12] px-2.5 py-1.5 text-xs">
                <Filter className="h-3.5 w-3.5 text-[#786BFF]" aria-hidden="true" />
                <label htmlFor="nyaa-category-filter" className="text-[#949CAF]">
                  Category:
                </label>
                <select
                  id="nyaa-category-filter"
                  aria-label="Filter torrent category"
                  value={sourceFilters?.category || "1_0"}
                  onChange={(e) =>
                    onChangeSourceFilters({
                      ...(sourceFilters || {}),
                      category: e.target.value,
                    })
                  }
                  className="bg-transparent font-medium text-white focus:outline-none"
                >
                  <option value="1_0" className="bg-[#090B12]">
                    All Anime (Sub + Raw + Non-EN)
                  </option>
                  <option value="1_2" className="bg-[#090B12]">
                    Anime — English-Translated (1_2)
                  </option>
                  <option value="1_4" className="bg-[#090B12]">
                    Anime — Raw / Japanese (1_4)
                  </option>
                  <option value="1_3" className="bg-[#090B12]">
                    Anime — Non-English Sub (1_3)
                  </option>
                  <option value="4_0" className="bg-[#090B12]">
                    Live Action TV (4_0)
                  </option>
                  <option value="0_0" className="bg-[#090B12]">
                    All Categories (0_0)
                  </option>
                </select>
              </div>
            )}

            {loadingEpisodes ? (
              <span
                role="status"
                className="inline-flex items-center gap-2 rounded-lg border border-[#786BFF]/30 bg-[#786BFF]/10 px-3 py-1.5 text-xs font-medium text-[#CFC9FF]"
              >
                <RefreshCw className="h-3.5 w-3.5 animate-spin" aria-hidden="true" />
                <span>Discovering episodes...</span>
              </span>
            ) : onLoadEpisodes ? (
              <button
                type="button"
                onClick={onLoadEpisodes}
                className="inline-flex items-center gap-2 rounded-xl border border-white/10 bg-[#191E2E] px-3 py-2 text-xs font-medium text-[#D5D7E3] transition-colors hover:bg-[#232A40] hover:text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <RefreshCw className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Refresh sources</span>
              </button>
            ) : null}
          </div>
        </div>

        {/* Per-Provider Retrieval Status Pills */}
        {media.providerStatuses && media.providerStatuses.length > 0 && (
          <div className="flex flex-wrap items-center gap-2 rounded-xl border border-white/[0.06] bg-[#0B0E18] px-3 py-2 text-[11px]">
            <span className="font-semibold uppercase tracking-wider text-[#949CAF]">
              Source Providers:
            </span>
            {media.providerStatuses.map((st) => (
              <span
                key={st.provider_id}
                title={st.error || `${st.provider_name}: ${st.items_found} releases`}
                className={`inline-flex items-center gap-1.5 rounded-md border px-2 py-0.5 font-mono ${
                  st.status === "ok"
                    ? st.items_found > 0
                      ? "border-emerald-400/30 bg-emerald-400/10 text-emerald-200"
                      : "border-white/10 bg-white/5 text-[#949CAF]"
                    : "border-amber-400/30 bg-amber-400/10 text-amber-200"
                }`}
              >
                <span>{st.provider_name}:</span>
                <strong>{st.status === "ok" ? `${st.items_found} found` : st.status}</strong>
              </span>
            ))}
          </div>
        )}

        {episodeError && (
          <div
            role="alert"
            className="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-amber-400/25 bg-amber-400/10 p-3 text-xs text-amber-100"
          >
            <div className="flex items-center gap-2">
              <AlertTriangle className="h-4 w-4 shrink-0 text-amber-300" aria-hidden="true" />
              <span>
                {episodeError} — Previously discovered episodes and sources remain available below.
              </span>
            </div>
            {onLoadEpisodes && (
              <button
                type="button"
                onClick={onLoadEpisodes}
                className="rounded-lg border border-amber-300/35 px-2.5 py-1 font-semibold text-amber-100 hover:bg-amber-400/15"
              >
                Retry
              </button>
            )}
          </div>
        )}

        {moreEpisodesAvailable && (
          <div className="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-white/10 bg-[#191E2E] p-3.5">
            <p className="flex-1 text-xs text-[#AAB5CA]">
              Additional search pages exist for this title. Load more pages on demand to discover
              further episodes or seasons.
            </p>
            <button
              type="button"
              disabled={loadingEpisodes}
              onClick={onLoadMoreEpisodes}
              className="rounded-xl border border-[#786BFF]/40 bg-[#786BFF]/15 px-3.5 py-2 text-xs font-semibold text-[#DAD6FF] transition-colors hover:bg-[#786BFF]/25 disabled:opacity-50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              Load more episodes
            </button>
          </div>
        )}

        {/* Season Selector */}
        {catalog.seasons.length > 0 ? (
          <div role="group" aria-label="Choose season" className="flex flex-wrap items-center gap-2">
            {catalog.seasons.map((value) => {
              const isSelected = currentSeason === value;
              return (
                <button
                  key={value}
                  type="button"
                  onClick={() => {
                    setSeason(value);
                    setEpisode(null);
                    setExpandedSource(null);
                  }}
                  aria-pressed={isSelected}
                  className={`rounded-xl px-4 py-2 text-xs font-semibold transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                    isSelected
                      ? "border border-[#786BFF]/70 bg-[#786BFF]/25 text-white shadow-sm"
                      : "border border-white/10 bg-[#191E2E] text-[#AAB5CA] hover:bg-[#232A40] hover:text-white"
                  }`}
                >
                  Season {value}
                </button>
              );
            })}
          </div>
        ) : (
          <div className="inline-flex items-center gap-2 rounded-xl border border-white/10 bg-[#191E2E] px-3.5 py-2 text-xs text-[#AAB5CA]">
            <span>Season Unspecified / Multi-Season — No explicit season number in release titles</span>
          </div>
        )}

        {/* Episode Grid */}
        {loadingEpisodes && visibleEpisodes.length === 0 && catalog.batches.length === 0 ? (
          <div
            aria-busy="true"
            className="grid grid-cols-3 gap-2.5 sm:grid-cols-5 md:grid-cols-6 lg:grid-cols-8 xl:grid-cols-10"
          >
            {Array.from({ length: 10 }, (_, idx) => (
              <div key={idx} className="skeleton-shimmer h-16 rounded-xl" />
            ))}
          </div>
        ) : visibleEpisodes.length > 0 ? (
          <div
            role="group"
            aria-label="Choose episode"
            className="grid max-h-[340px] grid-cols-3 gap-2.5 overflow-y-auto pr-1 sm:grid-cols-5 md:grid-cols-6 lg:grid-cols-8 xl:grid-cols-10"
          >
            {visibleEpisodes.map((entry) => {
              const isSelected = selectedEpisode?.number === entry.number;
              return (
                <button
                  type="button"
                  key={entry.number}
                  onClick={() => handleSelectEpisode(entry.number)}
                  aria-label={`Episode ${entry.number}${entry.title ? `: ${entry.title}` : ""}`}
                  title={entry.title ? `Episode ${entry.number}: ${entry.title}` : `Episode ${entry.number}`}
                  aria-pressed={isSelected}
                  className={`flex min-h-[74px] flex-col items-center justify-center gap-0.5 rounded-xl border px-2 py-2 text-xs transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                    isSelected
                      ? "border-[#786BFF] bg-[#786BFF]/25 text-white shadow-sm"
                      : "border-white/10 bg-[#191E2E] text-[#D2D7E4] hover:border-white/25 hover:bg-[#232A40]"
                  }`}
                >
                  <span className="text-[10px] font-medium uppercase tracking-wider text-[#AAB5CA]">
                    EP {String(entry.number).padStart(2, "0")}
                  </span>
                  {entry.title ? (
                    <span className="line-clamp-1 max-w-full text-[11px] font-semibold text-white">
                      {entry.title}
                    </span>
                  ) : (
                    <strong className="font-mono text-base font-bold">
                      {String(entry.number).padStart(2, "0")}
                    </strong>
                  )}
                  <span className="text-[10px] text-[#AAB5CA]">
                    {entry.sources.length} {entry.sources.length === 1 ? "source" : "sources"}
                  </span>
                </button>
              );
            })}
          </div>
        ) : (
          <div className="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-dashed border-white/15 bg-[#0C101C] p-5 text-xs text-[#AAB5CA]">
            <div className="flex items-center gap-3">
              <Search className="h-5 w-5 shrink-0 text-[#786BFF]" aria-hidden="true" />
              <span>
                {availableBatches.length > 0
                  ? "This show has batch or full-season packs rather than individually numbered episode torrents. Inspect the batch releases below to copy a magnet link."
                  : "No individually numbered episodes were identified for this season. Check batch or unclassified releases below, or refresh sources."}
              </span>
            </div>
          </div>
        )}
      </section>

      {/* Episode Magnet Sources Panel */}
      {(visibleEpisodes.length > 0 || selectedEpisode !== null) && (
        <section
          aria-live="polite"
          className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6"
        >
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div>
              <h3 className="text-lg font-semibold text-white">
                {selectedEpisode
                  ? `Season ${selectedEpisode.season} · Episode ${selectedEpisode.number}${
                      selectedEpisode.title ? ` — ${selectedEpisode.title}` : ""
                    } — Magnet sources`
                  : "Select an episode"}
              </h3>
              <p className="mt-1 text-xs text-[#AAB5CA]">
                {selectedEpisode
                  ? `${sortedSources.length} matching ${sortedSources.length === 1 ? "release" : "releases"} for Episode ${String(selectedEpisode.number).padStart(2, "0")}. Compare resolution, codec, and seeders, then copy your preferred magnet URI.`
                  : "Source links appear here after you choose an episode above."}
              </p>
            </div>

            {selectedEpisode && sortedSources.length > 1 && (
              <div className="flex items-center gap-2">
                <ArrowUpDown className="h-3.5 w-3.5 text-[#786BFF]" aria-hidden="true" />
                <label htmlFor="episode-source-sort" className="text-xs text-[#AAB5CA]">
                  Sort by
                </label>
                <select
                  id="episode-source-sort"
                  aria-label="Sort episode sources"
                  value={sortBy}
                  onChange={(e) => setSortBy(e.target.value as EpisodeSourceSort)}
                  className="h-8 rounded-lg border border-white/15 bg-[#090B12] px-2.5 text-xs text-white focus:border-[#786BFF] focus:outline-none"
                >
                  <option value="seeders">Seeders (High → Low)</option>
                  <option value="quality">Quality / Resolution</option>
                  <option value="size">File Size</option>
                </select>
              </div>
            )}
          </div>

          {selectedEpisode && (
            <div className="space-y-3">
              {sortedSources.map((source) => (
                <article
                  key={sourceKey(source)}
                  className="space-y-3 rounded-xl border border-white/[0.08] bg-[#191E2E] p-4"
                >
                  <div className="flex items-start gap-3">
                    <Magnet className="mt-0.5 h-4 w-4 shrink-0 text-[#AFA6FF]" aria-hidden="true" />
                    <div className="min-w-0 flex-1">
                      <p className="break-words text-sm font-semibold leading-snug text-white">
                        {source.sourceTitle}
                      </p>
                      {renderSourceMetadataBadges(source)}
                    </div>
                  </div>
                  {renderMagnet(source, true)}
                </article>
              ))}
            </div>
          )}
        </section>
      )}

      {/* Batch & Season Packs Panel */}
      {availableBatches.length > 0 && (
        <section
          ref={batchSectionRef}
          className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6"
        >
          <div className="flex items-center gap-3">
            <Layers3 className="h-5 w-5 shrink-0 text-[#AFA6FF]" aria-hidden="true" />
            <div>
              <h3 className="text-lg font-semibold text-white">Batch & season packs</h3>
              <p className="mt-1 text-xs text-[#AAB5CA]">
                Multi-episode ranges, full-season batches, and complete-series packs are kept
                separate from single episodes. One batch torrent provides one magnet URI; individual
                episode contents are not fabricated.
              </p>
            </div>
          </div>

          <div className="space-y-3">
            {availableBatches.map((batch) => {
              const badge = batchBadgeStyle(batch.batchKind);
              return (
                <article
                  key={sourceKey(batch.source)}
                  className="space-y-3 rounded-xl border border-white/10 bg-[#191E2E] p-4"
                >
                  <div className="flex flex-wrap items-start justify-between gap-2">
                    <div className="min-w-0 flex-1">
                      <div className="flex flex-wrap items-center gap-2">
                        <span
                          className={`rounded-md border px-2 py-0.5 text-[11px] font-semibold ${badge.className}`}
                        >
                          {badge.label}
                        </span>
                        <span className="font-mono text-xs text-[#AAB5CA]">
                          {batch.batchLabel}
                        </span>
                      </div>

                      <p className="mt-1.5 break-words text-sm font-semibold text-white">
                        {batch.source.sourceTitle}
                      </p>

                      <p className="mt-1 text-xs text-[#AAB5CA]">
                        {batch.firstEpisode !== null && batch.lastEpisode !== null
                          ? `Episodes ${batch.firstEpisode}–${batch.lastEpisode} (inferred from title)`
                          : "Exact episode list unverified until torrent metadata is inspected"}
                      </p>

                      {renderSourceMetadataBadges(batch.source)}
                    </div>
                  </div>
                  {renderMagnet(batch.source, isBatchOnlyShow)}
                </article>
              );
            })}
          </div>
        </section>
      )}

      {/* Unclassified Releases Panel */}
      {catalog.other.length > 0 && (
        <details className="rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6">
          <summary className="cursor-pointer text-sm font-semibold text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]">
            Unclassified releases ({catalog.other.length}) — specials, OVAs, or unnumbered sources
          </summary>
          <p className="mt-1.5 text-xs text-[#AAB5CA]">
            These releases did not match a standard season/episode number and are preserved here
            instead of fabricating episode entries.
          </p>
          <div className="mt-4 space-y-3">
            {catalog.other.map((source) => (
              <article
                key={sourceKey(source)}
                className="space-y-2.5 rounded-xl border border-white/10 bg-[#191E2E] p-4"
              >
                <p className="break-words text-sm font-medium text-white">{source.sourceTitle}</p>
                {renderSourceMetadataBadges(source)}
                {renderMagnet(source)}
              </article>
            ))}
          </div>
        </details>
      )}
    </div>
  );
}
