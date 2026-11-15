from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from gui_scraper.app import UI_INDEX_PATH
from gui_scraper.metadata import TMDBMetadataProvider, TVMazeMetadataProvider


def test_metadata_providers_preserve_season_zero_specials_and_blank_titles() -> None:
    """Verify TMDB and TVMaze preserve Season 0 Specials and leave missing episode titles blank."""
    tmdb_ident = TMDBMetadataProvider.normalize_tv_payload(
        {
            "id": 1429,
            "name": "Attack on Titan",
            "original_name": "進撃の巨人",
            "first_air_date": "2013-04-07",
            "overview": "Humanity vs Titans",
            "seasons": [
                {"season_number": 0, "name": "Specials", "episode_count": 2},
                {"season_number": 1, "name": "Season 1", "episode_count": 25},
            ],
        },
        season_payloads={
            0: {
                "episodes": [
                    {"episode_number": 1, "name": "Ilse's Notebook", "air_date": "2013-12-09"},
                    {"episode_number": 2, "name": ""},
                ]
            },
            1: {
                "episodes": [
                    {"episode_number": 1, "name": "To You, in 2000 Years", "air_date": "2013-04-07"}
                ]
            },
        },
    )
    assert [s.season_number for s in tmdb_ident.seasons] == [0, 1]
    assert tmdb_ident.seasons[0].title == "Specials"
    assert tmdb_ident.seasons[0].episodes[1].title == ""
    assert tmdb_ident.seasons[1].episodes[0].title == "To You, in 2000 Years"

    tvmaze_ident = TVMazeMetadataProvider.normalize_show_payload(
        {
            "id": 999,
            "name": "Attack on Titan",
            "premiered": "2013-04-07",
            "_embedded": {
                "seasons": [
                    {"number": 0, "name": "Specials", "episodeOrder": 1},
                    {"number": 1, "name": "Season 1", "episodeOrder": 2},
                ],
                "episodes": [
                    {"season": 0, "number": 1, "name": "Ilse's Notebook"},
                    {"season": 1, "number": 1, "name": "To You, in 2000 Years"},
                    {"season": 1, "number": 2, "name": ""},
                ],
            },
        }
    )
    assert [s.season_number for s in tvmaze_ident.seasons] == [0, 1]
    assert tvmaze_ident.seasons[0].title == "Specials"
    assert tvmaze_ident.seasons[1].episodes[1].title == ""


def test_frontend_batch_episode_list_catalog_matching_and_components(tmp_path: Path) -> None:
    """Bundle and execute batchEpisodeListTest.ts covering official catalog, matching, and React states."""
    ui_dir = UI_INDEX_PATH.parents[1]
    esbuild_bin = ui_dir / "node_modules" / ".bin" / "esbuild"
    test_entry = ui_dir / "src" / "lib" / "batchEpisodeListTest.ts"
    bundle_out = tmp_path / "batchEpisodeListTest.cjs"

    build_proc = subprocess.run(
        [
            str(esbuild_bin),
            str(test_entry),
            "--bundle",
            "--platform=node",
            "--format=cjs",
            f"--outfile={bundle_out}",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert build_proc.returncode == 0, build_proc.stderr

    env = os.environ.copy()
    env["NODE_PATH"] = str(ui_dir / "node_modules")
    run_proc = subprocess.run(
        ["node", str(bundle_out)],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    assert run_proc.returncode == 0, run_proc.stderr + run_proc.stdout
    assert "ALL_BATCH_EPISODE_LIST_TS_TESTS_PASSED" in run_proc.stdout
