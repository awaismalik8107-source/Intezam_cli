import { ChevronLeft, ChevronRight, Loader2, Plus } from "lucide-react";

interface PaginationBarProps {
  currentPage: number;
  totalPages: number;
  hasNextPage: boolean;
  itemCount: number;
  isLoading: boolean;
  isLoadingMore: boolean;
  onSelectPage: (page: number) => void;
  onLoadMore: () => void;
}

export function PaginationBar({
  currentPage,
  totalPages,
  hasNextPage,
  itemCount,
  isLoading,
  isLoadingMore,
  onSelectPage,
  onLoadMore,
}: PaginationBarProps) {
  if (itemCount === 0 && currentPage <= 1) return null;

  const maxPage = Math.max(totalPages, currentPage);
  const start = Math.max(1, currentPage - 2);
  const end = Math.min(maxPage, start + 4);
  const pageNumbers: number[] = [];
  for (let p = Math.max(1, end - 4); p <= end; p += 1) {
    pageNumbers.push(p);
  }

  return (
    <div className="flex flex-wrap items-center justify-between gap-3 rounded-2xl border border-white/[0.07] bg-[#111522] px-4 py-3">
      <div className="flex items-center gap-2 text-xs text-[#949CAF]">
        <span className="font-mono text-[#F8FAFC]">{itemCount}</span>
        <span>loaded link(s)</span>
        <span>·</span>
        <span className="font-mono">
          Page {currentPage}
          {maxPage > 1 ? ` of ${maxPage}` : ""}
        </span>
      </div>

      <div className="flex flex-wrap items-center gap-2">
        {hasNextPage && (
          <button
            type="button"
            disabled={isLoading || isLoadingMore}
            onClick={onLoadMore}
            className="inline-flex h-8 items-center gap-1.5 rounded-xl border border-[#3582F7]/40 bg-[#3582F7]/15 px-3 text-xs font-semibold text-[#F8FAFC] transition-colors hover:bg-[#3582F7]/25 disabled:cursor-not-allowed disabled:opacity-45"
          >
            {isLoadingMore ? (
              <Loader2 className="h-3.5 w-3.5 animate-spin text-[#38BDF8]" aria-hidden="true" />
            ) : (
              <Plus className="h-3.5 w-3.5 text-[#38BDF8]" aria-hidden="true" />
            )}
            <span>Load Page {currentPage + 1} More</span>
          </button>
        )}

        <div className="flex items-center gap-1 rounded-xl border border-white/[0.08] bg-[#0B0D16] p-1">
          <button
            type="button"
            disabled={currentPage <= 1 || isLoading || isLoadingMore}
            onClick={() => onSelectPage(currentPage - 1)}
            aria-label="Previous page"
            className="inline-flex h-7 items-center gap-1 rounded-lg px-2 text-xs text-[#949CAF] transition-colors hover:bg-[#191E2E] hover:text-[#F8FAFC] disabled:cursor-not-allowed disabled:opacity-35"
          >
            <ChevronLeft className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Prev</span>
          </button>

          {pageNumbers.map((pageNum) => {
            const active = pageNum === currentPage;
            return (
              <button
                key={pageNum}
                type="button"
                disabled={isLoading || isLoadingMore}
                onClick={() => onSelectPage(pageNum)}
                className={`inline-flex h-7 min-w-7 items-center justify-center rounded-lg px-2 font-mono text-xs font-medium transition-colors ${
                  active
                    ? "bg-[#786BFF] text-white"
                    : "text-[#949CAF] hover:bg-[#191E2E] hover:text-[#F8FAFC]"
                }`}
              >
                {pageNum}
              </button>
            );
          })}

          <button
            type="button"
            disabled={!hasNextPage || isLoading || isLoadingMore}
            onClick={() => onSelectPage(currentPage + 1)}
            aria-label="Next page"
            className="inline-flex h-7 items-center gap-1 rounded-lg px-2 text-xs text-[#949CAF] transition-colors hover:bg-[#191E2E] hover:text-[#F8FAFC] disabled:cursor-not-allowed disabled:opacity-35"
          >
            <span>Next</span>
            <ChevronRight className="h-3.5 w-3.5" aria-hidden="true" />
          </button>
        </div>
      </div>
    </div>
  );
}
