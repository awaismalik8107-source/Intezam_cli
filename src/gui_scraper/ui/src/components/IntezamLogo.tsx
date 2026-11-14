export function IntezamLogo({ size = 28 }: { size?: number }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 32 32"
      fill="none"
      aria-hidden="true"
      className="shrink-0"
    >
      <defs>
        <linearGradient id="intezam-stream-grad" x1="4" y1="6" x2="28" y2="26" gradientUnits="userSpaceOnUse">
          <stop offset="0%" stopColor="#786BFF" />
          <stop offset="100%" stopColor="#38BDF8" />
        </linearGradient>
      </defs>
      <rect
        x="1"
        y="1"
        width="30"
        height="30"
        rx="8.5"
        fill="#111522"
        stroke="rgba(120, 107, 255, 0.35)"
        strokeWidth="1.2"
      />
      <path
        d="M8.5 11.5C11.2 9.2 14.5 8.5 18 9.2C20.5 9.7 22.5 11 23.8 12.6"
        stroke="url(#intezam-stream-grad)"
        strokeWidth="1.8"
        strokeLinecap="round"
      />
      <path
        d="M8.2 20.5C10.5 22.4 13.7 23.2 17 22.7C19.8 22.3 22.2 20.9 23.8 19.2"
        stroke="#786BFF"
        strokeOpacity="0.55"
        strokeWidth="1.6"
        strokeLinecap="round"
      />
      <path
        d="M13 12.4L20.4 16L13 19.6V12.4Z"
        fill="#F8FAFC"
      />
    </svg>
  );
}
