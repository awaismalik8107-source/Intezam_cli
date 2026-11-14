"""Data models for the isolated scraper subsystem."""

from dataclasses import dataclass


@dataclass(slots=True)
class ScrapedResult:
    """Represents a single scraped row and its validated download URI."""

    title: str
    size: str
    date: str
    stats: str
    download_uri: str
    is_valid_link: bool
