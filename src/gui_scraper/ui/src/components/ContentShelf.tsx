import { ChevronLeft, ChevronRight } from "lucide-react";
import { useRef } from "react";
import type { MediaEntry } from "../lib/types";
import { PosterCard } from "./PosterCard";

interface ContentShelfProps {
  title: string;
  subtitle?: string;
  items: MediaEntry[];
  libraryKeys: Set<string>;
  onSelectMedia: (media: MediaEntry) => void;
  onToggleLibrary: (media: MediaEntry) => void;
  onViewAll?: () => void;
}

export function ContentShelf({
  title,
  subtitle,
  items,
  libraryKeys,
  onSelectMedia,
  onToggleLibrary,
  onViewAll,
}: ContentShelfProps) {
  const scrollerRef = useRef<HTMLDivElement | null>(null);

  if (items.length === 0) return null;

  const scrollBy = (direction: -1 | 1) => {
    const el = scrollerRef.current;
    if (!el) return;
    el.scrollBy({ left: direction * 520, behavior: "smooth" });
  };

  return (
    <section className="space-y-3">
      <div className="flex items-end justify-between gap-4">
        <div>
          <h2 className="text-base font-semibold tracking-tight text-[#F8FAFC]">{title}</h2>
          {subtitle && <p className="mt-0.5 text-xs text-[#949CAF]">{subtitle}</p>}
        </div>

        <div className="flex items-center gap-1.5">
          {onViewAll && (
            <button
              type="button"
              onClick={onViewAll}
              className="mr-1 rounded-lg px-2.5 py-1 text-xs font-medium text-[#949CAF] transition-colors hover:bg-[#191E2E] hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
            >
              View All
            </button>
          )}
          <button
            type="button"
            onClick={() => scrollBy(-1)}
            aria-label={`Scroll ${title} left`}
            className="flex h-7 w-7 items-center justify-center rounded-lg border border-white/10 bg-[#111522] text-[#949CAF] transition-colors hover:bg-[#191E2E] hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
          >
            <ChevronLeft className="h-4 w-4" aria-hidden="true" />
          </button>
          <button
            type="button"
            onClick={() => scrollBy(1)}
            aria-label={`Scroll ${title} right`}
            className="flex h-7 w-7 items-center justify-center rounded-lg border border-white/10 bg-[#111522] text-[#949CAF] transition-colors hover:bg-[#191E2E] hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
          >
            <ChevronRight className="h-4 w-4" aria-hidden="true" />
          </button>
        </div>
      </div>

      <div
        ref={scrollerRef}
        className="no-scrollbar flex gap-4 overflow-x-auto pb-2 pt-1"
      >
        {items.map((media, idx) => (
          <PosterCard
            key={media.key}
            media={media}
            index={idx}
            isSaved={libraryKeys.has(media.key)}
            onSelect={onSelectMedia}
            onToggleLibrary={onToggleLibrary}
            compactWidth
          />
        ))}
      </div>
    </section>
  );
}
