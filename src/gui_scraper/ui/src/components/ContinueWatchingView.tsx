import { Play, PlayCircle, Search, Trash2 } from "lucide-react";
import { m } from "motion/react";
import { PAGE_VARIANTS } from "../lib/motion";
import type { WatchHistoryEntry } from "../lib/types";

interface ContinueWatchingViewProps {
  watchHistory: WatchHistoryEntry[];
  onLaunchEntry: (entry: WatchHistoryEntry) => void;
  onClearHistory: () => void;
  onExplore: () => void;
}

export function ContinueWatchingView({
  watchHistory,
  onLaunchEntry,
  onClearHistory,
  onExplore,
}: ContinueWatchingViewProps) {
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
          <h2 className="text-lg font-semibold text-[#F8FAFC]">Continue Watching</h2>
          <p className="mt-0.5 text-xs text-[#949CAF]">
            Real stream sessions launched through Intezam Flow or external handlers
          </p>
        </div>

        {watchHistory.length > 0 && (
          <button
            type="button"
            onClick={onClearHistory}
            className="inline-flex h-8 items-center gap-1.5 rounded-lg border border-white/10 bg-[#111522] px-3 text-xs text-[#949CAF] hover:bg-[#191E2E] hover:text-[#F8FAFC]"
          >
            <Trash2 className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Clear History</span>
          </button>
        )}
      </div>

      {watchHistory.length === 0 ? (
        <div className="flex flex-col items-center justify-center rounded-2xl border border-white/[0.06] bg-[#111522]/75 px-6 py-20 text-center">
          <div className="mb-4 flex h-12 w-12 items-center justify-center rounded-2xl border border-[#38BDF8]/25 bg-[#38BDF8]/10">
            <PlayCircle className="h-5 w-5 text-[#38BDF8]" aria-hidden="true" />
          </div>
          <h3 className="text-base font-semibold text-[#F8FAFC]">No playback history yet</h3>
          <p className="mt-1.5 max-w-md text-xs leading-relaxed text-[#949CAF]">
            Streams launched via Intezam Flow or external handlers are recorded here automatically.
            Progress bars are only shown when genuine playback telemetry is reported.
          </p>
          <button
            type="button"
            onClick={onExplore}
            className="mt-5 inline-flex h-9 items-center gap-2 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC]"
          >
            <Search className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Browse &amp; Discover</span>
          </button>
        </div>
      ) : (
        <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          {watchHistory.map((entry) => (
            <div
              key={entry.key}
              className="flex flex-col justify-between gap-4 rounded-2xl border border-white/[0.08] bg-[#111522] p-4"
            >
              <div>
                <div className="flex items-center justify-between gap-2">
                  <span className="rounded-md border border-[#786BFF]/30 bg-[#786BFF]/10 px-2 py-0.5 font-mono text-[10px] text-[#786BFF]">
                    {entry.engine}
                  </span>
                  <span className="font-mono text-[10.5px] text-[#949CAF]">
                    {new Date(entry.launched_at).toLocaleString()}
                  </span>
                </div>
                <h3 className="mt-3 line-clamp-2 text-sm font-semibold text-[#F8FAFC]">
                  {entry.title}
                </h3>
                <p className="mt-1.5 truncate font-mono text-[11px] text-[#949CAF]">
                  {entry.resolved_link}
                </p>
              </div>

              <button
                type="button"
                onClick={() => onLaunchEntry(entry)}
                className="inline-flex h-9 items-center justify-center gap-1.5 rounded-xl bg-[#191E2E] text-xs font-medium text-[#F8FAFC] ring-1 ring-white/10 transition-colors hover:bg-[#786BFF]"
              >
                <Play className="h-3.5 w-3.5 fill-current" aria-hidden="true" />
                <span>Re-launch Stream</span>
              </button>
            </div>
          ))}
        </div>
      )}
    </m.div>
  );
}
