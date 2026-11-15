import {
  AlertTriangle,
  Check,
  CheckCircle2,
  Copy,
  FileVideo,
  Globe,
  HardDrive,
  Info,
  Magnet,
  Play,
  RefreshCw,
  ShieldAlert,
  Sparkles,
  Users,
  XCircle,
} from "lucide-react";
import { useState } from "react";
import { formatSeasonLabel } from "../lib/episodeCatalog";
import type {
  EpisodeResolutionOptions,
  EpisodeResolutionResult,
  MediaSource,
  ResolvedFileCandidate,
} from "../lib/types";

export interface EpisodeResolutionViewState {
  status: "idle" | "loading" | "done" | "error";
  requestId?: string;
  result?: EpisodeResolutionResult;
  error?: string;
}

interface EpisodeSourcesPanelProps {
  season: number;
  episodeNumber: number;
  episodeTitle?: string;
  isMovie: boolean;
  resolutionState: EpisodeResolutionViewState;
  fallbackSources: MediaSource[];
  engineAvailable: boolean;
  onResolve: (options?: EpisodeResolutionOptions) => void;
  onCancelResolution: () => void;
  onPlayCandidate: (candidate: ResolvedFileCandidate) => void;
  onCopyMagnetText: (magnetUri: string, label?: string) => Promise<boolean>;
}

function statusBadge(candidate: ResolvedFileCandidate): {
  label: string;
  className: string;
} {
  if (candidate.status === "confirmed") {
    return {
      label: "Confirmed Target",
      className: "border-emerald-400/40 bg-emerald-400/15 text-emerald-200",
    };
  }
  if (candidate.status === "probable") {
    return {
      label: "Probable — Review",
      className: "border-amber-400/40 bg-amber-400/15 text-amber-200",
    };
  }
  if (candidate.status === "possible") {
    return {
      label: "Possible Match",
      className: "border-orange-400/35 bg-orange-400/10 text-orange-200",
    };
  }
  return {
    label: "Unresolved File",
    className: "border-slate-400/35 bg-slate-400/10 text-slate-300",
  };
}

export function EpisodeSourcesPanel({
  season,
  episodeNumber,
  episodeTitle,
  isMovie,
  resolutionState,
  fallbackSources,
  engineAvailable,
  onResolve,
  onCancelResolution,
  onPlayCandidate,
  onCopyMagnetText,
}: EpisodeSourcesPanelProps) {
  const [copiedId, setCopiedId] = useState<string | null>(null);
  const [expandedMagnetId, setExpandedMagnetId] = useState<string | null>(null);

  const res = resolutionState.result;
  const isLoading = resolutionState.status === "loading";

  const handleCopy = async (candidate: ResolvedFileCandidate) => {
    const ok = await onCopyMagnetText(candidate.magnet_uri, candidate.release_title);
    if (ok) {
      setCopiedId(candidate.candidate_id);
      window.setTimeout(() => {
        setCopiedId((prev) => (prev === candidate.candidate_id ? null : prev));
      }, 2000);
    }
  };

  const renderCandidateCard = (candidate: ResolvedFileCandidate, allowExplicitPlay = true) => {
    const badge = statusBadge(candidate);
    const hasPlayableTarget =
      allowExplicitPlay &&
      candidate.torrent_file_index !== null &&
      candidate.magnet_uri.toLowerCase().startsWith("magnet:?");
    const isCopied = copiedId === candidate.candidate_id;
    const isMagnetOpen = expandedMagnetId === candidate.candidate_id;
    const pct = Math.round(candidate.confidence * 100);

    return (
      <article
        key={candidate.candidate_id}
        data-testid={`resolved-candidate-${candidate.status}`}
        className={`space-y-3 rounded-xl border p-4 ${
          candidate.status === "confirmed"
            ? "border-emerald-400/25 bg-[#151D2B]"
            : candidate.status === "probable"
              ? "border-amber-400/25 bg-[#191E2E]"
              : "border-white/[0.08] bg-[#191E2E]"
        }`}
      >
        <div className="flex flex-wrap items-start justify-between gap-3">
          <div className="min-w-0 flex-1 space-y-1.5">
            <div className="flex flex-wrap items-center gap-2">
              <span
                className={`inline-flex items-center gap-1 rounded-md border px-2 py-0.5 text-[11px] font-semibold ${badge.className}`}
              >
                {candidate.status === "confirmed" ? (
                  <CheckCircle2 className="h-3 w-3" aria-hidden="true" />
                ) : (
                  <AlertTriangle className="h-3 w-3" aria-hidden="true" />
                )}
                {badge.label}
              </span>

              <span className="rounded-md border border-[#786BFF]/35 bg-[#786BFF]/10 px-2 py-0.5 font-mono text-[11px] text-[#DAD6FF]">
                {pct}% confidence
              </span>

              {candidate.is_batch_source && (
                <span className="rounded-md border border-[#38BDF8]/30 bg-[#38BDF8]/10 px-2 py-0.5 text-[11px] font-medium text-[#7DD3FC]">
                  From Batch Pack
                </span>
              )}

              {candidate.torrent_file_index !== null && (
                <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[11px] text-[#CBD5E1]">
                  File #{candidate.torrent_file_index}
                </span>
              )}
            </div>

            <p className="break-words text-sm font-semibold leading-snug text-white">
              {candidate.release_title}
            </p>

            {candidate.file_path && (
              <div className="flex items-center gap-1.5 rounded-lg border border-white/10 bg-[#090B12]/90 px-2.5 py-1.5 text-xs text-[#E2E8F0]">
                <FileVideo className="h-3.5 w-3.5 shrink-0 text-emerald-300" aria-hidden="true" />
                <span className="break-all font-mono">{candidate.file_path}</span>
              </div>
            )}
          </div>

          <div className="flex shrink-0 flex-wrap items-center gap-2">
            {hasPlayableTarget ? (
              <button
                type="button"
                onClick={() => onPlayCandidate(candidate)}
                className={`inline-flex items-center gap-1.5 rounded-xl px-3.5 py-2 text-xs font-semibold transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                  candidate.can_auto_play
                    ? "bg-emerald-500 text-[#062016] hover:bg-emerald-400"
                    : "border border-amber-400/40 bg-amber-400/15 text-amber-100 hover:bg-amber-400/25"
                }`}
              >
                <Play className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
                <span>
                  {candidate.can_auto_play
                    ? engineAvailable
                      ? "Play Episode"
                      : "Play Target"
                    : "Play Selected File"}
                </span>
              </button>
            ) : (
              <span className="inline-flex items-center gap-1.5 rounded-xl border border-white/10 bg-[#090B12] px-3 py-2 text-[11px] text-[#949CAF]">
                <ShieldAlert className="h-3.5 w-3.5 text-amber-300" aria-hidden="true" />
                Unverified File Target
              </span>
            )}
          </div>
        </div>

        {/* Metadata Badges */}
        <div className="flex flex-wrap items-center gap-1.5 text-[11px]">
          {candidate.resolution && (
            <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#E2E8F0]">
              {candidate.resolution}
            </span>
          )}
          {candidate.codec && (
            <span className="rounded-md border border-[#786BFF]/30 bg-[#786BFF]/10 px-2 py-0.5 font-mono text-[#DAD6FF]">
              {candidate.codec}
            </span>
          )}
          {candidate.subtitle_lang && (
            <span className="rounded-md border border-emerald-400/30 bg-emerald-400/10 px-2 py-0.5 text-emerald-200">
              {candidate.subtitle_lang}
            </span>
          )}
          {candidate.file_size_label && (
            <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#E2E8F0]">
              <HardDrive className="h-3 w-3 text-[#786BFF]" aria-hidden="true" />
              {candidate.file_size_label}
            </span>
          )}
          <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-[#AAB5CA]">
            <Users className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
            <span className="text-emerald-300">{candidate.seeders} seeders</span>
            <span>·</span>
            <span className="text-amber-200">{candidate.leechers} leechers</span>
          </span>
          {candidate.provider_name && (
            <span className="inline-flex items-center gap-1 rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 text-[#C8D0E2]">
              <Globe className="h-3 w-3 text-[#AFA6FF]" aria-hidden="true" />
              {candidate.provider_name}
            </span>
          )}
        </div>

        {/* Evidence & Contradictions */}
        {(candidate.evidence.length > 0 || candidate.contradictions.length > 0) && (
          <div className="space-y-1.5 border-t border-white/[0.06] pt-2.5 text-[11px]">
            {candidate.evidence.length > 0 && (
              <div className="flex flex-wrap items-center gap-1.5">
                <span className="text-[#949CAF]">Evidence:</span>
                {candidate.evidence.map((ev) => (
                  <span
                    key={ev}
                    className="rounded border border-emerald-400/20 bg-emerald-400/5 px-1.5 py-0.5 text-emerald-200/90"
                  >
                    {ev}
                  </span>
                ))}
              </div>
            )}
            {candidate.contradictions.length > 0 && (
              <div className="flex flex-wrap items-center gap-1.5 text-amber-200">
                <AlertTriangle className="h-3 w-3 shrink-0" aria-hidden="true" />
                <span>{candidate.contradictions.join(" · ")}</span>
              </div>
            )}
          </div>
        )}

        {/* Magnet inspection / Copy actions */}
        {candidate.magnet_uri && (
          <div className="flex flex-wrap items-center gap-2 pt-0.5">
            <button
              type="button"
              onClick={() =>
                setExpandedMagnetId((prev) =>
                  prev === candidate.candidate_id ? null : candidate.candidate_id,
                )
              }
              className="inline-flex items-center gap-1.5 rounded-lg border border-[#786BFF]/30 bg-[#786BFF]/10 px-2.5 py-1 text-[11px] font-medium text-[#CFC9FF] transition-colors hover:bg-[#786BFF]/20 hover:text-white focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <Magnet className="h-3 w-3" aria-hidden="true" />
              <span>{isMagnetOpen ? "Hide magnet URI" : "Show magnet URI"}</span>
            </button>

            <button
              type="button"
              onClick={() => void handleCopy(candidate)}
              className="inline-flex items-center gap-1.5 rounded-lg border border-white/10 bg-[#090B12] px-2.5 py-1 text-[11px] font-medium text-[#E2E8F0] transition-colors hover:bg-[#232A40] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              {isCopied ? (
                <>
                  <Check className="h-3 w-3 text-emerald-300" aria-hidden="true" />
                  <span>Copied magnet</span>
                </>
              ) : (
                <>
                  <Copy className="h-3 w-3 text-[#CFC9FF]" aria-hidden="true" />
                  <span>Copy magnet URI</span>
                </>
              )}
            </button>
          </div>
        )}

        {isMagnetOpen && candidate.magnet_uri && (
          <code className="block max-h-24 overflow-auto break-all rounded-lg border border-white/10 bg-[#090B12] p-2.5 font-mono text-[11px] leading-relaxed text-[#C8D0E2] select-all">
            {candidate.magnet_uri}
          </code>
        )}
      </article>
    );
  };

  return (
    <section
      aria-live="polite"
      className="space-y-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4 sm:p-6"
    >
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div>
          <h3 className="text-lg font-semibold text-white">
            {isMovie
              ? "Verified Feature Film File Targets"
              : `${formatSeasonLabel(season)} · Episode ${episodeNumber}${
                  episodeTitle ? ` — ${episodeTitle}` : ""
                } — Exact File Resolution`}
          </h3>
          <p className="mt-1 text-xs text-[#AAB5CA]">
            {res
              ? `${res.candidates.length} verified ${
                  res.candidates.length === 1 ? "file" : "files"
                } identified cleanly (${res.rejected_count} mismatched rejected, ${
                  res.unresolved_candidates.length
                } unresolved). Plays the exact resolved torrent file index.`
              : "Inspects candidate torrents and multi-file season packs without downloading video data to target this exact episode."}
          </p>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          {isLoading ? (
            <button
              type="button"
              onClick={onCancelResolution}
              className="inline-flex items-center gap-1.5 rounded-xl border border-rose-400/35 bg-rose-500/15 px-3 py-1.5 text-xs font-medium text-rose-200 transition-colors hover:bg-rose-500/25 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <XCircle className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Cancel lookup</span>
            </button>
          ) : (
            <>
              <button
                type="button"
                onClick={() => onResolve({ force_refresh: true })}
                className="inline-flex items-center gap-1.5 rounded-xl border border-white/15 bg-[#191E2E] px-3 py-1.5 text-xs font-medium text-white transition-colors hover:bg-[#232A40] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <RefreshCw className="h-3.5 w-3.5 text-[#AFA6FF]" aria-hidden="true" />
                <span>Re-resolve</span>
              </button>
              <button
                type="button"
                onClick={() => onResolve({ force_refresh: true, allow_magnet_peers: true })}
                className="inline-flex items-center gap-1.5 rounded-xl border border-[#786BFF]/40 bg-[#786BFF]/15 px-3 py-1.5 text-xs font-medium text-[#DAD6FF] transition-colors hover:bg-[#786BFF]/25 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <Sparkles className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Inspect Magnet Peers</span>
              </button>
            </>
          )}
        </div>
      </div>

      {isLoading && (
        <div className="space-y-2.5" aria-busy="true">
          <div className="flex items-center gap-2 text-xs text-[#C8D0E2]">
            <RefreshCw className="h-3.5 w-3.5 animate-spin text-[#786BFF]" aria-hidden="true" />
            <span>Discovering candidate torrents & verifying manifests for Episode {episodeNumber}…</span>
          </div>
          <div className="skeleton-shimmer h-24 rounded-xl" />
          <div className="skeleton-shimmer h-24 rounded-xl" />
        </div>
      )}

      {!isLoading && resolutionState.status === "error" && (
        <div className="rounded-xl border border-rose-400/30 bg-rose-500/10 p-4 text-xs text-rose-200">
          {resolutionState.error || "Unable to resolve episode files right now."}
        </div>
      )}

      {!isLoading && res && (
        <>
          {res.metrics && (
            <div className="flex flex-wrap items-center gap-3 rounded-xl border border-white/[0.06] bg-[#0C101C] px-3.5 py-2 text-[11px] text-[#949CAF]">
              <span>
                Considered <strong className="text-white">{res.metrics.candidates_considered}</strong>{" "}
                ({res.metrics.deduplicated_torrents} duplicate mirrors merged)
              </span>
              <span>·</span>
              <span>
                Manifests inspected <strong className="text-white">{res.metrics.candidates_inspected}</strong>{" "}
                ({res.metrics.cache_hits} cached)
              </span>
              <span>·</span>
              <span>
                Resolved in <strong className="text-white">{Math.round(res.metrics.total_ms)} ms</strong>
              </span>
            </div>
          )}

          {res.candidates.length > 0 ? (
            <div className="space-y-3">
              {res.candidates.map((cand) => renderCandidateCard(cand, true))}
            </div>
          ) : (
            <div className="flex items-start gap-2.5 rounded-xl border border-dashed border-white/15 bg-[#0C101C] p-4 text-xs text-[#AAB5CA]">
              <Info className="mt-0.5 h-4 w-4 shrink-0 text-[#786BFF]" aria-hidden="true" />
              <span>
                No high-confidence verified video file was confirmed for this episode yet.
                {res.unresolved_candidates.length > 0
                  ? " Check unresolved torrents below or click Inspect Magnet Peers to fetch metadata from BitTorrent peers."
                  : " Try refreshing sources or expanding Batch & season packs below."}
              </span>
            </div>
          )}

          {res.possible_matches.length > 0 && (
            <details className="rounded-xl border border-amber-400/20 bg-[#0D111C] p-4">
              <summary className="cursor-pointer text-xs font-semibold text-amber-200 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]">
                Possible Matches ({res.possible_matches.length}) — Weak or ambiguous filename signals
              </summary>
              <div className="mt-3 space-y-3">
                {res.possible_matches.map((cand) => renderCandidateCard(cand, true))}
              </div>
            </details>
          )}

          {res.unresolved_candidates.length > 0 && (
            <details
              open={res.candidates.length === 0}
              className="rounded-xl border border-white/10 bg-[#0D111C] p-4"
            >
              <summary className="cursor-pointer text-xs font-semibold text-[#C8D0E2] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]">
                Unresolved Torrents ({res.unresolved_candidates.length}) — Manifest unavailable or ambiguous container
              </summary>
              <div className="mt-3 space-y-3">
                {res.unresolved_candidates.map((cand) => renderCandidateCard(cand, false))}
              </div>
            </details>
          )}
        </>
      )}

      {!isLoading && !res && fallbackSources.length > 0 && (
        <div className="space-y-2 rounded-xl border border-white/10 bg-[#0C101C] p-4 text-xs text-[#AAB5CA]">
          <p>
            {fallbackSources.length} unverified release{" "}
            {fallbackSources.length === 1 ? "candidate" : "candidates"} pre-indexed. Click{" "}
            <strong>Re-resolve</strong> to inspect exact file indexes before launching playback.
          </p>
        </div>
      )}
    </section>
  );
}
