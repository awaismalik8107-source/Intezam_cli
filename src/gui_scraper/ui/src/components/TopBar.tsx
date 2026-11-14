import { ArrowLeft, Loader2, Search, Settings, X } from "lucide-react";
import { LayoutGroup, m } from "motion/react";
import { type RefObject } from "react";
import { PILL_SPRING } from "../lib/motion";
import type { ScrapeMode } from "../lib/types";

interface TopBarProps {
  pageTitle: string;
  canGoBack: boolean;
  onGoBack: () => void;
  searchInputRef: RefObject<HTMLInputElement | null>;
  query: string;
  mode: ScrapeMode;
  isLoading: boolean;
  bridgeReady: boolean;
  onQueryChange: (value: string) => void;
  onModeChange: (mode: ScrapeMode) => void;
  onSearch: () => void;
  onOpenSettings: () => void;
}

export function TopBar({
  pageTitle,
  canGoBack,
  onGoBack,
  searchInputRef,
  query,
  mode,
  isLoading,
  bridgeReady,
  onQueryChange,
  onModeChange,
  onSearch,
  onOpenSettings,
}: TopBarProps) {
  return (
    <header className="relative shrink-0 border-b border-white/[0.06] bg-[#0B0D16]">
      <div className="flex h-16 items-center justify-between gap-4 px-5">
        {/* Left: Back button or Page Title */}
        <div className="flex min-w-0 items-center gap-3">
          {canGoBack && (
            <button
              type="button"
              onClick={onGoBack}
              aria-label="Back to browse"
              className="inline-flex h-8 items-center gap-1.5 rounded-lg border border-white/10 bg-[#111522] px-2.5 text-xs font-medium text-[#F8FAFC] transition-colors hover:bg-[#191E2E] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <ArrowLeft className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Back</span>
            </button>
          )}
          <h1 className="truncate text-sm font-semibold tracking-tight text-[#F8FAFC]">
            {pageTitle}
          </h1>
        </div>

        {/* Center/Right: Integrated Cinema Search Field + Mode Pill + Settings */}
        <div className="flex flex-1 items-center justify-end gap-3">
          <div className="flex w-full max-w-[480px] items-center gap-2 rounded-full border border-white/10 bg-[#111522] py-1 pl-3.5 pr-1.5 transition-colors focus-within:border-[#786BFF]/60">
            {isLoading ? (
              <Loader2 className="h-4 w-4 shrink-0 animate-spin text-[#38BDF8]" aria-hidden="true" />
            ) : (
              <Search className="h-4 w-4 shrink-0 text-[#949CAF]" aria-hidden="true" />
            )}

            <input
              ref={searchInputRef}
              id="cinema-search-input"
              type="text"
              value={query}
              autoComplete="off"
              spellCheck={false}
              aria-label="Search Nyaa or browse default endpoint"
              placeholder="Search Nyaa releases, or press Enter to browse..."
              onChange={(e) => onQueryChange(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === "Enter") {
                  e.preventDefault();
                  onSearch();
                }
              }}
              className="w-full bg-transparent text-[13px] text-[#F8FAFC] placeholder-[#949CAF]/70 focus:outline-none"
            />

            {query.length > 0 && (
              <button
                type="button"
                aria-label="Clear search"
                onClick={() => {
                  onQueryChange("");
                  searchInputRef.current?.focus();
                }}
                className="rounded-full p-1 text-[#949CAF] hover:bg-white/10 hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <X className="h-3.5 w-3.5" aria-hidden="true" />
              </button>
            )}

            <kbd className="hidden select-none rounded-md border border-white/10 bg-[#191E2E] px-1.5 py-0.5 font-mono text-[10px] text-[#949CAF] md:inline-block">
              Ctrl+K
            </kbd>

            {/* Compact Mock / Live toggle */}
            <LayoutGroup id="topbar-mode-toggle">
              <div
                role="tablist"
                aria-label="Source mode"
                className="flex items-center rounded-full border border-white/[0.08] bg-[#090B12] p-0.5"
              >
                {(["mock", "live"] as const).map((mOption) => {
                  const active = mode === mOption;
                  return (
                    <button
                      key={mOption}
                      type="button"
                      role="tab"
                      aria-selected={active}
                      onClick={() => onModeChange(mOption)}
                      className={`relative rounded-full px-2.5 py-1 text-[11px] font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                        active ? "text-[#F8FAFC]" : "text-[#949CAF] hover:text-[#F8FAFC]"
                      }`}
                    >
                      {active && (
                        <m.span
                          layoutId="topbar-mode-pill"
                          transition={PILL_SPRING}
                          aria-hidden="true"
                          className="absolute inset-0 rounded-full bg-[#191E2E] ring-1 ring-[#786BFF]/40"
                        />
                      )}
                      <span className="relative z-10 capitalize">{mOption}</span>
                    </button>
                  );
                })}
              </div>
            </LayoutGroup>
          </div>

          {/* Connection status dot */}
          <div
            title={bridgeReady ? "Python bridge connected" : "Connecting bridge..."}
            className="hidden items-center gap-1.5 rounded-full border border-white/[0.08] bg-[#111522] px-2.5 py-1 text-[11px] text-[#949CAF] xl:flex"
          >
            <span
              className={`h-1.5 w-1.5 rounded-full ${bridgeReady ? "bg-emerald-400" : "bg-amber-400 animate-pulse"}`}
              aria-hidden="true"
            />
            <span>{bridgeReady ? "Ready" : "Init"}</span>
          </div>

          <button
            type="button"
            id="cinema-settings-btn"
            onClick={onOpenSettings}
            aria-label="Open settings"
            title="Settings (Ctrl+,)"
            className="flex h-9 w-9 shrink-0 items-center justify-center rounded-full border border-white/10 bg-[#111522] text-[#949CAF] transition-colors hover:bg-[#191E2E] hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
          >
            <Settings className="h-4 w-4" aria-hidden="true" />
          </button>
        </div>
      </div>

      {/* Intezam Flowing-Light Data Progress Indicator */}
      {isLoading && (
        <div
          aria-hidden="true"
          className="intezam-flow-bar absolute bottom-0 left-0 right-0 h-[2px] bg-[#786BFF]/15"
        />
      )}
    </header>
  );
}
