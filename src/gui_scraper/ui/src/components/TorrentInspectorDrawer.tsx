import {
  Check,
  Copy,
  Download,
  ExternalLink,
  Loader2,
  Magnet,
  Play,
  ShieldCheck,
  X,
} from "lucide-react";
import { AnimatePresence, m } from "motion/react";
import { useEffect, useState } from "react";
import { EASE_CINEMA } from "../lib/motion";
import type { EngineStatus, ScrapedItem, VerifyUrlResponse } from "../lib/types";
import { NyaaLogo } from "./NyaaLogo";
import { StatusBadge } from "./StatusBadge";

interface TorrentInspectorDrawerProps {
  item: ScrapedItem | null;
  engineStatus: EngineStatus | null;
  onClose: () => void;
  onCopyText: (value: string, label: string) => Promise<boolean>;
  onVerifyTorrentHead: (url: string) => Promise<VerifyUrlResponse | null>;
  onOpenExternal: (url: string, label: string) => void;
  onLaunchIntezam?: (item: ScrapedItem) => void;
}

export function TorrentInspectorDrawer({
  item,
  engineStatus,
  onClose,
  onCopyText,
  onVerifyTorrentHead,
  onOpenExternal,
  onLaunchIntezam,
}: TorrentInspectorDrawerProps) {
  const [copiedField, setCopiedField] = useState<"magnet" | "torrent" | null>(null);
  const [isVerifyingHead, setIsVerifyingHead] = useState(false);
  const [headResult, setHeadResult] = useState<VerifyUrlResponse | null>(null);

  useEffect(() => {
    setCopiedField(null);
    setIsVerifyingHead(false);
    setHeadResult(null);
  }, [item?.id, item?.title]);

  useEffect(() => {
    if (!item) return;
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.preventDefault();
        onClose();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [item, onClose]);

  const handleCopy = async (value: string, field: "magnet" | "torrent", label: string) => {
    const ok = await onCopyText(value, label);
    if (ok) {
      setCopiedField(field);
      window.setTimeout(() => setCopiedField(null), 1600);
    }
  };

  const handleVerifyHead = async () => {
    if (!item?.torrent_link || isVerifyingHead) return;
    setIsVerifyingHead(true);
    try {
      const res = await onVerifyTorrentHead(item.torrent_link);
      setHeadResult(res);
    } finally {
      setIsVerifyingHead(false);
    }
  };

  return (
    <AnimatePresence>
      {item && (
        <div className="fixed inset-0 z-50 flex flex-col justify-end">
          {/* Backdrop */}
          <m.div
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.18 }}
            onClick={onClose}
            aria-hidden="true"
            className="absolute inset-0 bg-black/70"
          />

          {/* Bottom Slide-Up Inspector Drawer */}
          <m.section
            role="dialog"
            aria-modal="true"
            aria-label="Torrent Link Inspector"
            initial={{ y: "100%", opacity: 0.6 }}
            animate={{ y: 0, opacity: 1 }}
            exit={{ y: "100%", opacity: 0.6 }}
            transition={{ duration: 0.28, ease: EASE_CINEMA }}
            className="relative z-10 mx-auto w-full max-w-5xl rounded-t-2xl border border-b-0 border-white/15 bg-[#0E121D] px-6 py-5 shadow-[0_-24px_60px_rgba(0,0,0,0.8)]"
          >
            {/* Drag/Visual Handle */}
            <div className="mx-auto mb-4 h-1 w-12 rounded-full bg-white/15" aria-hidden="true" />

            {/* Header */}
            <div className="flex flex-wrap items-start justify-between gap-4">
              <div className="flex min-w-0 flex-1 items-start gap-3">
                <NyaaLogo size={32} className="mt-0.5 shrink-0" />
                <div className="min-w-0 space-y-1.5">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="font-mono text-[11px] text-[#38BDF8]">ROW #{item.id}</span>
                    <StatusBadge isValid={item.is_valid} message={item.verification_msg} />
                    <span className="font-mono text-[11px] text-[#949CAF]">{item.size}</span>
                    <span
                      title={item.relative_date || item.date}
                      className="font-mono text-[11px] text-[#949CAF]"
                    >
                      {item.date}
                      {item.relative_date ? ` (${item.relative_date})` : ""}
                    </span>
                    <span className="font-mono text-[11px] font-semibold text-emerald-400">
                      S: {(item.seeders ?? 0).toLocaleString()}
                    </span>
                    <span className="font-mono text-[11px] font-semibold text-amber-400">
                      L: {(item.leechers ?? 0).toLocaleString()}
                    </span>
                    <span className="font-mono text-[11px] text-[#949CAF]">
                      D: {(item.downloads ?? 0).toLocaleString()}
                    </span>
                  </div>
                  <h3 className="break-words text-sm font-semibold leading-snug text-[#F8FAFC] sm:text-base">
                    {item.title}
                  </h3>
                </div>
              </div>

              <button
                type="button"
                onClick={onClose}
                aria-label="Close bottom inspector drawer"
                className="flex h-8 w-8 shrink-0 items-center justify-center rounded-xl border border-white/10 bg-[#191E2E] text-[#949CAF] transition-colors hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <X className="h-4 w-4" aria-hidden="true" />
              </button>
            </div>

            {/* URIs */}
            <div className="mt-4 grid gap-3 md:grid-cols-2">
              <div className="rounded-xl border border-white/[0.08] bg-[#090B12] p-3">
                <div className="mb-1 flex items-center justify-between text-[10.5px] font-medium uppercase tracking-wider text-[#949CAF]">
                  <span className="inline-flex items-center gap-1.5">
                    <Magnet className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
                    Magnet URI
                  </span>
                  {item.view_url && (
                    <button
                      type="button"
                      onClick={() => onOpenExternal(item.view_url!, "Nyaa view page")}
                      className="inline-flex items-center gap-1 text-[10.5px] text-[#38BDF8] hover:underline"
                    >
                      <span>View Entry</span>
                      <ExternalLink className="h-3 w-3" aria-hidden="true" />
                    </button>
                  )}
                </div>
                <div className="max-h-16 overflow-y-auto break-all font-mono text-[11px] leading-relaxed text-[#F8FAFC] select-all">
                  {item.magnet_link || "No magnet link found on row"}
                </div>
              </div>

              <div className="rounded-xl border border-white/[0.08] bg-[#090B12] p-3">
                <div className="mb-1 flex items-center justify-between text-[10.5px] font-medium uppercase tracking-wider text-[#949CAF]">
                  <span className="inline-flex items-center gap-1.5">
                    <Download className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
                    Resolved .torrent URL
                  </span>
                  {headResult && (
                    <StatusBadge
                      isValid={headResult.is_valid}
                      message={headResult.message}
                    />
                  )}
                </div>
                <div className="max-h-16 overflow-y-auto break-all font-mono text-[11px] leading-relaxed text-[#38BDF8] select-all">
                  {item.torrent_link || "No .torrent link found on row"}
                </div>
              </div>
            </div>

            {/* Action Bar */}
            <div className="mt-4 flex flex-wrap items-center justify-between gap-2.5 border-t border-white/[0.06] pt-3.5">
              <div className="flex flex-wrap items-center gap-2">
                <button
                  type="button"
                  disabled={!item.magnet_link}
                  onClick={() => handleCopy(item.magnet_link || "", "magnet", "Magnet URI")}
                  className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3.5 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-[#786BFF]/50 disabled:cursor-not-allowed disabled:opacity-40"
                >
                  {copiedField === "magnet" ? (
                    <Check className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
                  ) : (
                    <Copy className="h-3.5 w-3.5" aria-hidden="true" />
                  )}
                  <span>{copiedField === "magnet" ? "Copied Magnet" : "Copy Magnet"}</span>
                </button>

                <button
                  type="button"
                  disabled={!item.torrent_link}
                  onClick={() => handleCopy(item.torrent_link || "", "torrent", ".torrent URL")}
                  className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3.5 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-[#38BDF8]/50 disabled:cursor-not-allowed disabled:opacity-40"
                >
                  {copiedField === "torrent" ? (
                    <Check className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
                  ) : (
                    <Copy className="h-3.5 w-3.5" aria-hidden="true" />
                  )}
                  <span>{copiedField === "torrent" ? "Copied .torrent" : "Copy .torrent URL"}</span>
                </button>

                <button
                  type="button"
                  disabled={!item.torrent_link || isVerifyingHead}
                  onClick={handleVerifyHead}
                  className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3.5 text-xs font-medium text-[#F8FAFC] transition-colors hover:border-emerald-400/45 disabled:cursor-not-allowed disabled:opacity-40"
                >
                  {isVerifyingHead ? (
                    <Loader2 className="h-3.5 w-3.5 animate-spin text-[#38BDF8]" aria-hidden="true" />
                  ) : (
                    <ShieldCheck className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
                  )}
                  <span>Verify .torrent HTTP HEAD</span>
                </button>
              </div>

              <div className="flex flex-wrap items-center gap-2">
                {item.magnet_link && (
                  <button
                    type="button"
                    onClick={() => onOpenExternal(item.magnet_link!, "Magnet client")}
                    className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-[#38BDF8]/35 bg-[#38BDF8]/15 px-3.5 text-xs font-semibold text-[#F8FAFC] transition-colors hover:bg-[#38BDF8]/25"
                  >
                    <ExternalLink className="h-3.5 w-3.5" aria-hidden="true" />
                    <span>Open Magnet in Client</span>
                  </button>
                )}

                {item.magnet_link &&
                  engineStatus?.intezam_binary_available &&
                  onLaunchIntezam && (
                    <button
                      type="button"
                      onClick={() => onLaunchIntezam(item)}
                      className="inline-flex h-9 items-center gap-1.5 rounded-xl bg-gradient-to-r from-[#786BFF] to-[#3582F7] px-4 text-xs font-semibold text-[#F8FAFC] shadow-lg shadow-[#786BFF]/25 transition-transform hover:scale-[1.015] active:scale-[0.985]"
                    >
                      <Play className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
                      <span>Stream in Intezam</span>
                    </button>
                  )}
              </div>
            </div>
          </m.section>
        </div>
      )}
    </AnimatePresence>
  );
}
