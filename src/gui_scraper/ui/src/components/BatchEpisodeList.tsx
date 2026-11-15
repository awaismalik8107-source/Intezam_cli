import {
  AlertTriangle,
  CheckCircle2,
  FileArchive,
  FileText,
  Film,
  HelpCircle,
  Info,
  Loader2,
  RefreshCw,
  ShieldAlert,
  Subtitles,
} from "lucide-react";
import { useEffect, useId, useMemo, useState, type KeyboardEvent } from "react";
import {
  type BatchEntry,
  buildBatchOfficialCatalog,
  type FileEpisodeMatchConfidence,
  formatSeasonLabel,
  type MatchedManifestFile,
  matchManifestFilesToEpisodes,
} from "../lib/episodeCatalog";
import type {
  ManifestFileType,
  SeasonMetadata,
  TorrentManifestInspectionResult,
} from "../lib/types";

export interface BatchInspectionState {
  status: "idle" | "loading" | "done";
  result?: TorrentManifestInspectionResult;
  error?: string;
}

interface BatchEpisodeListProps {
  panelId: string;
  batch: BatchEntry;
  officialSeasons?: SeasonMetadata[];
  metadataProvider?: string;
  activeTab: "official" | "verified";
  onChangeTab: (tab: "official" | "verified") => void;
  selectedSeason: number | null;
  onSelectSeason: (season: number) => void;
  inspectionState: BatchInspectionState;
  onInspectManifest: (forceRefresh?: boolean) => void;
}

type FileTypeFilter = "all" | ManifestFileType;

function fileTypeBadge(fileType: ManifestFileType) {
  if (fileType === "video") {
    return {
      label: "Video",
      Icon: Film,
      className: "border-[#786BFF]/40 bg-[#786BFF]/15 text-[#DAD6FF]",
    };
  }
  if (fileType === "subtitle") {
    return {
      label: "Subtitle",
      Icon: Subtitles,
      className: "border-[#38BDF8]/40 bg-[#38BDF8]/10 text-[#7DD3FC]",
    };
  }
  return {
    label: "Auxiliary",
    Icon: FileText,
    className: "border-white/15 bg-white/5 text-[#AAB5CA]",
  };
}

function matchConfidenceBadge(
  confidence: FileEpisodeMatchConfidence,
  season: number | null,
  epStart: number | null,
  epEnd: number | null,
) {
  const epCode =
    season !== null && epStart !== null
      ? season === 0
        ? `Specials · E${String(epStart).padStart(2, "0")}${epEnd ? `–E${String(epEnd).padStart(2, "0")}` : ""}`
        : `S${String(season).padStart(2, "0")}E${String(epStart).padStart(2, "0")}${epEnd ? `–E${String(epEnd).padStart(2, "0")}` : ""}`
      : null;

  if (confidence === "confirmed") {
    return {
      label: epCode ? `Confirmed · ${epCode}` : "Confirmed",
      Icon: CheckCircle2,
      className: "border-emerald-400/35 bg-emerald-400/10 text-emerald-300",
    };
  }
  if (confidence === "possible") {
    return {
      label: epCode ? `Possible · ${epCode}` : "Possible match",
      Icon: HelpCircle,
      className: "border-amber-400/35 bg-amber-400/10 text-amber-200",
    };
  }
  return {
    label: "Unmatched",
    Icon: FileArchive,
    className: "border-white/10 bg-[#090B12] text-[#949CAF]",
  };
}

function inspectionFooterLabel(state: BatchInspectionState): string {
  if (state.status === "loading") {
    return "Inspecting .torrent manifest...";
  }
  if (state.status === "idle" || !state.result) {
    if (state.error) return `Failed (${state.error})`;
    return "Not performed";
  }
  if (state.result.status === "verified") {
    const cacheTag = state.result.cached ? " · cached" : "";
    return `Verified (${state.result.total_files} files, ${state.result.video_count} video, ${state.result.subtitle_count} subtitle${cacheTag})`;
  }
  if (state.result.status === "unavailable") {
    return "Unavailable (magnet-only release)";
  }
  return `Failed (${state.result.error || state.error || "invalid manifest"})`;
}

export function BatchEpisodeList({
  panelId,
  batch,
  officialSeasons,
  metadataProvider,
  activeTab,
  onChangeTab,
  selectedSeason,
  onSelectSeason,
  inspectionState,
  onInspectManifest,
}: BatchEpisodeListProps) {
  const selectId = useId();
  const officialTabId = `${panelId}-tab-official`;
  const verifiedTabId = `${panelId}-tab-verified`;
  const officialPanelId = `${panelId}-panel-official`;
  const verifiedPanelId = `${panelId}-panel-verified`;

  const [fileFilter, setFileFilter] = useState<FileTypeFilter>("all");

  const officialCatalog = useMemo(
    () => buildBatchOfficialCatalog(batch, officialSeasons, metadataProvider),
    [batch, officialSeasons, metadataProvider],
  );

  // Preserve explicit user season selection as long as that season still exists
  const effectiveSeason: number | null =
    selectedSeason !== null && officialCatalog.seasonNumbers.includes(selectedSeason)
      ? selectedSeason
      : officialCatalog.defaultSeason;

  const activeSeasonGroup = useMemo(
    () =>
      effectiveSeason !== null
        ? (officialCatalog.seasons.find((s) => s.seasonNumber === effectiveSeason) ?? null)
        : null,
    [officialCatalog.seasons, effectiveSeason],
  );

  // Trigger metadata-only .torrent inspection only when the user opens Verified Files (and not yet done)
  useEffect(() => {
    if (activeTab === "verified" && inspectionState.status === "idle") {
      onInspectManifest(false);
    }
  }, [activeTab, inspectionState.status, onInspectManifest]);

  const matchedFiles: MatchedManifestFile[] = useMemo(() => {
    if (inspectionState.result?.status !== "verified") return [];
    return matchManifestFilesToEpisodes(
      inspectionState.result.files,
      batch,
      officialSeasons,
    );
  }, [inspectionState.result, batch, officialSeasons]);

  const filteredFiles = useMemo(() => {
    if (fileFilter === "all") return matchedFiles;
    return matchedFiles.filter((m) => m.fileType === fileFilter);
  }, [matchedFiles, fileFilter]);

  const matchCounts = useMemo(() => {
    let confirmed = 0;
    let possible = 0;
    let unmatched = 0;
    for (const item of matchedFiles) {
      if (item.confidence === "confirmed") confirmed += 1;
      else if (item.confidence === "possible") possible += 1;
      else unmatched += 1;
    }
    return { confirmed, possible, unmatched };
  }, [matchedFiles]);

  const handleTabKeyDown = (event: KeyboardEvent<HTMLButtonElement>) => {
    if (event.key === "ArrowRight" || event.key === "ArrowLeft") {
      event.preventDefault();
      const next = activeTab === "official" ? "verified" : "official";
      onChangeTab(next);
    }
  };

  return (
    <div
      id={panelId}
      className="mt-3 space-y-3 rounded-xl border border-white/10 bg-[#0D111C] p-3.5 sm:p-4"
    >
      <div className="flex flex-wrap items-center justify-between gap-2 border-b border-white/[0.08] pb-2.5">
        <span className="text-[11px] font-bold uppercase tracking-wider text-[#AAB5CA]">
          EPISODE LIST
        </span>

        <div
          role="tablist"
          aria-label="Episode list source"
          className="inline-flex rounded-lg border border-white/10 bg-[#090B12] p-0.5"
        >
          <button
            id={officialTabId}
            type="button"
            role="tab"
            aria-selected={activeTab === "official"}
            aria-controls={officialPanelId}
            tabIndex={activeTab === "official" ? 0 : -1}
            onClick={() => onChangeTab("official")}
            onKeyDown={handleTabKeyDown}
            className={`rounded-md px-3 py-1.5 text-xs font-semibold transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
              activeTab === "official"
                ? "bg-[#786BFF]/25 text-white"
                : "text-[#949CAF] hover:text-white"
            }`}
          >
            Official Episodes
          </button>
          <button
            id={verifiedTabId}
            type="button"
            role="tab"
            aria-selected={activeTab === "verified"}
            aria-controls={verifiedPanelId}
            tabIndex={activeTab === "verified" ? 0 : -1}
            onClick={() => onChangeTab("verified")}
            onKeyDown={handleTabKeyDown}
            className={`rounded-md px-3 py-1.5 text-xs font-semibold transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
              activeTab === "verified"
                ? "bg-[#38BDF8]/20 text-white"
                : "text-[#949CAF] hover:text-white"
            }`}
          >
            Verified Files
          </button>
        </div>
      </div>

      {/* Tab 1: Official Episodes */}
      {activeTab === "official" && (
        <div
          id={officialPanelId}
          role="tabpanel"
          aria-labelledby={officialTabId}
          className="space-y-3"
        >
          <div
            className={`flex items-start gap-2 rounded-lg border px-3 py-2 text-xs ${
              officialCatalog.coverageUnverified
                ? "border-amber-400/30 bg-amber-400/10 text-amber-100"
                : "border-white/[0.08] bg-[#111522] text-[#C8D0E2]"
            }`}
          >
            {officialCatalog.coverageUnverified ? (
              <AlertTriangle className="mt-0.5 h-3.5 w-3.5 shrink-0 text-amber-300" aria-hidden="true" />
            ) : (
              <Info className="mt-0.5 h-3.5 w-3.5 shrink-0 text-[#786BFF]" aria-hidden="true" />
            )}
            <span>{officialCatalog.coverageNotice}</span>
          </div>

          {officialCatalog.seasons.length > 0 ? (
            <>
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="flex items-center gap-2">
                  <label htmlFor={selectId} className="text-xs font-medium text-[#949CAF]">
                    Season:
                  </label>
                  <select
                    id={selectId}
                    aria-label="Select official season"
                    value={effectiveSeason ?? ""}
                    onChange={(e) => onSelectSeason(Number(e.target.value))}
                    className="rounded-lg border border-white/15 bg-[#111522] px-2.5 py-1.5 text-xs font-medium text-white focus:border-[#786BFF] focus:outline-none focus:ring-2 focus:ring-[#786BFF]"
                  >
                    {officialCatalog.seasons.map((s) => (
                      <option key={s.seasonNumber} value={s.seasonNumber} className="bg-[#090B12]">
                        {formatSeasonLabel(s.seasonNumber)} ({s.episodes.length} eps)
                      </option>
                    ))}
                  </select>
                </div>

                {activeSeasonGroup && (
                  <span className="text-[11px] text-[#949CAF]">
                    {activeSeasonGroup.label}
                    {activeSeasonGroup.airYear ? ` · ${activeSeasonGroup.airYear}` : ""}
                    {activeSeasonGroup.providerTitle &&
                    activeSeasonGroup.providerTitle.toLowerCase() !==
                      activeSeasonGroup.label.toLowerCase()
                      ? ` · ${activeSeasonGroup.providerTitle}`
                      : ""}
                  </span>
                )}
              </div>

              {activeSeasonGroup && activeSeasonGroup.episodes.length > 0 ? (
                <ol className="max-h-72 space-y-1.5 overflow-y-auto pr-1">
                  {activeSeasonGroup.episodes.map((ep) => (
                    <li
                      key={`${ep.seasonNumber}:${ep.episodeNumber}`}
                      className="flex items-start gap-3 rounded-lg border border-white/[0.06] bg-[#131826] px-3 py-2 text-xs"
                    >
                      {ep.stillUrl && (
                        <img
                          src={ep.stillUrl}
                          alt=""
                          loading="lazy"
                          className="h-10 w-16 shrink-0 rounded object-cover opacity-90"
                        />
                      )}
                      <div className="min-w-0 flex-1 space-y-0.5">
                        <div className="flex flex-wrap items-baseline justify-between gap-2">
                          <p className="font-medium text-white">
                            <span className="mr-1.5 font-mono text-[#AFA6FF]">
                              E{String(ep.episodeNumber).padStart(2, "0")}
                            </span>
                            <span className="text-[#64748B]">—</span>{" "}
                            <span
                              className={
                                ep.titleAvailable ? "text-white" : "italic text-[#949CAF]"
                              }
                            >
                              {ep.title}
                            </span>
                          </p>
                          <div className="flex items-center gap-2 text-[11px] text-[#949CAF]">
                            <span>{formatSeasonLabel(ep.seasonNumber)}</span>
                            {ep.airDate && <span className="font-mono">{ep.airDate}</span>}
                          </div>
                        </div>
                        {ep.overview && (
                          <p className="line-clamp-2 text-[11px] leading-relaxed text-[#AAB5CA]">
                            {ep.overview}
                          </p>
                        )}
                      </div>
                    </li>
                  ))}
                </ol>
              ) : (
                <div className="rounded-lg border border-dashed border-white/15 bg-[#090B12] p-3 text-xs text-[#949CAF]">
                  No official episode entries available for this season.
                </div>
              )}
            </>
          ) : (
            <div className="rounded-lg border border-dashed border-white/15 bg-[#090B12] p-4 text-xs text-[#949CAF]">
              Official season and episode titles are unavailable from the metadata provider for this title.
            </div>
          )}
        </div>
      )}

      {/* Tab 2: Verified Files (.torrent metadata inspection) */}
      {activeTab === "verified" && (
        <div
          id={verifiedPanelId}
          role="tabpanel"
          aria-labelledby={verifiedTabId}
          className="space-y-3"
        >
          <div className="flex flex-wrap items-center justify-between gap-2 rounded-lg border border-white/[0.08] bg-[#111522] px-3 py-2 text-xs text-[#C8D0E2]">
            <span>
              Inspects the .torrent metadata manifest only (no video downloads, peer connections,
              or individual episode magnets).
            </span>
            <button
              type="button"
              disabled={inspectionState.status === "loading"}
              onClick={() => onInspectManifest(true)}
              className="inline-flex items-center gap-1.5 rounded-md border border-[#38BDF8]/35 bg-[#38BDF8]/10 px-2.5 py-1 text-[11px] font-semibold text-[#7DD3FC] transition-colors hover:bg-[#38BDF8]/20 disabled:opacity-50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              <RefreshCw
                className={`h-3 w-3 ${inspectionState.status === "loading" ? "animate-spin" : ""}`}
                aria-hidden="true"
              />
              <span>Refresh Manifest</span>
            </button>
          </div>

          {inspectionState.status === "loading" ? (
            <div
              role="status"
              className="flex items-center gap-2.5 rounded-lg border border-[#38BDF8]/25 bg-[#38BDF8]/10 p-4 text-xs text-[#BAE6FD]"
            >
              <Loader2 className="h-4 w-4 animate-spin" aria-hidden="true" />
              <span>Fetching and validating .torrent metadata manifest...</span>
            </div>
          ) : inspectionState.result?.status === "verified" ? (
            <div className="space-y-2.5">
              {/* Manifest Summary & Filters */}
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="flex flex-wrap items-center gap-1.5 text-[11px]">
                  <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 font-mono text-white">
                    {inspectionState.result.total_files} files · {inspectionState.result.total_size_label}
                  </span>
                  <span className="rounded-md border border-[#786BFF]/35 bg-[#786BFF]/10 px-2 py-0.5 text-[#DAD6FF]">
                    {inspectionState.result.video_count} video
                  </span>
                  <span className="rounded-md border border-[#38BDF8]/35 bg-[#38BDF8]/10 px-2 py-0.5 text-[#7DD3FC]">
                    {inspectionState.result.subtitle_count} subtitle
                  </span>
                  <span className="rounded-md border border-white/10 bg-white/5 px-2 py-0.5 text-[#AAB5CA]">
                    {inspectionState.result.auxiliary_count} auxiliary
                  </span>
                  <span className="rounded-md border border-emerald-400/30 bg-emerald-400/10 px-2 py-0.5 text-emerald-300">
                    {matchCounts.confirmed} confirmed
                  </span>
                  {matchCounts.possible > 0 && (
                    <span className="rounded-md border border-amber-400/30 bg-amber-400/10 px-2 py-0.5 text-amber-200">
                      {matchCounts.possible} possible
                    </span>
                  )}
                  {matchCounts.unmatched > 0 && (
                    <span className="rounded-md border border-white/10 bg-[#090B12] px-2 py-0.5 text-[#949CAF]">
                      {matchCounts.unmatched} unmatched
                    </span>
                  )}
                </div>

                <div className="flex items-center gap-1 text-[11px]">
                  {(["all", "video", "subtitle", "auxiliary"] as const).map((kind) => (
                    <button
                      key={kind}
                      type="button"
                      onClick={() => setFileFilter(kind)}
                      className={`rounded-md border px-2 py-0.5 capitalize transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                        fileFilter === kind
                          ? "border-[#786BFF]/60 bg-[#786BFF]/20 text-white"
                          : "border-white/10 bg-[#090B12] text-[#949CAF] hover:text-white"
                      }`}
                    >
                      {kind}
                    </button>
                  ))}
                </div>
              </div>

              <ul className="max-h-80 space-y-1.5 overflow-y-auto pr-1">
                {filteredFiles.map((entry) => {
                  const typeBadge = fileTypeBadge(entry.fileType);
                  const confBadge = matchConfidenceBadge(
                    entry.confidence,
                    entry.matchedSeason,
                    entry.matchedEpisode,
                    entry.matchedEpisodeEnd,
                  );
                  const TypeIcon = typeBadge.Icon;
                  const ConfIcon = confBadge.Icon;

                  return (
                    <li
                      key={entry.file.relative_path}
                      className="space-y-1.5 rounded-lg border border-white/[0.06] bg-[#131826] p-2.5 text-xs"
                    >
                      <div className="flex flex-wrap items-start justify-between gap-2">
                        <div className="min-w-0 flex-1">
                          <p className="break-all font-medium text-white">{entry.file.filename}</p>
                          {entry.file.relative_path !== entry.file.filename && (
                            <p className="mt-0.5 break-all font-mono text-[11px] text-[#7E89A1]">
                              {entry.file.relative_path}
                            </p>
                          )}
                        </div>
                        <span className="shrink-0 font-mono text-[11px] text-[#E2E8F0]">
                          {entry.file.size_label}
                        </span>
                      </div>

                      <div className="flex flex-wrap items-center gap-1.5 text-[11px]">
                        <span
                          className={`inline-flex items-center gap-1 rounded border px-1.5 py-0.5 font-medium ${typeBadge.className}`}
                        >
                          <TypeIcon className="h-3 w-3" aria-hidden="true" />
                          {typeBadge.label}
                        </span>

                        <span
                          className={`inline-flex items-center gap-1 rounded border px-1.5 py-0.5 font-medium ${confBadge.className}`}
                        >
                          <ConfIcon className="h-3 w-3" aria-hidden="true" />
                          {confBadge.label}
                        </span>

                        {entry.officialEpisodeTitle && (
                          <span className="text-[#C8D0E2]">
                            — {entry.officialEpisodeTitle}
                          </span>
                        )}
                      </div>

                      <p className="text-[11px] text-[#949CAF]">{entry.reason}</p>
                    </li>
                  );
                })}
              </ul>
            </div>
          ) : inspectionState.result?.status === "unavailable" ? (
            <div className="flex items-start gap-2.5 rounded-lg border border-amber-400/30 bg-amber-400/10 p-3.5 text-xs text-amber-100">
              <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0 text-amber-300" aria-hidden="true" />
              <div className="space-y-1">
                <p className="font-semibold">Verified file manifest unavailable</p>
                <p className="text-amber-100/90">
                  {inspectionState.result.inspection_note ||
                    "No .torrent metadata URL is attached to this magnet release."}
                </p>
              </div>
            </div>
          ) : (
            <div
              role="alert"
              className="flex flex-wrap items-center justify-between gap-3 rounded-lg border border-rose-400/30 bg-rose-400/10 p-3.5 text-xs text-rose-100"
            >
              <div className="flex items-start gap-2.5">
                <ShieldAlert className="mt-0.5 h-4 w-4 shrink-0 text-rose-300" aria-hidden="true" />
                <div className="space-y-0.5">
                  <p className="font-semibold">Torrent metadata inspection failed</p>
                  <p className="text-rose-100/90">
                    {inspectionState.result?.error ||
                      inspectionState.error ||
                      "Unable to inspect torrent manifest."}
                  </p>
                </div>
              </div>
              <button
                type="button"
                onClick={() => onInspectManifest(true)}
                className="rounded-md border border-rose-300/35 px-2.5 py-1 font-semibold text-rose-100 hover:bg-rose-400/20"
              >
                Retry
              </button>
            </div>
          )}
        </div>
      )}

      {/* Honest provenance footer */}
      <div className="flex flex-wrap items-center justify-between gap-x-4 gap-y-1 border-t border-white/[0.06] pt-2 text-[11px] text-[#949CAF]">
        <span>Official metadata provider: {officialCatalog.metadataProvider}</span>
        <span>Torrent file inspection: {inspectionFooterLabel(inspectionState)}</span>
      </div>
    </div>
  );
}
