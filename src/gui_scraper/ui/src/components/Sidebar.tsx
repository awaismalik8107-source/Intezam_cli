import {
  Bookmark,
  Compass,
  Home,
  PlayCircle,
  Puzzle,
  Search,
  Settings,
} from "lucide-react";
import { LayoutGroup, m } from "motion/react";
import { PILL_SPRING } from "../lib/motion";
import type { EngineStatus, NavSection } from "../lib/types";
import { IntezamLogo } from "./IntezamLogo";
import { NyaaLogo } from "./NyaaLogo";

interface SidebarProps {
  activeSection: NavSection;
  onSelectSection: (section: NavSection) => void;
  libraryCount: number;
  watchCount: number;
  engineStatus: EngineStatus | null;
}

const NAV_ITEMS: ReadonlyArray<{
  id: NavSection;
  label: string;
  icon: typeof Home;
}> = [
  { id: "home", label: "Home", icon: Home },
  { id: "discover", label: "Discover", icon: Compass },
  { id: "extensions", label: "Extensions", icon: Puzzle },
  { id: "search", label: "Search", icon: Search },
  { id: "library", label: "Library", icon: Bookmark },
  { id: "continue", label: "Continue Watching", icon: PlayCircle },
  { id: "settings", label: "Settings", icon: Settings },
];

export function Sidebar({
  activeSection,
  onSelectSection,
  libraryCount,
  watchCount,
  engineStatus,
}: SidebarProps) {
  return (
    <aside
      aria-label="Primary navigation"
      className="flex h-full w-[72px] shrink-0 flex-col justify-between border-r border-white/[0.06] bg-[#0B0D16] px-2.5 py-4 md:w-[224px] md:px-3.5"
    >
      <div className="space-y-6">
        {/* Brand mark */}
        <div className="flex items-center justify-center gap-2.5 px-1.5 md:justify-start">
          <IntezamLogo size={30} />
          <div className="hidden min-w-0 md:block">
            <div className="text-[13.5px] font-semibold tracking-[0.12em] text-[#F8FAFC]">
              INTEZAM
            </div>
            <div className="text-[10.5px] font-medium text-[#949CAF]">Cinema Stream</div>
          </div>
        </div>

        {/* Navigation Links */}
        <LayoutGroup id="sidebar-navigation">
          <nav className="space-y-1">
            {NAV_ITEMS.map((item) => {
              const Icon = item.icon;
              const isActive = activeSection === item.id;
              const badgeCount =
                item.id === "library"
                  ? libraryCount
                  : item.id === "continue"
                    ? watchCount
                    : 0;

              return (
                <button
                  key={item.id}
                  type="button"
                  onClick={() => onSelectSection(item.id)}
                  aria-current={isActive ? "page" : undefined}
                  title={item.label}
                  className={`group relative flex w-full items-center justify-center gap-3 rounded-xl px-2.5 py-2.5 text-[13px] font-medium transition-colors duration-150 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] md:justify-start md:px-3 ${
                    isActive
                      ? "text-[#F8FAFC]"
                      : "text-[#949CAF] hover:bg-[#111522] hover:text-[#F8FAFC]"
                  }`}
                >
                  {isActive && (
                    <m.span
                      layoutId="sidebar-active-indicator"
                      transition={PILL_SPRING}
                      aria-hidden="true"
                      className="absolute inset-0 rounded-xl border border-[#786BFF]/25 bg-[#191E2E]"
                    >
                      <span className="absolute bottom-2 left-0 top-2 w-[3px] rounded-r-full bg-gradient-to-b from-[#786BFF] to-[#38BDF8]" />
                    </m.span>
                  )}

                  <Icon
                    className={`relative z-10 h-4 w-4 shrink-0 transition-colors ${
                      isActive ? "text-[#786BFF]" : "text-[#949CAF] group-hover:text-[#F8FAFC]"
                    }`}
                    aria-hidden="true"
                  />
                  <span className="relative z-10 hidden truncate md:inline">
                    {item.label}
                  </span>
                  {item.id === "extensions" && (
                    <span
                      title="Nyaa Extension Active"
                      className="relative z-10 ml-auto hidden items-center gap-1 md:inline-flex"
                    >
                      <NyaaLogo size={17} className="rounded shadow-sm shadow-[#3582F7]/30" />
                    </span>
                  )}
                  {badgeCount > 0 && (
                    <span className="relative z-10 ml-auto hidden rounded-md bg-white/[0.07] px-1.5 py-0.5 font-mono text-[10px] text-[#949CAF] md:inline">
                      {badgeCount}
                    </span>
                  )}
                </button>
              );
            })}
          </nav>
        </LayoutGroup>
      </div>

      {/* Bottom Engine Status Card */}
      <div className="hidden rounded-xl border border-white/[0.07] bg-[#111522] p-3 md:block">
        <div className="flex items-center justify-between">
          <span className="text-[11px] font-medium text-[#949CAF]">Intezam Engine</span>
          <span
            className={`h-2 w-2 rounded-full ${
              engineStatus?.stream_active
                ? "bg-[#38BDF8] animate-pulse"
                : engineStatus?.intezam_binary_available
                  ? "bg-emerald-400"
                  : "bg-amber-400"
            }`}
            aria-hidden="true"
          />
        </div>
        <div className="mt-1 text-xs font-medium text-[#F8FAFC]">
          {engineStatus?.stream_active
            ? "Stream Active"
            : engineStatus?.intezam_binary_available
              ? "LibVLC + Cache Ready"
              : "Scraper + External Mode"}
        </div>
        <div className="mt-1 font-mono text-[10px] text-[#949CAF]">
          2.5% rolling cache policy
        </div>
      </div>
    </aside>
  );
}
