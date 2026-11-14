import {
  ArrowLeft,
  Bookmark,
  Check,
  Globe,
  HardDrive,
  Magnet,
  Play,
  ShieldCheck,
  Users,
} from "lucide-react";
import { m } from "motion/react";
import { useMemo, useRef, useState } from "react";
import { QUALITY_RANK } from "../lib/mediaAdapter";
import { EASE_CINEMA, PAGE_VARIANTS } from "../lib/motion";
import type {
  EngineStatus,
  MediaEntry,
  MediaSource,
  QualityTier,
  SourceSortKey,
  StreamSessionState,
} from "../lib/types";
import { IntezamFlowPanel } from "./IntezamFlowPanel";
import { PosterArtwork } from "./PosterArtwork";
import { StatusBadge } from "./StatusBadge";

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
}

const QUALITY_ORDER: QualityTier[] = ["4K", "1080p", "720p", "Other"];

export function MediaDetailsView({
  media,
  isSaved,
  engineStatus,
  streamState,
  verifyingId,
  strictCacheBudget,
  onBack,
  onToggleLibrary,
  onVerifySource,
  onCopyUri,
  onLaunchIntezam,
  onStopIntezam,
  onOpenExternal,
}: MediaDetailsViewProps) {
  const sourcesRef = useRef<HTMLDivElement | null>(null);
  const [sortBy, setSortBy] = useState<SourceSortKey>("quality");
  const [selectedSourceId, setSelectedSourceId] = useState<number | null>(() => {
    const firstValidMagnet = media.sources.find((s) => s.item.is_valid && s.isMagnet);
    const firstValid = media.sources.find((s) => s.item.is_valid);
    return (firstValidMagnet ?? firstValid ?? media.sources[0])?.item.id ?? null;
  });

  const sortedSources = useMemo(() => {
    return [...media.sources].sort((a, b) => {
      if (sortBy === "quality") {
        const qDiff = QUALITY_RANK[b.qualityTier] - QUALITY_RANK[a.qualityTier];
        if (qDiff !== 0) return qDiff;
        return Number(b.item.is_valid) - Number(a.item.is_valid);
      }
      if (sortBy === "size") return b.sizeBytes - a.sizeBytes;
      if (sortBy === "peers") return (b.seeders ?? -1) - (a.seeders ?? -1);
      return Number(b.item.is_valid) - Number(a.item.is_valid);
    });
  }, [media.sources, sortBy]);

  const groupedSources = useMemo(() => {
    const map = new Map<QualityTier, MediaSource[]>();
    for (const tier of QUALITY_ORDER) map.set(tier, []);
    for (const src of sortedSources) {
      map.get(src.qualityTier)?.push(src);
    }
    return QUALITY_ORDER.filter((tier) => (map.get(tier)?.length ?? 0) > 0).map((tier) => ({
      tier,
      sources: map.get(tier)!,
    }));
  }, [sortedSources]);

  const selectedSource =
    media.sources.find((s) => s.item.id === selectedSourceId) ?? sortedSources[0] ?? null;

  const handlePlayBest = () => {
    if (selectedSource) {
      sourcesRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
      if (
        selectedSource.isMagnet &&
        selectedSource.item.is_valid &&
        engineStatus?.intezam_binary_available
      ) {
        onLaunchIntezam(selectedSource);
      }
    }
  };

  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="space-y-6 pb-10"
    >
      {/* Cinematic Backdrop Header */}
      <section className="relative overflow-hidden rounded-2xl border border-white/[0.08] bg-[#111522] p-5 sm:p-6">
        <PosterArtwork
          title={media.title}
          year={media.year}
          quality={media.bestQuality}
          mediaType={media.mediaType}
          variant="backdrop"
        />

        <div className="relative z-10 flex flex-col gap-5 md:flex-row md:items-end">
          {/* Poster Thumbnail */}
          <m.div
            initial={{ opacity: 0, scale: 0.96 }}
            animate={{ opacity: 1, scale: 1 }}
            transition={{ duration: 0.3, ease: EASE_CINEMA }}
            className="hidden aspect-[2/3] w-[136px] shrink-0 overflow-hidden rounded-xl border border-white/15 shadow-2xl md:block"
          >
            <PosterArtwork
              title={media.title}
              year={media.year}
              quality={media.bestQuality}
              mediaType={media.mediaType}
            />
          </m.div>

          {/* Title & Genuine Metadata */}
          <div className="flex-1 space-y-4">
            <m.div
              initial={{ opacity: 0, y: 10 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ duration: 0.28, ease: EASE_CINEMA }}
              className="space-y-2"
            >
              <div className="flex flex-wrap items-center gap-2 text-[11px]">
                <span className="rounded-md border border-white/15 bg-black/40 px-2 py-0.5 font-mono text-[#949CAF]">
                  {media.providerName}
                </span>
                {media.year && (
                  <span className="rounded-md border border-white/15 bg-black/40 px-2 py-0.5 font-mono text-[#F8FAFC]">
                    {media.year}
                  </span>
                )}
                {media.seasonEpisode && (
                  <span className="rounded-md border border-[#786BFF]/35 bg-[#786BFF]/15 px-2 py-0.5 font-mono text-[#F8FAFC]">
                    {media.seasonEpisode}
                  </span>
                )}
                {media.bestQuality !== "Other" && (
                  <span className="rounded-md border border-[#38BDF8]/30 bg-[#38BDF8]/10 px-2 py-0.5 font-mono text-[#38BDF8]">
                    {media.bestQuality} (inferred)
                  </span>
                )}
              </div>

              <h2 className="text-2xl font-bold tracking-tight text-[#F8FAFC] sm:text-3xl">
                {media.title}
              </h2>
            </m.div>

            <m.p
              initial={{ opacity: 0, y: 8 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ duration: 0.28, delay: 0.06, ease: EASE_CINEMA }}
              className="max-w-2xl text-xs leading-relaxed text-[#949CAF] sm:text-[13px]"
            >
              {media.sources.length} extracted{" "}
              {media.sources.length === 1 ? "source" : "sources"} · {media.verifiedCount} passed
              initial URI validation
              {media.largestSizeLabel !== "N/A" ? ` · Up to ${media.largestSizeLabel}` : ""}
              {media.latestDate !== "N/A" ? ` · Latest date ${media.latestDate}` : ""}. Metadata
              tags such as resolution or codec are inferred from source titles until stream
              metadata is retrieved.
            </m.p>

            {/* Primary Actions */}
            <m.div
              initial={{ opacity: 0, y: 8 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ duration: 0.28, delay: 0.1, ease: EASE_CINEMA }}
              className="flex flex-wrap items-center gap-2.5 pt-1"
            >
              <button
                type="button"
                onClick={handlePlayBest}
                className="inline-flex h-10 items-center gap-2 rounded-xl bg-gradient-to-r from-[#786BFF] to-[#5B8DEF] px-5 text-xs font-semibold text-[#F8FAFC] shadow-lg shadow-[#786BFF]/25 transition-transform hover:scale-[1.015] active:scale-[0.985] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <Play className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
                <span>
                  {selectedSource?.isMagnet && engineStatus?.intezam_binary_available
                    ? "Watch with Intezam"
                    : "View Sources"}
                </span>
              </button>

              <button
                type="button"
                onClick={() => onToggleLibrary(media)}
                className="inline-flex h-10 items-center gap-2 rounded-xl border border-white/15 bg-[#191E2E] px-4 text-xs font-medium text-[#F8FAFC] transition-colors hover:bg-[#232A40] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                {isSaved ? (
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

              <button
                type="button"
                onClick={onBack}
                className="inline-flex h-10 items-center gap-1.5 rounded-xl border border-white/10 bg-[#090B12]/70 px-3.5 text-xs font-medium text-[#949CAF] transition-colors hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <ArrowLeft className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Back</span>
              </button>
            </m.div>
          </div>
        </div>
      </section>

      {/* Intezam Flow Source Preparation Panel for Active Source */}
      {selectedSource && (
        <IntezamFlowPanel
          source={selectedSource}
          engineStatus={engineStatus}
          streamState={streamState}
          isVerifying={verifyingId === selectedSource.item.id}
          strictCacheBudget={strictCacheBudget}
          onVerify={onVerifySource}
          onCopyUri={onCopyUri}
          onLaunchIntezam={onLaunchIntezam}
          onStopIntezam={onStopIntezam}
          onOpenExternal={onOpenExternal}
        />
      )}

      {/* Stream Source Selection — Grouped by Quality */}
      <section ref={sourcesRef} className="space-y-5">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="text-base font-semibold tracking-tight text-[#F8FAFC]">
              Stream Sources
            </h3>
            <p className="mt-0.5 text-xs text-[#949CAF]">
              Select any source card to verify its URI or prepare Intezam Flow playback
            </p>
          </div>

          <div className="flex items-center gap-2">
            <span className="text-xs text-[#949CAF]">Sort sources</span>
            <select
              aria-label="Sort stream sources"
              value={sortBy}
              onChange={(e) => setSortBy(e.target.value as SourceSortKey)}
              className="h-8 rounded-lg border border-white/10 bg-[#111522] px-2.5 text-xs text-[#F8FAFC] focus:border-[#786BFF] focus:outline-none"
            >
              <option value="quality">Quality (Inferred)</option>
              <option value="size">File Size</option>
              <option value="peers">Seeders</option>
              <option value="verified">Verification Status</option>
            </select>
          </div>
        </div>

        {groupedSources.length === 0 ? (
          <div className="rounded-2xl border border-white/[0.06] bg-[#111522] p-8 text-center text-xs text-[#949CAF]">
            No streaming sources were found for this entry.
          </div>
        ) : (
          <div className="space-y-5">
            {groupedSources.map((group) => (
              <div key={group.tier} className="space-y-2.5">
                <div className="flex items-center gap-2">
                  <span className="rounded-md border border-white/10 bg-[#191E2E] px-2 py-0.5 font-mono text-[11px] font-semibold text-[#F8FAFC]">
                    {group.tier === "Other" ? "Other / Unknown Resolution" : `${group.tier} (Inferred)`}
                  </span>
                  <span className="font-mono text-[11px] text-[#949CAF]">
                    {group.sources.length} {group.sources.length === 1 ? "source" : "sources"}
                  </span>
                </div>

                <div className="grid gap-2.5 md:grid-cols-2">
                  {group.sources.map((src) => {
                    const isSelected = selectedSource?.item.id === src.item.id;
                    return (
                      <m.button
                        key={src.item.id}
                        type="button"
                        layout="position"
                        onClick={() => setSelectedSourceId(src.item.id)}
                        className={`flex flex-col justify-between gap-3 rounded-xl border p-4 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                          isSelected
                            ? "border-[#786BFF]/60 bg-[#191E2E] shadow-[0_0_0_1px_rgba(120,107,255,0.25)]"
                            : "border-white/[0.07] bg-[#111522] hover:border-white/20 hover:bg-[#191E2E]/75"
                        }`}
                      >
                        <div className="flex items-start justify-between gap-3">
                          <div className="flex min-w-0 items-start gap-2.5">
                            <span
                              className={`mt-0.5 flex h-7 w-7 shrink-0 items-center justify-center rounded-lg border ${
                                src.isMagnet
                                  ? "border-[#38BDF8]/25 bg-[#38BDF8]/10 text-[#38BDF8]"
                                  : "border-[#786BFF]/25 bg-[#786BFF]/10 text-[#786BFF]"
                              }`}
                              aria-hidden="true"
                            >
                              {src.isMagnet ? (
                                <Magnet className="h-3.5 w-3.5" />
                              ) : (
                                <Globe className="h-3.5 w-3.5" />
                              )}
                            </span>
                            <div className="min-w-0">
                              <div className="line-clamp-2 text-[13px] font-medium text-[#F8FAFC]">
                                {src.sourceTitle}
                              </div>
                              <div className="mt-1 flex flex-wrap items-center gap-2 font-mono text-[10.5px] text-[#949CAF]">
                                <span>{src.providerName}</span>
                                {src.inferredCodec && (
                                  <span className="rounded bg-white/[0.06] px-1.5 py-0.5 text-[#F8FAFC]">
                                    {src.inferredCodec} (inferred)
                                  </span>
                                )}
                                {src.inferredContainer && (
                                  <span className="rounded bg-white/[0.06] px-1.5 py-0.5 text-[#F8FAFC]">
                                    {src.inferredContainer}
                                  </span>
                                )}
                              </div>
                            </div>
                          </div>

                          <StatusBadge
                            isValid={src.item.is_valid}
                            message={src.item.verification_msg}
                            isVerifying={verifyingId === src.item.id}
                          />
                        </div>

                        <div className="flex flex-wrap items-center justify-between gap-2 border-t border-white/[0.05] pt-2.5 font-mono text-[11px] text-[#949CAF]">
                          <div className="flex items-center gap-3">
                            <span className="inline-flex items-center gap-1 text-[#F8FAFC]">
                              <HardDrive className="h-3 w-3 text-[#786BFF]" aria-hidden="true" />
                              {src.item.size}
                            </span>
                            <span className="inline-flex items-center gap-1">
                              <Users className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
                              {src.item.stats}
                            </span>
                          </div>
                          <span className="inline-flex items-center gap-1 text-[10.5px]">
                            <ShieldCheck className="h-3 w-3" aria-hidden="true" />
                            {src.item.date}
                          </span>
                        </div>
                      </m.button>
                    );
                  })}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>
    </m.div>
  );
}
