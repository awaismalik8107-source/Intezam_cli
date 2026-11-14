import { AlertCircle, CheckCircle2, Loader2, XCircle } from "lucide-react";
import { AnimatePresence, m } from "motion/react";
import { stripStatusPrefix } from "../lib/mediaAdapter";
import { BADGE_SWAP_VARIANTS } from "../lib/motion";

interface StatusBadgeProps {
  isValid: boolean;
  message: string;
  isVerifying?: boolean;
}

export function StatusBadge({ isValid, message, isVerifying = false }: StatusBadgeProps) {
  const cleanMsg = stripStatusPrefix(message) || (isValid ? "Valid" : "Invalid");
  const key = isVerifying ? "verifying" : isValid ? `valid-${cleanMsg}` : `invalid-${cleanMsg}`;

  return (
    <AnimatePresence mode="wait" initial={false}>
      {isVerifying ? (
        <m.span
          key="verifying"
          variants={BADGE_SWAP_VARIANTS}
          initial="initial"
          animate="animate"
          exit="exit"
          className="inline-flex items-center gap-1.5 rounded-full border border-amber-400/30 bg-amber-400/10 px-2.5 py-0.5 text-[11px] font-medium text-amber-300"
        >
          <Loader2 className="h-3 w-3 animate-spin" aria-hidden="true" />
          <span>Verifying...</span>
        </m.span>
      ) : isValid ? (
        <m.span
          key={key}
          variants={BADGE_SWAP_VARIANTS}
          initial="initial"
          animate="animate"
          exit="exit"
          title={cleanMsg}
          className="inline-flex max-w-full items-center gap-1.5 rounded-full border border-emerald-400/25 bg-emerald-400/10 px-2.5 py-0.5 text-[11px] font-medium text-emerald-300"
        >
          <CheckCircle2 className="h-3 w-3 shrink-0" aria-hidden="true" />
          <span className="truncate">{cleanMsg}</span>
        </m.span>
      ) : (
        <m.span
          key={key}
          variants={BADGE_SWAP_VARIANTS}
          initial="initial"
          animate="animate"
          exit="exit"
          title={cleanMsg}
          className="inline-flex max-w-full items-center gap-1.5 rounded-full border border-rose-400/25 bg-rose-400/10 px-2.5 py-0.5 text-[11px] font-medium text-rose-300"
        >
          {cleanMsg.toLowerCase().includes("timed out") ? (
            <AlertCircle className="h-3 w-3 shrink-0" aria-hidden="true" />
          ) : (
            <XCircle className="h-3 w-3 shrink-0" aria-hidden="true" />
          )}
          <span className="truncate">{cleanMsg}</span>
        </m.span>
      )}
    </AnimatePresence>
  );
}
