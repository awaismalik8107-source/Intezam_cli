import { useEffect, useState } from "react";
import { isBridgeReady } from "../lib/bridge";

export function usePyWebview(): boolean {
  const [ready, setReady] = useState<boolean>(() => isBridgeReady());

  useEffect(() => {
    if (isBridgeReady()) {
      setReady(true);
      return;
    }

    const handleReady = () => setReady(true);
    window.addEventListener("pywebviewready", handleReady);

    const interval = window.setInterval(() => {
      if (isBridgeReady()) {
        setReady(true);
        window.clearInterval(interval);
      }
    }, 80);

    return () => {
      window.removeEventListener("pywebviewready", handleReady);
      window.clearInterval(interval);
    };
  }, []);

  return ready;
}
