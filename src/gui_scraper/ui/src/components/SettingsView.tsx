import {
  Cpu,
  Globe,
  HardDrive,
  Palette,
  RefreshCw,
  RotateCcw,
  Save,
  Terminal,
} from "lucide-react";
import { m } from "motion/react";
import { useEffect, useMemo, useState } from "react";
import { buildPreviewUrl } from "../lib/mediaAdapter";
import { PAGE_VARIANTS } from "../lib/motion";
import type {
  EngineStatus,
  ScrapeMode,
  ScraperConfig,
  UiPreferences,
} from "../lib/types";

interface SettingsViewProps {
  config: ScraperConfig;
  preferences: UiPreferences;
  engineStatus: EngineStatus | null;
  appLogs: string[];
  isSaving: boolean;
  errorMessage: string | null;
  onUpdatePreferences: (patch: Partial<UiPreferences>) => void;
  onSaveConfig: (draft: ScraperConfig) => Promise<void>;
  onResetConfig: () => Promise<ScraperConfig | null>;
  onRefreshDiagnostics: () => void;
}

type SettingsTab = "appearance" | "scraper" | "streaming" | "advanced";

const INPUT_CLS =
  "h-9 w-full rounded-xl border border-white/10 bg-[#090B12] px-3 font-mono text-xs text-[#F8FAFC] placeholder-[#949CAF]/50 focus:border-[#786BFF] focus:outline-none";

export function SettingsView({
  config,
  preferences,
  engineStatus,
  appLogs,
  isSaving,
  errorMessage,
  onUpdatePreferences,
  onSaveConfig,
  onResetConfig,
  onRefreshDiagnostics,
}: SettingsViewProps) {
  const [activeTab, setActiveTab] = useState<SettingsTab>("scraper");
  const [draft, setDraft] = useState<ScraperConfig>(config);
  const [localError, setLocalError] = useState<string | null>(null);

  useEffect(() => {
    setDraft(config);
  }, [config]);

  const hasUnsaved = useMemo(
    () => JSON.stringify(draft) !== JSON.stringify(config),
    [draft, config],
  );
  const previewUrl = useMemo(() => buildPreviewUrl(draft, "dune"), [draft]);
  const displayedError = localError || errorMessage;

  const updateField = <K extends keyof ScraperConfig>(field: K, value: ScraperConfig[K]) => {
    setLocalError(null);
    setDraft((prev) => ({ ...prev, [field]: value }));
  };

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!draft.base_url.trim().startsWith("http")) {
      setLocalError("BASE_URL must start with http:// or https://");
      return;
    }
    if (!draft.row_selector.trim()) {
      setLocalError("ROW_SELECTOR cannot be empty.");
      return;
    }
    const timeoutVal = Number(draft.timeout);
    if (!Number.isFinite(timeoutVal) || timeoutVal <= 0) {
      setLocalError("TIMEOUT must be a positive number.");
      return;
    }
    await onSaveConfig(draft);
  };

  const handleRestore = async () => {
    setLocalError(null);
    const restored = await onResetConfig();
    if (restored) setDraft(restored);
  };

  const tabs: ReadonlyArray<{ id: SettingsTab; label: string; icon: typeof Palette }> = [
    { id: "appearance", label: "Appearance", icon: Palette },
    { id: "scraper", label: "Scraper", icon: Globe },
    { id: "streaming", label: "Streaming", icon: HardDrive },
    { id: "advanced", label: "Advanced", icon: Terminal },
  ];

  return (
    <m.div
      variants={PAGE_VARIANTS}
      initial="initial"
      animate="animate"
      exit="exit"
      className="mx-auto max-w-4xl space-y-6 pb-12"
    >
      <div>
        <h2 className="text-lg font-semibold text-[#F8FAFC]">Settings</h2>
        <p className="mt-0.5 text-xs text-[#949CAF]">
          Manage appearance, scraper endpoints, CSS selectors, and Intezam streaming integration
        </p>
      </div>

      {/* Section Tabs */}
      <div className="flex flex-wrap gap-2 border-b border-white/[0.07] pb-3">
        {tabs.map((tab) => {
          const Icon = tab.icon;
          const active = activeTab === tab.id;
          return (
            <button
              key={tab.id}
              type="button"
              onClick={() => setActiveTab(tab.id)}
              className={`inline-flex items-center gap-2 rounded-xl px-3.5 py-2 text-xs font-medium transition-colors ${
                active
                  ? "border border-[#786BFF]/35 bg-[#191E2E] text-[#F8FAFC]"
                  : "border border-transparent text-[#949CAF] hover:bg-[#111522] hover:text-[#F8FAFC]"
              }`}
            >
              <Icon className={`h-3.5 w-3.5 ${active ? "text-[#786BFF]" : ""}`} aria-hidden="true" />
              <span>{tab.label}</span>
            </button>
          );
        })}
      </div>

      {/* Appearance */}
      {activeTab === "appearance" && (
        <section className="space-y-4 rounded-2xl border border-white/[0.07] bg-[#111522] p-5">
          <div className="flex items-center justify-between gap-4 border-b border-white/[0.06] pb-4">
            <div>
              <div className="text-sm font-medium text-[#F8FAFC]">Motion Preference</div>
              <div className="text-xs text-[#949CAF]">
                Respect system accessibility settings or force reduced motion
              </div>
            </div>
            <select
              aria-label="Motion preference"
              value={preferences.reducedMotion}
              onChange={(e) =>
                onUpdatePreferences({
                  reducedMotion: e.target.value as UiPreferences["reducedMotion"],
                })
              }
              className="h-9 rounded-xl border border-white/10 bg-[#090B12] px-3 text-xs text-[#F8FAFC]"
            >
              <option value="system">Follow System</option>
              <option value="reduce">Reduce Motion</option>
              <option value="full">Full Transitions</option>
            </select>
          </div>

          <div className="flex items-center justify-between gap-4">
            <div>
              <div className="text-sm font-medium text-[#F8FAFC]">Ambient Cinema Lighting</div>
              <div className="text-xs text-[#949CAF]">
                Subtle midnight-indigo backdrop illumination behind featured surfaces
              </div>
            </div>
            <button
              type="button"
              role="switch"
              aria-checked={preferences.ambientLighting}
              onClick={() =>
                onUpdatePreferences({ ambientLighting: !preferences.ambientLighting })
              }
              className={`relative h-6 w-11 rounded-full transition-colors ${
                preferences.ambientLighting ? "bg-[#786BFF]" : "bg-[#191E2E]"
              }`}
            >
              <span
                className={`block h-4 w-4 rounded-full bg-white transition-transform ${
                  preferences.ambientLighting ? "translate-x-6" : "translate-x-1"
                }`}
              />
            </button>
          </div>
        </section>
      )}

      {/* Scraper Configuration */}
      {activeTab === "scraper" && (
        <form
          onSubmit={handleSubmit}
          className="space-y-4 rounded-2xl border border-white/[0.07] bg-[#111522] p-5"
        >
          <div className="flex flex-wrap items-center justify-between gap-2 border-b border-white/[0.06] pb-3">
            <div>
              <div className="flex items-center gap-2">
                <h3 className="text-sm font-semibold text-[#F8FAFC]">
                  Provider &amp; CSS Selectors
                </h3>
                {hasUnsaved && (
                  <span className="rounded-full border border-amber-400/30 bg-amber-400/10 px-2 py-0.5 font-mono text-[10px] text-amber-300">
                    Unsaved changes
                  </span>
                )}
              </div>
              <p className="text-xs text-[#949CAF]">
                Applied live to the Python ConfigurableTableScraper engine
              </p>
            </div>

            <div className="flex items-center gap-2">
              <label htmlFor="default-mode-select" className="text-xs text-[#949CAF]">
                Default mode
              </label>
              <select
                id="default-mode-select"
                value={preferences.defaultMode}
                onChange={(e) =>
                  onUpdatePreferences({ defaultMode: e.target.value as ScrapeMode })
                }
                className="h-8 rounded-lg border border-white/10 bg-[#090B12] px-2.5 text-xs text-[#F8FAFC]"
              >
                <option value="mock">Mock Data</option>
                <option value="live">Live Scraper</option>
              </select>
            </div>
          </div>

          <div className="grid gap-3.5 sm:grid-cols-2">
            <div className="sm:col-span-2">
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                Provider Name
              </label>
              <input
                type="text"
                value={draft.provider_name}
                onChange={(e) => updateField("provider_name", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                BASE_URL
              </label>
              <input
                type="text"
                required
                value={draft.base_url}
                onChange={(e) => updateField("base_url", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                  SEARCH_PATH
                </label>
                <input
                  type="text"
                  value={draft.search_path}
                  onChange={(e) => updateField("search_path", e.target.value)}
                  className={INPUT_CLS}
                />
              </div>
              <div>
                <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                  QUERY_PARAM
                </label>
                <input
                  type="text"
                  value={draft.query_param}
                  onChange={(e) => updateField("query_param", e.target.value)}
                  className={INPUT_CLS}
                />
              </div>
            </div>

            <div className="rounded-xl border border-white/[0.06] bg-[#090B12] px-3 py-2 sm:col-span-2">
              <div className="font-mono text-[10px] uppercase text-[#949CAF]">
                Constructed Request Preview
              </div>
              <div className="mt-1 truncate font-mono text-[11.5px] text-[#38BDF8]">
                {previewUrl || "Enter a valid BASE_URL"}
              </div>
            </div>

            <div className="sm:col-span-2">
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                ROW_SELECTOR
              </label>
              <input
                type="text"
                required
                value={draft.row_selector}
                onChange={(e) => updateField("row_selector", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                TITLE_SELECTOR
              </label>
              <input
                type="text"
                value={draft.title_selector}
                onChange={(e) => updateField("title_selector", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                LINK_SELECTOR
              </label>
              <input
                type="text"
                value={draft.link_selector}
                onChange={(e) => updateField("link_selector", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                SIZE_SELECTOR
              </label>
              <input
                type="text"
                value={draft.size_selector}
                onChange={(e) => updateField("size_selector", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                DATE_SELECTOR
              </label>
              <input
                type="text"
                value={draft.date_selector}
                onChange={(e) => updateField("date_selector", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                STATS_SELECTOR
              </label>
              <input
                type="text"
                value={draft.stats_selector}
                onChange={(e) => updateField("stats_selector", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div>
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                TIMEOUT (SEC)
              </label>
              <input
                type="number"
                step="0.5"
                min="1"
                value={draft.timeout}
                onChange={(e) => updateField("timeout", e.target.value)}
                className={INPUT_CLS}
              />
            </div>

            <div className="sm:col-span-2">
              <label className="mb-1.5 block font-mono text-[10.5px] uppercase text-[#949CAF]">
                USER_AGENT
              </label>
              <input
                type="text"
                value={draft.user_agent}
                onChange={(e) => updateField("user_agent", e.target.value)}
                className={INPUT_CLS}
              />
            </div>
          </div>

          {displayedError && (
            <div
              role="alert"
              className="rounded-xl border border-rose-400/30 bg-rose-500/10 px-3.5 py-2.5 text-xs text-rose-300"
            >
              {displayedError}
            </div>
          )}

          <div className="flex flex-wrap items-center justify-between gap-2 border-t border-white/[0.06] pt-4">
            <button
              type="button"
              disabled={isSaving}
              onClick={handleRestore}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl border border-white/10 bg-[#191E2E] px-3.5 text-xs font-medium text-[#949CAF] hover:text-[#F8FAFC]"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Restore Defaults</span>
            </button>

            <button
              type="submit"
              disabled={isSaving}
              className="inline-flex h-9 items-center gap-1.5 rounded-xl bg-[#786BFF] px-4 text-xs font-semibold text-[#F8FAFC] hover:bg-[#6A5CFF] disabled:opacity-55"
            >
              <Save className="h-3.5 w-3.5" aria-hidden="true" />
              <span>{isSaving ? "Saving..." : "Save Configuration"}</span>
            </button>
          </div>
        </form>
      )}

      {/* Streaming & Cache Policy */}
      {activeTab === "streaming" && (
        <section className="space-y-4 rounded-2xl border border-white/[0.07] bg-[#111522] p-5">
          <div className="flex items-center justify-between gap-4 border-b border-white/[0.06] pb-4">
            <div>
              <div className="text-sm font-medium text-[#F8FAFC]">Strict 2.5% Cache Budget</div>
              <div className="text-xs text-[#949CAF]">
                Pass <span className="font-mono">--strict-cache</span> when launching the Intezam
                C++ engine instead of the adaptive 2.5%–25% ceiling
              </div>
            </div>
            <button
              type="button"
              role="switch"
              aria-checked={preferences.strictCacheBudget}
              onClick={() =>
                onUpdatePreferences({ strictCacheBudget: !preferences.strictCacheBudget })
              }
              className={`relative h-6 w-11 rounded-full transition-colors ${
                preferences.strictCacheBudget ? "bg-[#786BFF]" : "bg-[#191E2E]"
              }`}
            >
              <span
                className={`block h-4 w-4 rounded-full bg-white transition-transform ${
                  preferences.strictCacheBudget ? "translate-x-6" : "translate-x-1"
                }`}
              />
            </button>
          </div>

          <div className="grid gap-3 sm:grid-cols-2">
            <div className="rounded-xl border border-white/[0.06] bg-[#090B12] p-3.5">
              <div className="text-xs font-medium text-[#949CAF]">Intezam C++ Binary</div>
              <div className="mt-1 text-sm font-semibold text-[#F8FAFC]">
                {engineStatus?.intezam_binary_available ? "Detected" : "Not Found"}
              </div>
              <div className="mt-1 truncate font-mono text-[10.5px] text-[#38BDF8]">
                {engineStatus?.intezam_binary_path || "External OS handler fallback"}
              </div>
            </div>

            <div className="rounded-xl border border-white/[0.06] bg-[#090B12] p-3.5">
              <div className="text-xs font-medium text-[#949CAF]">System Media Players</div>
              <div className="mt-1 text-sm font-semibold text-[#F8FAFC]">
                VLC: {engineStatus?.vlc_available ? "Available" : "Missing"} · MPV:{" "}
                {engineStatus?.mpv_available ? "Available" : "Missing"}
              </div>
              <div className="mt-1 font-mono text-[10.5px] text-[#949CAF]">
                {engineStatus?.rolling_cache_policy || "2.5% rolling cache"}
              </div>
            </div>
          </div>
        </section>
      )}

      {/* Advanced Diagnostics & Logs */}
      {activeTab === "advanced" && (
        <section className="space-y-4 rounded-2xl border border-white/[0.07] bg-[#111522] p-5">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-2">
              <Cpu className="h-4 w-4 text-[#786BFF]" aria-hidden="true" />
              <h3 className="text-sm font-semibold text-[#F8FAFC]">
                Bridge Diagnostics &amp; Logs
              </h3>
            </div>
            <button
              type="button"
              onClick={onRefreshDiagnostics}
              className="inline-flex h-8 items-center gap-1.5 rounded-lg border border-white/10 bg-[#191E2E] px-3 text-xs text-[#F8FAFC]"
            >
              <RefreshCw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Refresh</span>
            </button>
          </div>

          <div className="max-h-64 overflow-y-auto rounded-xl border border-white/[0.06] bg-[#090B12] p-3 font-mono text-[11px] leading-relaxed text-[#949CAF]">
            {appLogs.length === 0 ? (
              <div>No bridge log entries recorded yet.</div>
            ) : (
              appLogs.map((line, i) => (
                <div key={i} className="break-all">
                  {line}
                </div>
              ))
            )}
          </div>
        </section>
      )}
    </m.div>
  );
}
