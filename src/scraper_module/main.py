"""Standalone CLI entry point for the isolated scraper subsystem."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

if __package__ in (None, ""):
    # Allow running directly as `python src/scraper_module/main.py` or `python main.py`.
    _PACKAGE_PARENT = Path(__file__).resolve().parent.parent
    if str(_PACKAGE_PARENT) not in sys.path:
        sys.path.insert(0, str(_PACKAGE_PARENT))
    from scraper_module.engine import TARGET_BASE_URL, CustomTableScraper
    from scraper_module.ui import run_cli
else:
    from .engine import TARGET_BASE_URL, CustomTableScraper
    from .ui import run_cli


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse optional CLI flags for standalone testing."""
    parser = argparse.ArgumentParser(
        description="Isolated, modular web-scraping CLI subsystem.",
    )
    parser.add_argument(
        "-q",
        "--query",
        default=None,
        help="Optional initial search query (prompts interactively if omitted).",
    )
    parser.add_argument(
        "--base-url",
        default=TARGET_BASE_URL,
        help=f"Override target base URL (default: {TARGET_BASE_URL}).",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    """Launch the standalone scraper terminal UI."""
    args = parse_args(argv)
    scraper = CustomTableScraper(base_url=args.base_url)
    run_cli(scraper=scraper, initial_query=args.query)


if __name__ == "__main__":
    main()
