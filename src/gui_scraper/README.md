# Intezam GUI Scraper — TV & Anime Discovery, Metadata, and Nyaa / Multi-Source Torrent Engine

This directory (`src/gui_scraper/`) contains the standalone **Intezam** `pywebview` + React/TypeScript desktop application for discovering anime and TV series, inspecting official season/episode metadata and posters, and copying verified magnet links across **Nyaa**, **SubsPlease**, and **EZTV**.

---

## 1. How to Run

### Launch the Desktop GUI
From the repository root (`/home/error343/Streamers`):

```bash
cd /home/error343/Streamers
./src/gui_scraper/.venv/bin/python -m src.gui_scraper.app
```

- **Mock Mode (default):** Loads a built-in multi-season catalog with real posters, episode titles, batch packs, and magnet links for offline testing.
- **Live Mode:** Click **`Live`** in the top-right toggle bar to query live metadata (**AniList**, **TVMaze**, and optional **TMDB**) and live torrent sources (**Nyaa**, **SubsPlease**, **EZTV**).

*(Optional)* To enable live **TMDB v3** metadata queries alongside AniList and TVMaze:
```bash
TMDB_API_KEY="your_tmdb_api_key" ./src/gui_scraper/.venv/bin/python -m src.gui_scraper.app
```

### Rebuild the React / TypeScript Frontend
```bash
cd /home/error343/Streamers/src/gui_scraper/ui
npm run build
```

### Run Automated Tests
```bash
cd /home/error343/Streamers
./src/gui_scraper/.venv/bin/pytest src/gui_scraper/tests/test_gui_scraper.py -v
```

---

## 2. End-to-End Discovery Pipeline

```text
User Search Query
   │
   ▼
1. CatalogMetadataService (metadata.py)
   ├─ AniList GraphQL API (https://graphql.anilist.co)
   ├─ TMDB v3 REST API (https://api.themoviedb.org/3) [optional key]
   └─ TVMaze REST API (https://api.tvmaze.com)
   │  Returns canonical MediaIdentity cards (official title, Romaji title,
   │  alternate titles, release year, real poster URL, seasons & episodes)
   ▼
2. User Opens a Show Card → MultiProviderDiscoveryEngine (providers.py)
   ├─ NyaaSourceProvider (https://nyaa.si HTML table scraper)
   ├─ SubsPleaseSourceProvider (https://subsplease.org/api/ JSON API)
   └─ EZTVSourceProvider (https://eztvx.to/api/get-torrents JSON API)
   │  Queries primary + Romaji/alternate titles in bounded parallel
   ▼
3. Deduplication & Episode Matching (providers.py + matching.py)
   ├─ Deduplicates by lowercase BTIH info-hash (merging mirrors e.g. "Nyaa + SubsPlease")
   ├─ Strips release groups/CRC32/tech tags & scores title similarity (TitleIdentityMatcher)
   └─ Maps releases to Season → Episode, Multi-Episode Batch, Season Pack, or Complete Series
   ▼
4. GUI Episode & Magnet Inspector (MediaDetailsView.tsx)
   └─ Displays selectable magnet:?xt=urn:btih:... URI with one-click Copy Magnet Link
```

---

## 3. Nyaa Scraping Logic Explained (`engine.py` & `providers.py`)

The Nyaa scraper is implemented in two layers:
1. **`ConfigurableTableScraper`** (`src/gui_scraper/engine.py`): Low-level URL builder, HTTP client, BeautifulSoup HTML table parser, pagination parser, TTL cache, and retry engine.
2. **`NyaaSourceProvider`** (`src/gui_scraper/providers.py`): High-level `SourceProvider` adapter that handles category mapping, rate limiting, and normalization into `TorrentRelease` objects.

### 3.1 URL Structure & Query Encoding (`ConfigurableTableScraper.build_search_url`)

- **Base URL:** `https://nyaa.si`
- **Default Browse URL (when query is empty):**
  ```text
  https://nyaa.si/?q=&f=0&c={category}&s=leechers&o=desc
  ```
- **Search URL (when query is provided):**
  Spaces and special characters are encoded with `urllib.parse.quote_plus`:
  ```text
  https://nyaa.si/?f={filter_mode}&c={category}&q={encoded_query}&s={sort_by}&o={sort_order}
  ```
- **Pagination (`page > 1`):**
  Appends `&p={page}` (e.g. `https://nyaa.si/?f=0&c=1_0&q=Sousou+no+Frieren&s=seeders&o=desc&p=2`).

### 3.2 Nyaa Category Codes (`NyaaSourceProvider.resolve_category_code`)

Previously, hardcoding `c=1_2` hid raw Japanese releases, non-English subs, and live-action shows. The scraper now supports dynamic Nyaa categories:

| Category Code | Meaning | Usage |
|---|---|---|
| `1_0` | **All Anime** | Default for show-level episode discovery (includes English-Sub, Non-English, and Raw) |
| `1_2` | **Anime — English-Translated** | Default for direct table searches |
| `1_3` | **Anime — Non-English-Translated** | International subtitle releases |
| `1_4` | **Anime — Raw** | Unsubtitled Japanese broadcast/BD releases |
| `4_0` | **Live Action** | Live-action TV / tokusatsu / drama |
| `0_0` | **All Categories** | Unrestricted search across all Nyaa categories |

### 3.3 Why Multi-Title Querying Is Used (`MultiProviderDiscoveryEngine`)

On Nyaa, most anime release groups (`[SubsPlease]`, `[Erai-raws]`, `[Judas]`, `[Ember]`) name their torrents using the **Romaji** title (e.g. `Sousou no Frieren`) rather than the English title (`Frieren: Beyond Journey's End`).
- Searching only `"Frieren: Beyond Journey's End"` on Nyaa returns **4 pages** of results.
- Searching `"Sousou no Frieren"` on Nyaa returns **12 pages** of results (3× more torrents).
- When a show is opened, `MultiProviderDiscoveryEngine.discover_for_show` automatically takes `identity.all_search_titles()[:2]` (both the official English title and the Romaji title from AniList) and queries Nyaa for **both** in parallel, then merges and deduplicates the results under the single canonical show.

### 3.4 Exact HTML DOM Parsing (`ConfigurableTableScraper.parse_html`)

Nyaa search results are rendered inside `<table class="table table-bordered table-hover table-striped torrent-list">`. `parse_html` parses the response with `BeautifulSoup(html, "html.parser")`:

1. **Row Selection:**
   - Primary selector: `table.torrent-list tbody tr` (fallback: `table tbody tr`).
   - Guard check: `tds = row.find_all("td")`; rows with `len(tds) < 8` are skipped.
2. **Column Mapping (`tds[0]` through `tds[7]`):**
   - **`tds[0]` (Category Icon):** Category cell.
   - **`tds[1]` (`<td colspan="2">` — Title & View Link):**
     - Selector: `row.select_one('td[colspan="2"] a[href^="/view/"]:not(.comments)')`
     - **Why `:not(.comments)` matters:** When a torrent has user comments, Nyaa inserts a `<a href="/view/12345#comments" class="comments">` badge inside the same `<td>` before the title link. Excluding `.comments` guarantees we always extract the actual torrent title and `/view/<id>` link.
     - Reads the `title` attribute first (`self._attr_str(title_tag, "title")`) and falls back to `title_tag.get_text(strip=True)`.
   - **`tds[2]` (`<td class="text-center">` — Magnet & `.torrent` Download Links):**
     - **Magnet Link:** `links_cell.select_one('a[href^="magnet:?"]')` (or fallback anchor containing `<i class="fa-magnet">`).
     - **Torrent File Link:** `links_cell.select_one('a[href^="/download/"]')` (or fallback anchor containing `<i class="fa-download">`), resolved with `urljoin("https://nyaa.si", href)` to `https://nyaa.si/download/<id>.torrent`.
   - **`tds[3]` (File Size):** `tds[3].get_text(strip=True)` (e.g. `1.4 GiB`).
   - **`tds[4]` (Date / Timestamp):** `tds[4].get_text(strip=True)` for UTC date (`2026-11-14 12:00`) and `tds[4].get("title")` for relative age.
   - **`tds[5]` (Seeders):** Parsed as `int` via `_parse_int`.
   - **`tds[6]` (Leechers):** Parsed as `int` via `_parse_int`.
   - **`tds[7]` (Completed Downloads):** Parsed as `int` via `_parse_int`.

### 3.5 Pagination Detection (`ConfigurableTableScraper.parse_pagination`)

Inspects `<ul class="pagination">`:
- Extracts all numeric page links (`<a>` tags whose text is digits) to compute `total_pages`.
- Checks `li.next:not(.disabled) a` (or `a[rel="next"]`) and `current_page < max_page` to set `has_next_page`.

### 3.6 Link Validation & BTIH Info-Hash Deduplication (`validator.py`, `engine.py`, `providers.py`)

- **Validation (`validate_nyaa_links` in `validator.py`):**
  - Validates magnet links by checking `magnet:?` scheme and matching `xt=urn:btih:([A-Za-z0-9]{32,40})` (40-char hex SHA-1 BTIH or 32-char Base32 BTIH).
  - Validates `.torrent` URLs by checking `https://nyaa.si/download/...torrent`.
  - A release is marked valid if either its magnet URI or its `.torrent` URL is valid (and torrent-only items never fabricate fake magnet links).
- **BTIH Deduplication & Mirror Attribution (`deduplicate_releases` in `providers.py`):**
  - Extracts the lowercase BTIH hash (`xt=urn:btih:...`).
  - Only merges releases when their BTIH info-hash (or exact magnet/torrent URL) is identical.
  - When the same torrent is returned by both `Nyaa` and `SubsPlease`, their provider names are merged into `mirror_providers = ["Nyaa", "SubsPlease"]` (displayed in the GUI as `Nyaa + SubsPlease`) and the highest seeder count is kept.

### 3.7 Performance, Caching, Coalescing & Retry Logic (`ConfigurableTableScraper.search`)

- **Connection Pooling:** Reuses a shared `httpx.Client` (`max_keepalive_connections=8, max_connections=16`, `timeout=10.0s`) to avoid repeated TLS handshakes.
- **In-Flight Request Coalescing:** If two threads request the same `(query, page, category, filter, sort)` simultaneously, the second thread waits on a `threading.Event` and reuses the first thread's response instead of sending a duplicate request to Nyaa.
- **90-Second TTL Cache:** Successful scrape responses are cached in memory for `90s` (bypassed when the user clicks **Refresh sources** / `force_refresh=True`).
- **Rate Limiting & Retry Backoff:** Enforces a minimum interval between live requests (`0.15s` on Nyaa, `0.25s` on SubsPlease/EZTV) and retries HTTP `429/502/503/504` or network timeouts with exponential backoff (`retry_backoff * 2**attempt`).

---

## 4. Episode, Season & Batch Matching Logic (`matching.py` & `episodeCatalog.ts`)

Raw torrent titles are parsed and matched against canonical show metadata in `src/gui_scraper/matching.py` (Python) and `src/gui_scraper/ui/src/lib/episodeCatalog.ts` (TypeScript):

1. **Noise Stripping:** Removes leading release-group tags (`[SubsPlease]`), trailing CRC32 hashes (`[F02B964D]`), file extensions (`.mkv`, `.mp4`), and technical tags (`1080p`, `2160p`, `HEVC`, `x265`, `10bit`, `Dual-Audio`, `Multi-Sub`).
2. **Show Disambiguation (`TitleIdentityMatcher`):**
   - Compares normalized tokens against official, Romaji, native, and synonym titles.
   - Penalizes conflicting release years so remakes/reboots (e.g. `Hunter x Hunter (1999)` vs. `Hunter x Hunter (2011)`) are never mixed.
   - Rejects standalone movies/OVAs/spinoffs when matching a numbered TV season.
3. **Season & Episode Classification (`match_release_to_identity` & `buildEpisodeCatalog`):**
   - **Single Episodes:** Matches `S01E05`, `1x05`, `- 05`, `05v2`, `2nd Season - 05`, `Season 2 - 05`, and `Episode 05`.
   - **Absolute Episode Mapping:** If a release uses continuous numbering (e.g. `Sousou no Frieren - 29` when Season 1 has `28` episodes), maps it automatically to `Season 2, Episode 1` using official season episode counts.
   - **Batch & Season Packs:** Separates `01-12` (`multi-episode`), `Season 1 Batch` (`season-pack`), and `Complete Series` (`complete-series`) into the **Batch & season packs** section without fabricating individual episode numbers.
   - **Unclassified / Specials:** Places OVAs, PVs, and OSTs into **Unclassified releases**.

---

## 5. Directory Structure (`src/gui_scraper/`)

```text
src/gui_scraper/
├── app.py                  # pywebview entry point & ScraperBridge (discover_shows, discover_show_sources, search_paginated)
├── engine.py               # ConfigurableTableScraper (Nyaa URL builder, BeautifulSoup HTML parser, pagination, cache, retries)
├── models.py               # Canonical dataclasses (MediaIdentity, SeasonMetadata, EpisodeMetadata, TorrentRelease, ScrapedItem)
├── metadata.py             # AniListMetadataProvider, TMDBMetadataProvider, TVMazeMetadataProvider, CatalogMetadataService
├── matching.py             # Release title parser, TitleIdentityMatcher, and season/episode/batch classifier
├── providers.py            # SourceProvider ABC, NyaaSourceProvider, SubsPleaseSourceProvider, EZTVSourceProvider, MultiProviderDiscoveryEngine
├── storage.py              # Local JSON persistence for Library, Search History, and Watch History
├── validator.py            # Magnet BTIH & Nyaa .torrent URL validator
├── tests/
│   └── test_gui_scraper.py # 29 pytest unit & integration tests (models, metadata, matching, providers, engine, bridge, UI scenarios)
└── ui/                     # React 19 + TypeScript + Tailwind CSS + Motion frontend
    ├── dist/index.html     # Bundled single-file production UI loaded by pywebview
    └── src/
        ├── App.tsx
        ├── components/
        │   ├── PosterArtwork.tsx    # Real poster/backdrop <img> loader with procedural SVG fallback
        │   ├── PosterCard.tsx       # Show card with poster, season/episode counts, and library bookmark
        │   ├── SearchView.tsx       # Show discovery grid & starter queries
        │   ├── MediaDetailsView.tsx # Show metadata header, category filter, provider status pills, episode grid & magnet panel
        │   └── ...
        └── lib/
            ├── types.ts             # TypeScript interfaces matching Python models
            ├── bridge.ts            # pywebview JS bridge wrapper
            ├── mediaAdapter.ts      # Metadata-to-card adapter & multi-provider source merger
            ├── episodeCatalog.ts    # Frontend release parser, absolute episode mapper & catalog builder
            └── catalogScenariosTest.ts
```
