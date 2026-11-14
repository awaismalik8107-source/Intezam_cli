import {
  Check,
  ClipboardCopy,
  ExternalLink,
  HardDrive,
  Loader2,
  Play,
  ShieldCheck,
  Square,
  Waves,
} from "lucide-react";
import { m } from "motion/react";
import { useState } from "react";
import { EASE_CINEMA } from "../lib/motion";
import type { EngineStatus, MediaSource, StreamSessionState } from "../lib/types";
import { StatusBadge } from "./StatusBadge";

interface IntezamFlowPanelProps {
  source: MediaSource;
  engineStatus: EngineStatus | null;
  streamState: StreamSessionState | null;
  isVerifying: boolean;
  strictCacheBudget: boolean;
  onVerify: (source: MediaSource) => void;
  onCopyUri: (source: MediaSource) => Promise<boolean>;
  onLaunchIntezam: (source: MediaSource) => void;
  onStopIntezam: () => void;
  onOpenExternal: (source: MediaSource) => void;
}

const FLOW_STAGES: ReadonlyArray<{ id: string; label: string }> = [
  { id: "connecting", label: "Connecting to peers" },
  { id: "metadata", label: "Fetching metadata" },
  { id: "selecting", label: "Selecting video file" },
  { id: "buffering", label: "Preparing 2.5% rolling cache" },
  { id: "playing", label: "LibVLC playback active" },
];

function stageRank(stage: StreamSessionState["stage"] | undefined): number {
  switch (stage) {
    case "connecting":
      return 0;
    case "metadata":
      return 1;
    case "selecting":
      return 2;
    case "buffering":
      return 3;
    case "playing":
    case "completed":
      return 4;
    default:
      return -1;
  }
}

export function IntezamFlowPanel({
  source,
  engineStatus,
  streamState,
  isVerifying,
  strictCacheBudget,
  onVerify,
  onCopyUri,
  onLaunchIntezam,
  onStopIntezam,
  onOpenExternal,
}: IntezamFlowPanelProps) {
  const [copied, setCopied] = useState(false);

  const isCurrentStream =
    streamState && streamState.url && streamState.url === source.item.resolved_link;
  const activeRank = isCurrentStream ? stageRank(streamState?.stage) : -1;
  const canLaunchIntezam =
    source.isMagnet && source.item.is_valid && Boolean(engineStatus?.intezam_binary_available);

  const handleCopy = async () => {
    const ok = await onCopyUri(source);
    if (ok) {
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1500);
    }
  };

  return (
    <m.section
      initial={{ opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.25, ease: EASE_CINEMA }}
      aria-label="Intezam Flow playback readiness"
      className="rounded-2xl border border-[#786BFF]/25 bg-[#111522] p-5 shadow-[0_20px_50px_-24px_rgba(120,107,255,0.28)]"
    >
      {/* Header */}
      <div className="flex flex-wrap items-start justify-between gap-3 border-b border-white/[0.06] pb-4">
        <div className="space-y-1">
          <div className="flex items-center gap-2">
            <Waves className="h-4 w-4 text-[#38BDF8]" aria-hidden="true" />
            <span className="text-xs font-semibold uppercase tracking-wider text-[#38BDF8]">
              Intezam Flow
            </span>
            <StatusBadge
              isValid={source.item.is_valid}
              message={source.item.verification_msg}
              isVerifying={isVerifying}
            />
          </div>
          <h3 className="text-sm font-semibold text-[#F8FAFC]">{source.sourceTitle}</h3>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <button
            type="button"
            disabled={isVerifying || !source.item.resolved_link}
            onClick={() => onVerify(source)}
            className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-[#786BFF]/45 disabled:opacity-50"
          >
            {isVerifying ? (
              <Loader2 className="h-3.5 w-3.5 animate-spin text-amber-300" aria-hidden="true" />
            ) : (
              <ShieldCheck className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
            )}
            <span>{isVerifying ? "Verifying..." : "Verify Link"}</span>
          </button>

          <button
            type="button"
            disabled={!source.item.resolved_link}
            onClick={handleCopy}
            className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-white/20 disabled:opacity-50"
          >
            {copied ? (
              <Check className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
            ) : (
              <ClipboardCopy className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
            )}
            <span>{copied ? "Copied" : "Copy URI"}</span>
          </button>

          {isCurrentStream && streamState?.active ? (
            <button
              type="button"
              onClick={onStopIntezam}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-rose-400/40 bg-rose-500/20 px-3.5 text-xs font-semibold text-rose-200 hover:bg-rose-500/30"
            >
              <Square className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
              <span>Stop Stream</span>
            </button>
          ) : canLaunchIntezam ? (
            <button
              type="button"
              onClick={() => onLaunchIntezam(source)}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl bg-gradient-to-r from-[#786BFF] to-[#5B8DEF] px-4 text-xs font-semibold text-[#F8FAFC] shadow-lg shadow-[#786BFF]/25 hover:brightness-110"
            >
              <Play className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
              <span>Stream in Intezam</span>
            </button>
          ) : null}

          <button
            type="button"
            disabled={!source.item.resolved_link}
            onClick={() => onOpenExternal(source)}
            className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-white/20 disabled:opacity-50"
          >
            <ExternalLink className="h-3.5 w-3.5 text-[#786BFF]" aria-hidden="true" />
            <span>Open External</span>
          </button>
        </div>
      </div>

      {/* URI & Technical Identifiers */}
      <div className="mt-4 space-y-2">
        <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-[#949CAF]">
          <span>Resolved Stream URI</span>
          {source.infoHash && (
            <span className="font-mono text-[10.5px] text-[#38BDF8]">
              BTIH: {source.infoHash}
            </span>
          )}
        </div>
        <div className="max-h-24 select-text overflow-y-auto break-all rounded-xl border border-white/[0.07] bg-[#090B12] p-3 font-mono text-[11.5px] leading-relaxed text-[#F8FAFC]">
          {source.item.resolved_link || "(empty)"}
        </div>
      </div>

      {/* Rolling Cache Architecture + Real Stage Telemetry */}
      <div className="mt-4 grid gap-4 lg:grid-cols-2">
        {/* Rolling Cache Policy Visualization (honest representation of 2.5% budget) */}
        <div className="rounded-xl border border-white/[0.06] bg-[#090B12]/70 p-3.5">
          <div className="flex items-center justify-between text-xs">
            <span className="inline-flex items-center gap-1.5 font-medium text-[#F8FAFC]">
              <HardDrive className="h-3.5 w-3.5 text-[#786BFF]" aria-hidden="true" />
              Rolling Cache Policy
            </span>
            <span className="font-mono text-[10.5px] text-[#38BDF8]">
              {strictCacheBudget ? "Strict 2.5% budget" : "2.5% target · 25% ceiling"}
            </span>
          </div>

          {/* 1/3 behind playback | Cursor | 2/3 ahead prefetch visual */}
          <div className="mt-3 flex h-2.5 w-full overflow-hidden rounded-full bg-white/[0.06]">
            <div
              className="h-full w-1/3 bg-[#786BFF]/55"
              title="1/3 retained behind playback cursor"
            />
            <div className="h-full w-[3px] bg-[#F8FAFC]" title="Current playback position" />
            <div
              className={`h-full flex-1 ${
                isCurrentStream && streamState?.active
                  ? "intezam-flow-bar bg-[#38BDF8]/25"
                  : "bg-[#38BDF8]/30"
              }`}
              title="2/3 prefetch window ahead of playback"
            />
          </div>

          <div className="mt-2 flex justify-between font-mono text-[10px] text-[#949CAF]">
            <span>1/3 Retained Behind</span>
            <span className="text-[#F8FAFC]">Playback</span>
            <span>2/3 Prefetch Ahead</span>
          </div>
        </div>

        {/* Genuine Engine Preparation / Status */}
        <div className="rounded-xl border border-white/[0.06] bg-[#090B12]/70 p-3.5">
          <div className="flex items-center justify-between text-xs">
            <span className="font-medium text-[#F8FAFC]">Engine Readiness</span>
            <span className="font-mono text-[10.5px] text-[#949CAF]">
              {isCurrentStream && streamState
                ? streamState.stage.toUpperCase()
                : canLaunchIntezam
                  ? "READY FOR HANDOFF"
                  : source.isMagnet
                    ? "EXTERNAL HANDOFF"
                    : "HTTP / URI"}
            </span>
          </div>

          {isCurrentStream && streamState ? (
            <div className="mt-2.5 space-y-2">
              <div className="flex items-center gap-1.5">
                {FLOW_STAGES.map((st, idx) => {
                  const reached = activeRank >= idx;
                  return (
                    <div
                      key={st.id}
                      title={st.label}
                      className={`h-1.5 flex-1 rounded-full transition-colors ${
                        streamState.stage === "error"
                          ? "bg-rose-400/50"
                          : reached
                            ? "bg-gradient-to-r from-[#786BFF] to-[#38BDF8]"
                            : "bg-white/10"
                      }`}
                    />
                  );
                })}
              </div>
              <p className="text-xs text-[#F8FAFC]">{streamState.message}</p>
              {streamState.selected_video && (
                <p className="truncate font-mono text-[10.5px] text-emerald-300">
                  Stream: {streamState.selected_video}{" "}
                  {streamState.video_size ? `(${streamState.video_size})` : ""}
                </p>
              )}
              {streamState.lines.length > 0 && (
                <div className="max-h-20 overflow-y-auto rounded-lg border border-white/[0.06] bg-black/40 p-2 font-mono text-[10px] text-[#949CAF]">
                  {streamState.lines.slice(-4).map((line, i) => (
                    <div key={i} className="truncate">
                      {line}
                    </div>
                  ))}
                </div>
              )}
            </div>
          ) : (
            <p className="mt-2 text-xs leading-relaxed text-[#949CAF]">
              {canLaunchIntezam
                ? "Compiled Intezam C++ + LibVLC engine is ready. Click Stream in Intezam to fetch metadata and begin verified rolling-cache playback."
                : !source.isMagnet
                  ? "HTTP(S) links open directly in your OS browser/handler. The C++ rolling-cache engine accepts magnet:? URIs."
                  : "Compiled Intezam C++ binary was not detected; use Open External to hand off this magnet URI to your system client."}
            </p>
          )}
        </div>
      </div>
    </m.section>
  );
}
