import {
  ArrowDown,
  ArrowUp,
  ArrowUpDown,
  Copy,
  Download,
  ExternalLink,
  Magnet,
} from "lucide-react";
import { useMemo, useState } from "react";
import { parseSizeBytes } from "../lib/mediaAdapter";
import type { ScrapedItem } from "../lib/types";
import { StatusBadge } from "./StatusBadge";

type GridSortKey = "id" | "title" | "size" | "date" | "seeders" | "leechers" | "downloads";
type SortDirection = "asc" | "desc";

interface TorrentDataGridProps {
  items: ScrapedItem[];
  isLoading: boolean;
  selectedId: number | null;
  onSelectRow: (item: ScrapedItem) => void;
  onCopyMagnet: (item: ScrapedItem) => void;
  onOpenTorrent: (item: ScrapedItem) => void;
}

const COLUMNS: ReadonlyArray<{
  key: GridSortKey;
  label: string;
  align?: "left" | "right";
}> = [
  { key: "id", label: "#", align: "left" },
  { key: "title", label: "Title", align: "left" },
  { key: "size", label: "Size", align: "right" },
  { key: "date", label: "Date", align: "left" },
  { key: "seeders", label: "Seeders", align: "right" },
  { key: "leechers", label: "Leechers", align: "right" },
  { key: "downloads", label: "Downloads", align: "right" },
];

export function TorrentDataGrid({
  items,
  isLoading,
  selectedId,
  onSelectRow,
  onCopyMagnet,
  onOpenTorrent,
}: TorrentDataGridProps) {
  const [sortKey, setSortKey] = useState<GridSortKey>("id");
  const [sortDir, setSortDir] = useState<SortDirection>("asc");

  const handleSort = (key: GridSortKey) => {
    if (sortKey === key) {
      setSortDir((prev) => (prev === "asc" ? "desc" : "asc"));
    } else {
      setSortKey(key);
      setSortDir(key === "seeders" || key === "downloads" || key === "size" || key === "date" ? "desc" : "asc");
    }
  };

  const sortedItems = useMemo(() => {
    const copy = [...items];
    copy.sort((a, b) => {
      let diff = 0;
      if (sortKey === "id") diff = a.id - b.id;
      else if (sortKey === "title") diff = a.title.localeCompare(b.title);
      else if (sortKey === "size") diff = parseSizeBytes(a.size) - parseSizeBytes(b.size);
      else if (sortKey === "date") diff = a.date.localeCompare(b.date);
      else if (sortKey === "seeders") diff = (a.seeders ?? 0) - (b.seeders ?? 0);
      else if (sortKey === "leechers") diff = (a.leechers ?? 0) - (b.leechers ?? 0);
      else if (sortKey === "downloads") diff = (a.downloads ?? 0) - (b.downloads ?? 0);
      return sortDir === "asc" ? diff : -diff;
    });
    return copy;
  }, [items, sortDir, sortKey]);

  return (
    <div className="overflow-hidden rounded-2xl border border-white/[0.08] bg-[#111522]/90 shadow-xl shadow-black/30">
      <div className="overflow-x-auto">
        <table className="w-full border-collapse text-left text-xs">
          <thead>
            <tr className="border-b border-white/[0.07] bg-[#0B0D16]/90 text-[10px] font-medium uppercase tracking-wider text-[#949CAF]">
              {COLUMNS.map((col) => {
                const active = sortKey === col.key;
                return (
                  <th
                    key={col.key}
                    scope="col"
                    className={`px-1.5 py-2.5 ${col.align === "right" ? "text-right" : "text-left"}`}
                  >
                    <button
                      type="button"
                      onClick={() => handleSort(col.key)}
                      className={`inline-flex items-center gap-0.5 rounded px-0.5 py-0.5 transition-colors hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF] ${
                        col.align === "right" ? "flex-row-reverse" : ""
                      } ${active ? "text-[#F8FAFC]" : ""}`}
                    >
                      <span>{col.label}</span>
                      {active ? (
                        sortDir === "asc" ? (
                          <ArrowUp className="h-2.5 w-2.5 text-[#38BDF8]" aria-hidden="true" />
                        ) : (
                          <ArrowDown className="h-2.5 w-2.5 text-[#38BDF8]" aria-hidden="true" />
                        )
                      ) : (
                        <ArrowUpDown className="h-2.5 w-2.5 opacity-45" aria-hidden="true" />
                      )}
                    </button>
                  </th>
                );
              })}
              <th scope="col" className="px-2 py-2.5 text-right">
                Quick Actions
              </th>
            </tr>
          </thead>

          <tbody className="divide-y divide-white/[0.05]">
            {isLoading ? (
              Array.from({ length: 7 }, (_, idx) => (
                <tr key={`skeleton-${idx}`} aria-busy="true">
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer h-3.5 w-5 rounded" />
                  </td>
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer h-4 w-4/5 rounded" />
                  </td>
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer ml-auto h-3.5 w-12 rounded" />
                  </td>
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer h-3.5 w-20 rounded" />
                  </td>
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer ml-auto h-3.5 w-8 rounded" />
                  </td>
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer ml-auto h-3.5 w-8 rounded" />
                  </td>
                  <td className="px-1.5 py-2.5">
                    <div className="skeleton-shimmer ml-auto h-3.5 w-9 rounded" />
                  </td>
                  <td className="px-2 py-2.5">
                    <div className="skeleton-shimmer ml-auto h-7 w-16 rounded-lg" />
                  </td>
                </tr>
              ))
            ) : sortedItems.length === 0 ? (
              <tr>
                <td colSpan={8} className="px-6 py-12 text-center text-xs text-[#949CAF]">
                  No table rows extracted yet. Run a Nyaa browse or search query above.
                </td>
              </tr>
            ) : (
              sortedItems.map((item) => {
                const isSelected = selectedId === item.id;
                const hasMagnet = Boolean(item.magnet_link);
                const hasTorrent = Boolean(item.torrent_link);

                return (
                  <tr
                    key={`${item.id}-${item.title}`}
                    data-grid-row={item.id}
                    onClick={() => onSelectRow(item)}
                    tabIndex={0}
                    onKeyDown={(e) => {
                      if (e.key === "Enter" || e.key === " ") {
                        e.preventDefault();
                        onSelectRow(item);
                      }
                    }}
                    className={`group cursor-pointer transition-colors duration-75 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-[#786BFF] ${
                      isSelected
                        ? "bg-[#786BFF]/14"
                        : "hover:bg-[#191E2E]"
                    }`}
                  >
                    {/* # */}
                    <td className="whitespace-nowrap px-1.5 py-2.5 font-mono text-[11px] text-[#949CAF]">
                      {item.id}
                    </td>

                    {/* Title */}
                    <td className="w-full max-w-0 px-1.5 py-2.5">
                      <div className="flex min-w-0 items-center gap-1.5">
                        <span
                          title={item.title}
                          className="truncate font-medium text-[#F8FAFC] transition-colors group-hover:text-white"
                        >
                          {item.title}
                        </span>
                        {!item.is_valid && (
                          <StatusBadge isValid={item.is_valid} message={item.verification_msg} />
                        )}
                      </div>
                    </td>

                    {/* Size */}
                    <td className="whitespace-nowrap px-1.5 py-2.5 text-right font-mono text-[10.5px] text-[#F8FAFC]">
                      {item.size}
                    </td>

                    {/* Date with relative time tooltip */}
                    <td className="whitespace-nowrap px-1.5 py-2.5">
                      <span
                        title={item.relative_date || item.date}
                        className="inline-flex cursor-help items-center font-mono text-[10.5px] text-[#949CAF] underline decoration-white/15 decoration-dotted underline-offset-4"
                      >
                        {item.date}
                      </span>
                    </td>

                    {/* Seeders (emerald green) */}
                    <td className="whitespace-nowrap px-1.5 py-2.5 text-right font-mono text-[11px] font-semibold text-emerald-400">
                      {(item.seeders ?? 0).toLocaleString()}
                    </td>

                    {/* Leechers (rose/amber) */}
                    <td className="whitespace-nowrap px-1.5 py-2.5 text-right font-mono text-[11px] font-semibold text-amber-400">
                      {(item.leechers ?? 0).toLocaleString()}
                    </td>

                    {/* Downloads */}
                    <td className="whitespace-nowrap px-1.5 py-2.5 text-right font-mono text-[10.5px] text-[#949CAF]">
                      {(item.downloads ?? 0).toLocaleString()}
                    </td>

                    {/* Quick Actions */}
                    <td
                      className="whitespace-nowrap px-2 py-2 text-right"
                      onClick={(e) => e.stopPropagation()}
                    >
                      <div className="inline-flex items-center justify-end gap-1">
                        <button
                          type="button"
                          disabled={!hasMagnet}
                          title={hasMagnet ? "Copy Magnet URI" : "No magnet URI available"}
                          aria-label={`Copy magnet for ${item.title}`}
                          onClick={() => onCopyMagnet(item)}
                          className="inline-flex h-7 items-center gap-1 rounded-lg border border-[#786BFF]/35 bg-[#786BFF]/15 px-1.5 text-[10.5px] font-medium text-[#F8FAFC] transition-colors hover:bg-[#786BFF]/30 disabled:cursor-not-allowed disabled:opacity-35"
                        >
                          <Magnet className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
                          <Copy className="h-2.5 w-2.5" aria-hidden="true" />
                        </button>

                        <button
                          type="button"
                          disabled={!hasTorrent}
                          title={hasTorrent ? "Download .torrent file" : "No .torrent link available"}
                          aria-label={`Download .torrent for ${item.title}`}
                          onClick={() => onOpenTorrent(item)}
                          className="inline-flex h-7 items-center gap-1 rounded-lg border border-white/10 bg-[#191E2E] px-1.5 text-[10.5px] font-medium text-[#F8FAFC] transition-colors hover:border-[#38BDF8]/45 hover:bg-[#1E2538] disabled:cursor-not-allowed disabled:opacity-35"
                        >
                          <Download className="h-3 w-3 text-[#38BDF8]" aria-hidden="true" />
                          <span>.torrent</span>
                        </button>

                        {item.view_url && (
                          <button
                            type="button"
                            title="Inspect row details"
                            aria-label={`Inspect row for ${item.title}`}
                            onClick={() => onSelectRow(item)}
                            className="hidden h-7 w-7 items-center justify-center rounded-lg border border-white/10 bg-[#0B0D16] text-[#949CAF] transition-colors hover:text-[#F8FAFC] xl:inline-flex"
                          >
                            <ExternalLink className="h-3 w-3" aria-hidden="true" />
                          </button>
                        )}
                      </div>
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
