"""Isolated, modular web-scraping CLI subsystem."""

from .engine import CustomTableScraper
from .models import ScrapedResult
from .validator import verify_link

__all__ = [
    "CustomTableScraper",
    "ScrapedResult",
    "verify_link",
]
