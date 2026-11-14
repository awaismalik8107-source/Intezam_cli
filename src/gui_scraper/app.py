"""Standalone pywebview Desktop GUI application and Python <-> JS bridge."""

from __future__ import annotations

import shutil
import subprocess
import sys
import threading
import webbrowser
from collections import deque
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import pyperclip
import webview

if __package__ in (None, ""):
    # Allow running directly as `python src/gui_scraper/app.py`.
    _SRC_PARENT = Path(__file__).resolve().parents[2]
    if str(_SRC_PARENT) not in sys.path:
        sys.path.insert(0, str(_SRC_PARENT))
    from gui_scraper.engine import (
        BASE_URL,
        DATE_SELECTOR,
        DEFAULT_TIMEOUT,
        DEFAULT_USER_AGENT,
        LINK_SELECTOR,
        PROVIDER_NAME,
        QUERY_PARAM,
        ROW_SELECTOR,
        SEARCH_PATH,
        SIZE_SELECTOR,
        STATS_SELECTOR,
        TITLE_SELECTOR,
        ConfigurableTableScraper,
    )
    from gui_scraper.storage import LocalMediaStore
    from gui_scraper.validator import inspect_magnet_uri, verify_http_status
else:
    from .engine import (
        BASE_URL,
        DATE_SELECTOR,
        DEFAULT_TIMEOUT,
        DEFAULT_USER_AGENT,
        LINK_SELECTOR,
        PROVIDER_NAME,
        QUERY_PARAM,
        ROW_SELECTOR,
        SEARCH_PATH,
        SIZE_SELECTOR,
        STATS_SELECTOR,
        TITLE_SELECTOR,
        ConfigurableTableScraper,
    )
    from .storage import LocalMediaStore
    from .validator import inspect_magnet_uri, verify_http_status

UI_INDEX_PATH = Path(__file__).resolve().parent / "ui" / "dist" / "index.html"
_REPO_ROOT = Path(__file__).resolve().parents[2]


def _find_intezam_binary() -> Path | None:
    """Locate a compiled Intezam C++ binary without altering files outside ``src/gui_scraper``."""
    candidates = [
        _REPO_ROOT / "build" / "intezam",
        _REPO_ROOT / "build-cli" / "intezam",
        _REPO_ROOT / "intezam",
    ]
    for candidate in candidates:
        if candidate.is_file() and candidate.stat().st_mode & 0o111:
            return candidate
    which_path = shutil.which("intezam")
    return Path(which_path) if which_path else None


class ScraperBridge:
    """Thread-safe Python API bridge exposed to the pywebview frontend via ``js_api``."""

    def __init__(
        self,
        scraper: ConfigurableTableScraper | None = None,
        store: LocalMediaStore | None = None,
    ) -> None:
        self.scraper = scraper or ConfigurableTableScraper(demo_mode=True)
        self.store = store or LocalMediaStore()
        self._lock = threading.Lock()
        self._stream_lock = threading.Lock()
        self._logs: deque[str] = deque(maxlen=150)
        self._stream_proc: subprocess.Popen[str] | None = None
        self._stream_state: dict[str, Any] = {
            "active": False,
            "stage": "idle",
            "message": "Idle",
            "url": "",
            "torrent_name": "",
            "selected_video": "",
            "video_size": "",
            "detected_videos": 0,
            "strict_cache": False,
            "returncode": None,
            "lines": [],
        }
        self._log("Intezam bridge initialized.")

    def _log(self, message: str) -> None:
        stamp = datetime.now(timezone.utc).strftime("%H:%M:%S")
        self._logs.append(f"[{stamp}] {message}")

    def search_paginated(
        self,
        query: str = "",
        mode: str = "mock",
        page: int = 1,
    ) -> dict[str, Any]:
        """Run a paginated Nyaa browse or search and return items + pagination metadata."""
        normalized_mode = (mode or "mock").strip().lower()
        use_demo = normalized_mode in {"mock", "mock data", "demo"}
        mode_key = "mock" if use_demo else "live"
        page_num = max(1, int(page or 1))

        response = self.scraper.search(query, demo_mode=use_demo, page=page_num)

        if response.error:
            self._log(f"Search error ({mode_key}, p={page_num}) for {query!r}: {response.error}")
            raise RuntimeError(response.error)

        items = [asdict(item) for item in response.items]
        if page_num == 1:
            self.store.record_search(query, mode_key, len(items))
        self._log(
            f"Search ({mode_key}, p={page_num}/{response.total_pages}) for {query!r} returned {len(items)} source(s)."
        )
        return {
            "items": items,
            "page": response.page,
            "has_next_page": response.has_next_page,
            "total_pages": response.total_pages,
            "request_url": response.request_url,
        }

    def search(self, query: str = "", mode: str = "mock", page: int = 1) -> list[dict[str, Any]]:
        """Run a mock or live table scrape (or default browse when query is empty) and return serialized ``ScrapedItem`` dicts."""
        return self.search_paginated(query=query, mode=mode, page=page)["items"]

    def verify_url(self, url: str) -> dict[str, Any]:
        """Verify a Magnet URI's BTIH hash or send an HTTP HEAD check with a 5s timeout."""
        cleaned = (url or "").strip()
        if not cleaned:
            return {
                "url": "",
                "is_valid": False,
                "status": "invalid",
                "message": "Empty URI cannot be verified.",
            }

        if cleaned.lower().startswith("magnet:"):
            is_valid, message = inspect_magnet_uri(cleaned)
            self._log(f"Magnet verify -> {is_valid}: {message}")
            return {
                "url": cleaned,
                "is_valid": is_valid,
                "status": "valid" if is_valid else "invalid",
                "message": message,
            }

        with self._lock:
            headers = {"User-Agent": self.scraper.user_agent}

        is_valid, message = verify_http_status(cleaned, timeout=5.0, headers=headers)
        self._log(f"HTTP HEAD {cleaned} -> {is_valid}: {message}")
        return {
            "url": cleaned,
            "is_valid": is_valid,
            "status": "valid" if is_valid else "invalid",
            "message": message,
        }

    def get_config(self) -> dict[str, Any]:
        """Return the current scraper URL, selector, and HTTP configuration."""
        with self._lock:
            return {
                "provider_name": self.scraper.provider_name,
                "base_url": self.scraper.base_url,
                "search_path": self.scraper.search_path,
                "query_param": self.scraper.query_param,
                "row_selector": self.scraper.row_selector,
                "title_selector": self.scraper.title_selector,
                "link_selector": self.scraper.link_selector,
                "size_selector": self.scraper.size_selector,
                "date_selector": self.scraper.date_selector,
                "stats_selector": self.scraper.stats_selector,
                "timeout": self.scraper.timeout,
                "user_agent": self.scraper.user_agent,
            }

    def save_config(self, new_config: dict[str, Any]) -> dict[str, Any]:
        """Validate and apply updated target URL and CSS selector settings."""
        if not isinstance(new_config, dict):
            return {"ok": False, "error": "Configuration payload must be a dictionary."}

        base_url = str(new_config.get("base_url", self.scraper.base_url)).strip()
        row_selector = str(new_config.get("row_selector", self.scraper.row_selector)).strip()
        timeout_raw = new_config.get("timeout", self.scraper.timeout)

        if not base_url.startswith(("http://", "https://")):
            return {"ok": False, "error": "BASE_URL must start with http:// or https://"}
        if not row_selector:
            return {"ok": False, "error": "ROW_SELECTOR cannot be empty."}

        try:
            timeout_val = float(timeout_raw)
            if timeout_val <= 0:
                raise ValueError("Timeout must be greater than zero.")
        except (TypeError, ValueError):
            return {"ok": False, "error": "Timeout must be a positive number."}

        with self._lock:
            self.scraper.provider_name = (
                str(new_config.get("provider_name", self.scraper.provider_name)).strip()
                or PROVIDER_NAME
            )
            self.scraper.base_url = base_url
            self.scraper.search_path = (
                str(new_config.get("search_path", self.scraper.search_path)).strip() or "/"
            )
            self.scraper.query_param = str(
                new_config.get("query_param", self.scraper.query_param)
            ).strip()
            self.scraper.row_selector = row_selector
            self.scraper.title_selector = str(
                new_config.get("title_selector", self.scraper.title_selector)
            ).strip()
            self.scraper.link_selector = str(
                new_config.get("link_selector", self.scraper.link_selector)
            ).strip()
            self.scraper.size_selector = str(
                new_config.get("size_selector", self.scraper.size_selector)
            ).strip()
            self.scraper.date_selector = str(
                new_config.get("date_selector", self.scraper.date_selector)
            ).strip()
            self.scraper.stats_selector = str(
                new_config.get("stats_selector", self.scraper.stats_selector)
            ).strip()
            self.scraper.timeout = timeout_val
            self.scraper.user_agent = (
                str(new_config.get("user_agent", self.scraper.user_agent)).strip()
                or DEFAULT_USER_AGENT
            )

        self._log(f"Updated scraper config for {base_url}.")
        return {"ok": True, "config": self.get_config()}

    def reset_config(self) -> dict[str, Any]:
        """Restore the scraper configuration to engine defaults."""
        with self._lock:
            self.scraper.provider_name = PROVIDER_NAME
            self.scraper.base_url = BASE_URL
            self.scraper.search_path = SEARCH_PATH
            self.scraper.query_param = QUERY_PARAM
            self.scraper.row_selector = ROW_SELECTOR
            self.scraper.title_selector = TITLE_SELECTOR
            self.scraper.link_selector = LINK_SELECTOR
            self.scraper.size_selector = SIZE_SELECTOR
            self.scraper.date_selector = DATE_SELECTOR
            self.scraper.stats_selector = STATS_SELECTOR
            self.scraper.timeout = DEFAULT_TIMEOUT
            self.scraper.user_agent = DEFAULT_USER_AGENT

        self._log("Restored default scraper configuration.")
        return {"ok": True, "config": self.get_config()}

    def open_external(self, url: str) -> bool:
        """Open a validated HTTP(S) or Magnet URI in the OS default handler."""
        cleaned = (url or "").strip()
        if not cleaned.lower().startswith(("http://", "https://", "magnet:")):
            return False
        try:
            ok = bool(webbrowser.open(cleaned))
            self._log(f"External open ({ok}): {cleaned[:64]}")
            return ok
        except Exception as exc:
            self._log(f"External open failed: {exc}")
            return False

    def copy_to_clipboard(self, text: str) -> dict[str, Any]:
        """Copy text to the system clipboard via pyperclip or native Linux helpers."""
        if not text:
            return {"ok": False, "error": "Nothing to copy."}

        try:
            pyperclip.copy(text)
            return {"ok": True}
        except Exception:
            for cmd in (["wl-copy"], ["xclip", "-selection", "clipboard"], ["xsel", "-b", "-i"]):
                exe = shutil.which(cmd[0])
                if not exe:
                    continue
                try:
                    subprocess.run(
                        [exe, *cmd[1:]],
                        input=text,
                        text=True,
                        check=True,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE,
                    )
                    return {"ok": True}
                except subprocess.SubprocessError:
                    continue
            return {"ok": False, "error": "No system clipboard mechanism available."}

    # ------------------------------------------------------------------
    # Intezam Cinema: Local Library, History, Diagnostics & Engine Handoff
    # ------------------------------------------------------------------

    def get_library(self) -> list[dict[str, Any]]:
        return self.store.get_library()

    def toggle_library_item(self, item: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(item, dict):
            return {"ok": False, "error": "Invalid library item payload."}
        return self.store.toggle_library_item(item)

    def remove_library_item(self, key: str) -> dict[str, Any]:
        return self.store.remove_library_item(key)

    def get_search_history(self) -> list[dict[str, Any]]:
        return self.store.get_search_history()

    def clear_search_history(self) -> list[dict[str, Any]]:
        return self.store.clear_search_history()

    def get_watch_history(self) -> list[dict[str, Any]]:
        return self.store.get_watch_history()

    def record_watch_launch(self, entry: dict[str, Any]) -> list[dict[str, Any]]:
        if not isinstance(entry, dict):
            return self.store.get_watch_history()
        return self.store.record_watch_launch(entry)

    def clear_watch_history(self) -> list[dict[str, Any]]:
        return self.store.clear_watch_history()

    def get_engine_status(self) -> dict[str, Any]:
        binary = _find_intezam_binary()
        with self._stream_lock:
            active = bool(self._stream_state.get("active"))
        return {
            "intezam_binary_available": binary is not None,
            "intezam_binary_path": str(binary) if binary else "",
            "vlc_available": shutil.which("vlc") is not None,
            "mpv_available": shutil.which("mpv") is not None,
            "rolling_cache_policy": "2.5% target / 25% adaptive ceiling",
            "stream_active": active,
        }

    def launch_intezam_stream(
        self,
        url: str,
        title: str = "",
        strict_cache: bool = False,
        timeout_seconds: int = 120,
    ) -> dict[str, Any]:
        """Launch the standalone Intezam C++ + LibVLC player safely for a Magnet URI."""
        cleaned = (url or "").strip()
        if not cleaned.lower().startswith("magnet:"):
            return {
                "ok": False,
                "error": "Intezam C++ rolling-cache engine requires a valid magnet:? URI.",
            }

        is_valid, msg = inspect_magnet_uri(cleaned)
        if not is_valid:
            return {"ok": False, "error": msg}

        binary = _find_intezam_binary()
        if binary is None:
            return {
                "ok": False,
                "error": "Compiled Intezam engine binary not found. Use Open External instead.",
            }

        self.stop_stream()

        timeout_clamped = max(10, min(600, int(timeout_seconds)))
        cmd = [str(binary), "--timeout", str(timeout_clamped)]
        if strict_cache:
            cmd.append("--strict-cache")

        try:
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                cwd=str(_REPO_ROOT),
            )
        except OSError as exc:
            self._log(f"Failed to start Intezam engine: {exc}")
            return {"ok": False, "error": f"Failed to start Intezam engine: {exc}"}

        if proc.stdin is not None:
            try:
                # Feed magnet URI and default first eligible video index (1) if prompted.
                proc.stdin.write(f"{cleaned}\n1\n")
                proc.stdin.flush()
                proc.stdin.close()
            except OSError:
                pass

        with self._stream_lock:
            self._stream_proc = proc
            self._stream_state = {
                "active": True,
                "stage": "connecting",
                "message": "Connecting to peers and retrieving torrent metadata...",
                "url": cleaned,
                "torrent_name": title,
                "selected_video": "",
                "video_size": "",
                "detected_videos": 0,
                "strict_cache": strict_cache,
                "returncode": None,
                "lines": [],
            }

        self.store.record_watch_launch(
            {
                "key": cleaned,
                "title": title or "Magnet Stream",
                "resolved_link": cleaned,
                "engine": "intezam-cpp",
            }
        )
        self._log(f"Launched Intezam engine ({binary.name}) for magnet stream.")

        def _monitor_process() -> None:
            if proc.stdout is not None:
                for raw_line in proc.stdout:
                    line = raw_line.strip()
                    if not line:
                        continue
                    with self._stream_lock:
                        lines = list(self._stream_state.get("lines", []))
                        lines.append(line)
                        self._stream_state["lines"] = lines[-30:]

                        if line.startswith("Retrieving metadata"):
                            self._stream_state["stage"] = "metadata"
                            self._stream_state["message"] = line
                        elif line.startswith("Torrent:"):
                            self._stream_state["torrent_name"] = line.split(":", 1)[1].strip()
                            self._stream_state["stage"] = "selecting"
                            self._stream_state["message"] = "Inspecting playable video streams..."
                        elif line.startswith("Detected Videos:"):
                            try:
                                self._stream_state["detected_videos"] = int(
                                    line.split(":", 1)[1].strip()
                                )
                            except ValueError:
                                pass
                        elif line.startswith("Selected:"):
                            self._stream_state["selected_video"] = line.split(":", 1)[1].strip()
                            self._stream_state["stage"] = "buffering"
                            self._stream_state["message"] = (
                                "Preparing 2.5% rolling cache & LibVLC playback..."
                            )
                        elif line.startswith("Size:"):
                            self._stream_state["video_size"] = line.split(":", 1)[1].strip()
                        elif "playing" in line.lower() or "playback" in line.lower():
                            self._stream_state["stage"] = "playing"
                            self._stream_state["message"] = line
                        elif line.startswith("Error:"):
                            self._stream_state["stage"] = "error"
                            self._stream_state["message"] = line

            rc = proc.wait()
            with self._stream_lock:
                self._stream_state["active"] = False
                self._stream_state["returncode"] = rc
                if rc != 0 and self._stream_state["stage"] != "error":
                    self._stream_state["stage"] = "error"
                    last_lines = self._stream_state.get("lines") or []
                    self._stream_state["message"] = (
                        last_lines[-1] if last_lines else f"Engine exited with code {rc}."
                    )
                elif rc == 0 and self._stream_state["stage"] not in {"error", "stopped"}:
                    self._stream_state["stage"] = "completed"
                    self._stream_state["message"] = "Playback session ended."
                if self._stream_proc is proc:
                    self._stream_proc = None

        threading.Thread(target=_monitor_process, daemon=True).start()
        return {"ok": True, "state": self.get_stream_status()}

    def get_stream_status(self) -> dict[str, Any]:
        with self._stream_lock:
            return dict(self._stream_state)

    def stop_stream(self) -> dict[str, Any]:
        with self._stream_lock:
            proc = self._stream_proc
            if proc is not None and proc.poll() is None:
                try:
                    proc.terminate()
                except OSError:
                    pass
                self._stream_state["active"] = False
                self._stream_state["stage"] = "stopped"
                self._stream_state["message"] = "Playback session stopped."
                self._stream_proc = None
            return dict(self._stream_state)

    def get_app_logs(self) -> list[str]:
        return list(self._logs)


def main() -> None:
    """Create the pywebview desktop window and start the GUI event loop."""
    if not UI_INDEX_PATH.is_file():
        raise FileNotFoundError(f"Frontend entrypoint not found: {UI_INDEX_PATH}")

    bridge = ScraperBridge()
    webview.create_window(
        title="INTEZAM",
        url=UI_INDEX_PATH.as_uri(),
        js_api=bridge,
        width=1440,
        height=900,
        min_size=(1000, 650),
        background_color="#090B12",
    )
    webview.start(debug=False)


if __name__ == "__main__":
    main()
