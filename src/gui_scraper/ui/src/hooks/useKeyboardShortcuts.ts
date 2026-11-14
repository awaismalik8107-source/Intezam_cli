import { useEffect } from "react";

interface UseKeyboardShortcutsOptions {
  hasDetailsOpen: boolean;
  onFocusSearch: () => void;
  onOpenSettings: () => void;
  onCloseDetails: () => void;
}

export function useKeyboardShortcuts({
  hasDetailsOpen,
  onFocusSearch,
  onOpenSettings,
  onCloseDetails,
}: UseKeyboardShortcutsOptions): void {
  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      const hasModifier = event.ctrlKey || event.metaKey;
      const key = event.key.toLowerCase();

      if (hasModifier && key === "k") {
        event.preventDefault();
        onFocusSearch();
        return;
      }

      if (hasModifier && key === ",") {
        event.preventDefault();
        onOpenSettings();
        return;
      }

      if (event.key === "Escape" && hasDetailsOpen) {
        event.preventDefault();
        onCloseDetails();
      }
    };

    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [hasDetailsOpen, onFocusSearch, onOpenSettings, onCloseDetails]);
}
