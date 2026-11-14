"""Standalone, isolated Desktop GUI web-scraping subsystem."""

from .engine import ConfigurableTableScraper, ScrapeResponse
from .models import ScrapedItem
from .validator import resolve_and_validate_link, verify_http_status

__all__ = [
    "ConfigurableTableScraper",
    "ScrapeResponse",
    "ScrapedItem",
    "resolve_and_validate_link",
    "verify_http_status",
]
