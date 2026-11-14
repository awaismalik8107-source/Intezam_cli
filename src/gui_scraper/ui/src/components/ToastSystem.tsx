import { AlertTriangle, CheckCircle2, Info, X, XCircle } from "lucide-react";
import { AnimatePresence, m } from "motion/react";
import { TOAST_VARIANTS } from "../lib/motion";
import type { ToastMessage, ToastType } from "../lib/types";

interface ToastSystemProps {
  toasts: ToastMessage[];
  onDismiss: (id: string) => void;
}

const TOAST_STYLES: Record<
  ToastType,
  { border: string; iconColor: string; Icon: typeof CheckCircle2 }
> = {
  success: {
    border: "border-emerald-400/30",
    iconColor: "text-emerald-400",
    Icon: CheckCircle2,
  },
  error: {
    border: "border-rose-400/30",
    iconColor: "text-rose-400",
    Icon: XCircle,
  },
  warning: {
    border: "border-amber-400/30",
    iconColor: "text-amber-400",
    Icon: AlertTriangle,
  },
  info: {
    border: "border-sky-400/30",
    iconColor: "text-sky-400",
    Icon: Info,
  },
};

export function ToastSystem({ toasts, onDismiss }: ToastSystemProps) {
  return (
    <div
      aria-live="polite"
      aria-label="Notifications"
      className="pointer-events-none fixed right-5 top-5 z-50 flex w-full max-w-sm flex-col gap-2"
    >
      <AnimatePresence mode="popLayout">
        {toasts.map((toast) => {
          const style = TOAST_STYLES[toast.type];
          const Icon = style.Icon;
          return (
            <m.div
              key={toast.id}
              layout="position"
              variants={TOAST_VARIANTS}
              initial="hidden"
              animate="visible"
              exit="exit"
              role="status"
              className={`pointer-events-auto flex items-start gap-2.5 rounded-xl border ${style.border} bg-[#111522] px-3.5 py-2.5 shadow-[0_18px_40px_-12px_rgba(0,0,0,0.8)]`}
            >
              <Icon className={`mt-0.5 h-4 w-4 shrink-0 ${style.iconColor}`} aria-hidden="true" />
              <span className="flex-1 text-xs font-medium leading-relaxed text-[#F8FAFC]">
                {toast.message}
              </span>
              <button
                type="button"
                aria-label="Dismiss notification"
                onClick={() => onDismiss(toast.id)}
                className="rounded p-0.5 text-[#949CAF] transition-colors hover:bg-white/10 hover:text-[#F8FAFC] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[#786BFF]"
              >
                <X className="h-3.5 w-3.5" aria-hidden="true" />
              </button>
            </m.div>
          );
        })}
      </AnimatePresence>
    </div>
  );
}
