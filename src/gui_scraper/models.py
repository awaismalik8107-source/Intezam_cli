"""Data layer models for the isolated GUI scraper subsystem."""

from dataclasses import dataclass


@dataclass(slots=True)
class ScrapedItem:
    """Represents a single scraped table row and its link verification state."""

    id: int
    title: str
    size: str
    date: str
    stats: str
    raw_link: str
    resolved_link: str
    is_valid: bool
    verification_msg: str
    view_url: str = ""
    magnet_link: str = ""
    torrent_link: str = ""
    relative_date: str = ""
    seeders: int = 0
    leechers: int = 0
    downloads: int = 0
