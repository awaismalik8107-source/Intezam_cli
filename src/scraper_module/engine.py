"""Async HTML table scraper powered by httpx and BeautifulSoup."""

from __future__ import annotations

from urllib.parse import quote_plus, urljoin

import httpx
from bs4 import BeautifulSoup, Tag

from .models import ScrapedResult
from .validator import verify_link

# ---------------------------------------------------------------------------
# Target Site Configuration Placeholders
# Update these constants to match your target HTML results table.
# ---------------------------------------------------------------------------
TARGET_BASE_URL = "https://example.com"
SEARCH_PATH_TEMPLATE = "/search?q={query}"
ROW_SELECTOR = "table.results tbody tr"
TITLE_SELECTOR = "td.title"
LINK_SELECTOR = "td.link a"
SIZE_SELECTOR = "td.size"
DATE_SELECTOR = "td.date"
STATS_SELECTOR = "td.stats"

DEFAULT_TIMEOUT_SECONDS = 15.0
DEFAULT_HEADERS = {
    "User-Agent": "Mozilla/5.0 (compatible; ScraperModuleCLI/1.0)",
}


class CustomTableScraper:
    """Asynchronous scraper for extracting structured rows from HTML tables."""

    def __init__(
        self,
        base_url: str = TARGET_BASE_URL,
        search_path_template: str = SEARCH_PATH_TEMPLATE,
        row_selector: str = ROW_SELECTOR,
        title_selector: str = TITLE_SELECTOR,
        link_selector: str = LINK_SELECTOR,
        size_selector: str = SIZE_SELECTOR,
        date_selector: str = DATE_SELECTOR,
        stats_selector: str = STATS_SELECTOR,
        timeout: float = DEFAULT_TIMEOUT_SECONDS,
        headers: dict[str, str] | None = None,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        self.base_url = base_url
        self.search_path_template = search_path_template
        self.row_selector = row_selector
        self.title_selector = title_selector
        self.link_selector = link_selector
        self.size_selector = size_selector
        self.date_selector = date_selector
        self.stats_selector = stats_selector
        self.timeout = timeout
        self.headers = dict(headers) if headers is not None else dict(DEFAULT_HEADERS)
        self._client = client

    def build_search_url(self, query: str) -> str:
        """Build the target search URL from ``base_url`` and ``search_path_template``."""
        encoded_query = quote_plus(query.strip())
        relative_path = self.search_path_template.format(query=encoded_query)
        return urljoin(self.base_url, relative_path)

    @staticmethod
    def _extract_text(row: Tag, selector: str, default: str = "N/A") -> str:
        """Extract normalized text for the first element matching ``selector``."""
        if not selector:
            return default
        element = row.select_one(selector)
        if element is None:
            return default
        text = " ".join(element.get_text(" ", strip=True).split())
        return text if text else default

    @staticmethod
    def _extract_href(row: Tag, selector: str) -> str:
        """Extract the raw ``href`` attribute for the first element matching ``selector``."""
        if not selector:
            return ""
        element = row.select_one(selector)
        if element is None:
            return ""
        raw_href = element.get("href")
        if isinstance(raw_href, list):
            return str(raw_href[0]) if raw_href else ""
        return str(raw_href) if raw_href is not None else ""

    def parse_html(self, html: str, base_url: str | None = None) -> list[ScrapedResult]:
        """Parse HTML markup into a list of :class:`ScrapedResult` items."""
        soup = BeautifulSoup(html, "html.parser")
        effective_base = base_url or self.base_url
        results: list[ScrapedResult] = []

        for row in soup.select(self.row_selector):
            title = self._extract_text(row, self.title_selector, default="Untitled")
            size = self._extract_text(row, self.size_selector, default="N/A")
            date = self._extract_text(row, self.date_selector, default="N/A")
            stats = self._extract_text(row, self.stats_selector, default="N/A")
            raw_href = self._extract_href(row, self.link_selector)
            download_uri, is_valid_link = verify_link(effective_base, raw_href)

            results.append(
                ScrapedResult(
                    title=title,
                    size=size,
                    date=date,
                    stats=stats,
                    download_uri=download_uri,
                    is_valid_link=is_valid_link,
                )
            )

        return results

    async def search(self, query: str) -> list[ScrapedResult]:
        """Fetch and parse search results for ``query``."""
        cleaned_query = query.strip()
        if not cleaned_query:
            return []

        target_url = self.build_search_url(cleaned_query)

        if self._client is not None:
            response = await self._client.get(target_url)
            response.raise_for_status()
            return self.parse_html(response.text, base_url=str(response.url))

        async with httpx.AsyncClient(
            timeout=self.timeout,
            headers=self.headers,
            follow_redirects=True,
        ) as client:
            response = await client.get(target_url)
            response.raise_for_status()
            return self.parse_html(response.text, base_url=str(response.url))
