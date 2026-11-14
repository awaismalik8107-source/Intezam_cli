"""Isolated JSON persistence for local Intezam library and search history."""

from __future__ import annotations

import json
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

DEFAULT_DATA_DIR = Path(__file__).resolve().parent / ".data"


class LocalMediaStore:
    """Thread-safe JSON file store confined inside ``src/gui_scraper/.data``."""

    def __init__(self, data_dir: Path | None = None) -> None:
        self.data_dir = data_dir or DEFAULT_DATA_DIR
        self._lock = threading.Lock()
        self.library_path = self.data_dir / "library.json"
        self.history_path = self.data_dir / "history.json"
        self.watch_history_path = self.data_dir / "watch_history.json"

    def _read_list(self, path: Path) -> list[dict[str, Any]]:
        if not path.is_file():
            return []
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            return [entry for entry in raw if isinstance(entry, dict)] if isinstance(raw, list) else []
        except (OSError, json.JSONDecodeError):
            return []

    def _write_list(self, path: Path, items: list[dict[str, Any]]) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        temp_path = path.with_suffix(".tmp")
        temp_path.write_text(json.dumps(items, indent=2, ensure_ascii=False), encoding="utf-8")
        temp_path.replace(path)

    def get_library(self) -> list[dict[str, Any]]:
        with self._lock:
            return self._read_list(self.library_path)

    def toggle_library_item(self, item: dict[str, Any]) -> dict[str, Any]:
        key = str(item.get("key") or item.get("title") or "").strip()
        if not key:
            return {"ok": False, "error": "Library item requires a non-empty key."}

        with self._lock:
            items = self._read_list(self.library_path)
            existing_idx = next((idx for idx, entry in enumerate(items) if entry.get("key") == key), None)
            if existing_idx is not None:
                items.pop(existing_idx)
                self._write_list(self.library_path, items)
                return {"ok": True, "saved": False, "items": items}

            entry = {
                **item,
                "key": key,
                "saved_at": datetime.now(timezone.utc).isoformat(),
            }
            items.insert(0, entry)
            self._write_list(self.library_path, items)
            return {"ok": True, "saved": True, "items": items}

    def remove_library_item(self, key: str) -> dict[str, Any]:
        cleaned = (key or "").strip()
        with self._lock:
            items = self._read_list(self.library_path)
            filtered = [entry for entry in items if entry.get("key") != cleaned]
            self._write_list(self.library_path, filtered)
            return {"ok": True, "items": filtered}

    def get_search_history(self) -> list[dict[str, Any]]:
        with self._lock:
            return self._read_list(self.history_path)

    def record_search(self, query: str, mode: str, result_count: int) -> list[dict[str, Any]]:
        cleaned = (query or "").strip()
        if not cleaned:
            return self.get_search_history()

        with self._lock:
            items = self._read_list(self.history_path)
            items = [
                entry
                for entry in items
                if not (
                    str(entry.get("query", "")).lower() == cleaned.lower()
                    and entry.get("mode") == mode
                )
            ]
            items.insert(
                0,
                {
                    "query": cleaned,
                    "mode": mode,
                    "result_count": int(result_count),
                    "searched_at": datetime.now(timezone.utc).isoformat(),
                },
            )
            items = items[:20]
            self._write_list(self.history_path, items)
            return items

    def clear_search_history(self) -> list[dict[str, Any]]:
        with self._lock:
            self._write_list(self.history_path, [])
            return []

    def get_watch_history(self) -> list[dict[str, Any]]:
        with self._lock:
            return self._read_list(self.watch_history_path)

    def record_watch_launch(self, entry: dict[str, Any]) -> list[dict[str, Any]]:
        key = str(entry.get("key") or entry.get("resolved_link") or "").strip()
        if not key:
            return self.get_watch_history()

        with self._lock:
            items = self._read_list(self.watch_history_path)
            items = [row for row in items if row.get("key") != key]
            items.insert(
                0,
                {
                    **entry,
                    "key": key,
                    "launched_at": datetime.now(timezone.utc).isoformat(),
                },
            )
            items = items[:24]
            self._write_list(self.watch_history_path, items)
            return items

    def clear_watch_history(self) -> list[dict[str, Any]]:
        with self._lock:
            self._write_list(self.watch_history_path, [])
            return []
