import { useId } from "react";

interface NyaaLogoProps {
  size?: number;
  className?: string;
}

/** Custom inline SVG emblem for the Nyaa extension (blue cat-eared "N" badge). */
export function NyaaLogo({ size = 24, className = "" }: NyaaLogoProps) {
  const uid = useId().replace(/:/g, "");
  const bgId = `nyaa-bg-${uid}`;
  const accentId = `nyaa-accent-${uid}`;

  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 40 40"
      fill="none"
      xmlns="http://www.w3.org/2000/svg"
      className={className}
      aria-hidden="true"
    >
      <defs>
        <linearGradient id={bgId} x1="4" y1="4" x2="36" y2="36" gradientUnits="userSpaceOnUse">
          <stop offset="0%" stopColor="#1D4ED8" />
          <stop offset="55%" stopColor="#3582F7" />
          <stop offset="100%" stopColor="#38BDF8" />
        </linearGradient>
        <linearGradient id={accentId} x1="10" y1="9" x2="30" y2="31" gradientUnits="userSpaceOnUse">
          <stop offset="0%" stopColor="#FFFFFF" />
          <stop offset="100%" stopColor="#DBEAFE" />
        </linearGradient>
      </defs>

      <rect x="2" y="2" width="36" height="36" rx="10" fill={`url(#${bgId})`} />
      <rect x="2.75" y="2.75" width="34.5" height="34.5" rx="9.25" stroke="white" strokeOpacity="0.22" strokeWidth="1.5" />

      {/* Cat ears crowning the N */}
      <path d="M11 15L12.6 8.8L17.2 13.4H11V15Z" fill="#EFF6FF" />
      <path d="M29 15L27.4 8.8L22.8 13.4H29V15Z" fill="#EFF6FF" />

      {/* Crisp geometric N monogram */}
      <path
        d="M11 29.5V13.2H15.2L24.8 24.4V13.2H29V29.5H24.8L15.2 18.3V29.5H11Z"
        fill={`url(#${accentId})`}
      />
    </svg>
  );
}
