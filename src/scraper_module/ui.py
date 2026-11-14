"""Interactive terminal UI powered by rich and questionary."""

from __future__ import annotations

import asyncio
import shutil
import subprocess
from collections.abc import Sequence

import httpx
import questionary
from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from .engine import CustomTableScraper
from .models import ScrapedResult

_NEW_SEARCH = "__new_search__"
_EXIT_APP = "__exit__"

_ACTION_COPY = "copy"
_ACTION_PRINT = "print"
_ACTION_BACK = "back"
_ACTION_NEW_SEARCH = "new_search"
_ACTION_EXIT = "exit"


def truncate_text(value: str, max_length: int) -> str:
    """Truncate text to ``max_length`` characters with an ellipsis when needed."""
    if max_length <= 3 or len(value) <= max_length:
        return value
    return f"{value[: max_length - 3]}..."


def build_results_table(results: Sequence[ScrapedResult], query: str) -> Table:
    """Render scraped results into a formatted ``rich.table.Table``."""
    table = Table(
        title=f"Search Results for '{query}' ({len(results)} found)",
        show_lines=False,
        header_style="bold cyan",
        expand=True,
    )
    table.add_column("#", justify="right", style="dim", width=4, no_wrap=True)
    table.add_column("Title", style="bold white", ratio=3, min_width=20)
    table.add_column("Size", justify="right", style="magenta", no_wrap=True)
    table.add_column("Date", style="blue", no_wrap=True)
    table.add_column("Stats", style="yellow", no_wrap=True)
    table.add_column("Valid Link", justify="center", no_wrap=True)
    table.add_column("URI Preview", style="dim", ratio=2, min_width=18)

    for idx, item in enumerate(results, start=1):
        status_badge = (
            "[bold green]PASS[/bold green]"
            if item.is_valid_link
            else "[bold red]FAIL[/bold red]"
        )
        uri_preview = truncate_text(item.download_uri or "(empty)", 42)
        table.add_row(
            str(idx),
            truncate_text(item.title, 60),
            item.size,
            item.date,
            item.stats,
            status_badge,
            uri_preview,
        )

    return table


def build_detail_panel(result: ScrapedResult, index: int) -> Panel:
    """Build a ``rich.panel.Panel`` displaying full details for a selected row."""
    body = Text()
    body.append("Title: ", style="bold cyan")
    body.append(f"{result.title}\n")
    body.append("Size:  ", style="bold cyan")
    body.append(f"{result.size}\n")
    body.append("Date:  ", style="bold cyan")
    body.append(f"{result.date}\n")
    body.append("Stats: ", style="bold cyan")
    body.append(f"{result.stats}\n")
    body.append("Valid: ", style="bold cyan")
    if result.is_valid_link:
        body.append("PASS\n", style="bold green")
    else:
        body.append("FAIL\n", style="bold red")
    body.append("URI:   ", style="bold cyan")
    body.append(result.download_uri or "(empty)", style="underline white")

    border_style = "green" if result.is_valid_link else "red"
    return Panel(
        body,
        title=f"Result #{index} Details",
        border_style=border_style,
        expand=True,
    )


def copy_to_clipboard(text: str) -> tuple[bool, str]:
    """Copy ``text`` to the system clipboard using available OS utilities."""
    if not text:
        return (False, "Nothing to copy: the extracted URI is empty.")

    candidates: list[list[str]] = [
        ["wl-copy"],
        ["xclip", "-selection", "clipboard"],
        ["xsel", "--clipboard", "--input"],
        ["pbcopy"],
        ["clip.exe"],
        ["clip"],
    ]

    for cmd in candidates:
        executable = shutil.which(cmd[0])
        if not executable:
            continue
        try:
            subprocess.run(
                [executable, *cmd[1:]],
                input=text,
                text=True,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            return (True, f"Copied URI to clipboard via {cmd[0]}.")
        except subprocess.SubprocessError:
            continue

    return (
        False,
        "No working clipboard utility found (tried wl-copy, xclip, xsel, pbcopy, clip).",
    )


async def fetch_results_with_spinner(
    scraper: CustomTableScraper,
    query: str,
    console: Console,
) -> list[ScrapedResult]:
    """Execute ``scraper.search(query)`` while displaying a Rich loading spinner."""
    with console.status(
        f"[bold cyan]Scraping results for '{query}'...[/bold cyan]",
        spinner="dots",
    ):
        return await scraper.search(query)


def _inspect_results_loop(
    results: Sequence[ScrapedResult],
    query: str,
    console: Console,
) -> str:
    """Allow the user to pick rows, inspect full URIs, and copy them."""
    while True:
        row_choices: list[questionary.Choice] = []
        for idx, item in enumerate(results, start=1):
            validity = "VALID" if item.is_valid_link else "INVALID"
            label = f"[{idx}] {truncate_text(item.title, 52)} ({item.size} | {validity})"
            row_choices.append(questionary.Choice(title=label, value=idx - 1))

        row_choices.append(questionary.Choice(title="New search", value=_NEW_SEARCH))
        row_choices.append(questionary.Choice(title="Exit", value=_EXIT_APP))

        selection = questionary.select(
            "Select a row to inspect or copy its URI:",
            choices=row_choices,
        ).ask()

        if selection is None or selection == _EXIT_APP:
            return _EXIT_APP
        if selection == _NEW_SEARCH:
            return _NEW_SEARCH

        selected_idx = int(selection)
        selected = results[selected_idx]
        console.print(build_detail_panel(selected, selected_idx + 1))

        action = questionary.select(
            "Choose an action for this result:",
            choices=[
                questionary.Choice("Copy URI to clipboard", value=_ACTION_COPY),
                questionary.Choice("Print raw URI to terminal", value=_ACTION_PRINT),
                questionary.Choice("Back to results list", value=_ACTION_BACK),
                questionary.Choice("New search", value=_ACTION_NEW_SEARCH),
                questionary.Choice("Exit", value=_ACTION_EXIT),
            ],
        ).ask()

        if action is None or action == _ACTION_EXIT:
            return _EXIT_APP
        if action == _ACTION_NEW_SEARCH:
            return _NEW_SEARCH
        if action == _ACTION_COPY:
            copied, message = copy_to_clipboard(selected.download_uri)
            if copied:
                console.print(f"[green]{message}[/green]")
            else:
                console.print(f"[yellow]{message}[/yellow]")
                console.print("[dim]Raw URI:[/dim]")
                console.print(selected.download_uri, soft_wrap=True)
        elif action == _ACTION_PRINT:
            console.print("[dim]Raw URI:[/dim]")
            console.print(selected.download_uri, soft_wrap=True)
        elif action == _ACTION_BACK:
            console.print(build_results_table(results, query))


def run_cli(
    scraper: CustomTableScraper | None = None,
    initial_query: str | None = None,
    console: Console | None = None,
) -> None:
    """Run the interactive scraper CLI loop."""
    active_console = console or Console()
    active_scraper = scraper or CustomTableScraper()
    pending_query = initial_query

    active_console.print(
        Panel.fit(
            "[bold cyan]Isolated Table Scraper CLI[/bold cyan]\n"
            f"[dim]Target Base URL: {active_scraper.base_url}[/dim]",
            border_style="cyan",
        )
    )

    try:
        while True:
            if pending_query is not None:
                query = pending_query.strip()
                pending_query = None
            else:
                raw_query = questionary.text(
                    "Enter search query:",
                    validate=lambda text: True
                    if text.strip()
                    else "Please enter a non-empty search query.",
                ).ask()
                if raw_query is None:
                    break
                query = raw_query.strip()

            if not query:
                active_console.print("[yellow]Empty search query; exiting.[/yellow]")
                break

            try:
                results = asyncio.run(
                    fetch_results_with_spinner(active_scraper, query, active_console)
                )
            except httpx.HTTPStatusError as exc:
                active_console.print(
                    Panel(
                        f"HTTP {exc.response.status_code} error requesting {exc.request.url}",
                        title="Request Failed",
                        border_style="red",
                    )
                )
                if not questionary.confirm("Try another search?", default=True).ask():
                    break
                continue
            except httpx.HTTPError as exc:
                active_console.print(
                    Panel(
                        f"Network error: {exc}",
                        title="Connection Failed",
                        border_style="red",
                    )
                )
                if not questionary.confirm("Try another search?", default=True).ask():
                    break
                continue

            if not results:
                active_console.print(
                    Panel(
                        f"No rows matched selector '{active_scraper.row_selector}' for '{query}'.",
                        title="No Results",
                        border_style="yellow",
                    )
                )
                if not questionary.confirm("Search again?", default=True).ask():
                    break
                continue

            active_console.print(build_results_table(results, query))
            outcome = _inspect_results_loop(results, query, active_console)
            if outcome == _EXIT_APP:
                break

    except (KeyboardInterrupt, EOFError):
        active_console.print("\n[dim]Interrupted. Exiting.[/dim]")
