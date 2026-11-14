import { Film, Tv } from "lucide-react";
import { getMediaArtPalette } from "../lib/mediaAdapter";
import type { QualityTier } from "../lib/types";

interface PosterArtworkProps {
  title: string;
  year?: string | null;
  quality?: QualityTier;
  mediaType?: "series" | "release";
  variant?: "poster" | "backdrop";
}

export function PosterArtwork({
  title,
  year,
  quality,
  mediaType = "release",
  variant = "poster",
}: PosterArtworkProps) {
  const palette = getMediaArtPalette(title);

  if (variant === "backdrop") {
    return (
      <div
        aria-hidden="true"
        className="absolute inset-0 overflow-hidden"
        style={{
          background: `radial-gradient(ellipse 75% 85% at 72% 28%, ${palette.glow}, transparent 68%), linear-gradient(135deg, ${palette.from} 0%, ${palette.via} 55%, #090B12 100%)`,
        }}
      >
        <svg
          className="absolute inset-0 h-full w-full opacity-25"
          viewBox="0 0 1200 500"
          preserveAspectRatio="none"
          fill="none"
        >
          <path
            d="M-40 340C240 240 480 410 780 280C980 190 1120 220 1260 150"
            stroke={palette.accent}
            strokeOpacity="0.4"
            strokeWidth="1.5"
          />
          <path
            d="M-20 390C280 290 520 450 840 320C1020 245 1140 270 1260 210"
            stroke="#38BDF8"
            strokeOpacity="0.24"
            strokeWidth="1"
          />
        </svg>
        <div className="absolute inset-0 bg-gradient-to-t from-[#090B12] via-[#090B12]/55 to-transparent" />
        <div className="absolute inset-0 bg-gradient-to-r from-[#090B12] via-[#090B12]/70 to-transparent" />
      </div>
    );
  }

  return (
    <div
      className="relative flex h-full w-full flex-col justify-between overflow-hidden p-3.5"
      style={{
        background: `radial-gradient(circle at 75% 18%, ${palette.glow}, transparent 62%), linear-gradient(165deg, ${palette.from} 0%, ${palette.via} 60%, #0D101B 100%)`,
      }}
    >
      {/* Subtle geometric streaming wave motif */}
      <svg
        aria-hidden="true"
        className="pointer-events-none absolute inset-0 h-full w-full opacity-35"
        viewBox="0 0 240 360"
        preserveAspectRatio="none"
        fill="none"
      >
        <path
          d="M-20 235C45 190 115 265 185 215C220 190 245 195 265 180"
          stroke={palette.accent}
          strokeOpacity="0.45"
          strokeWidth="1.2"
        />
        <path
          d="M-20 262C55 218 125 288 195 240C225 220 245 225 265 210"
          stroke="#38BDF8"
          strokeOpacity="0.22"
          strokeWidth="1"
        />
      </svg>

      {/* Top metadata badges */}
      <div className="relative z-10 flex items-center justify-between gap-1.5">
        <span className="inline-flex h-6 w-6 items-center justify-center rounded-md border border-white/10 bg-black/30 text-[#F8FAFC]/80">
          {mediaType === "series" ? (
            <Tv className="h-3.5 w-3.5" aria-hidden="true" />
          ) : (
            <Film className="h-3.5 w-3.5" aria-hidden="true" />
          )}
        </span>

        {quality && quality !== "Other" && (
          <span className="rounded-md border border-white/15 bg-black/65 px-1.5 py-0.5 font-mono text-[10px] font-semibold tracking-wide text-[#F8FAFC]">
            {quality}
          </span>
        )}
      </div>

      {/* Title-driven typographic poster centerpiece */}
      <div className="relative z-10 space-y-1.5">
        <div
          className="h-0.5 w-7 rounded-full"
          style={{ backgroundColor: palette.accent }}
          aria-hidden="true"
        />
        <div className="line-clamp-4 text-[15px] font-semibold leading-snug tracking-tight text-[#F8FAFC] drop-shadow-sm">
          {title}
        </div>
        {year && (
          <div className="font-mono text-[10.5px] text-[#949CAF]">{year}</div>
        )}
      </div>
    </div>
  );
}
