from __future__ import annotations

import hashlib
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from gui_scraper.app import UI_INDEX_PATH, ScraperBridge, _find_intezam_binary
from gui_scraper.episode_resolver import (
    EpisodeResolver,
    make_intezam_magnet_fetcher,
    parse_media_path,
)
from gui_scraper.matching import match_release_to_identity, parse_release_title
from gui_scraper.models import (
    EpisodeMetadata,
    MediaIdentity,
    ScrapedItem,
    SeasonMetadata,
    TorrentRelease,
)
from gui_scraper.providers import MultiProviderDiscoveryEngine, format_bytes_label
from gui_scraper.storage import LocalMediaStore
from gui_scraper.torrent_manifest import (
    TorrentManifestError,
    TorrentManifestFile,
    TorrentManifestInspector,
    TorrentManifestResult,
    validate_remote_torrent_url,
)


def _bencode(value: Any) -> bytes:
    if isinstance(value, int):
        return f"i{value}e".encode("ascii")
    if isinstance(value, bytes):
        return f"{len(value)}:".encode("ascii") + value
    if isinstance(value, str):
        raw = value.encode("utf-8")
        return f"{len(raw)}:".encode("ascii") + raw
    if isinstance(value, list):
        return b"l" + b"".join(_bencode(v) for v in value) + b"e"
    if isinstance(value, dict):
        items = sorted(value.items(), key=lambda kv: kv[0].encode("utf-8") if isinstance(kv[0], str) else kv[0])
        return b"d" + b"".join(_bencode(k) + _bencode(v) for k, v in items) + b"e"
    raise TypeError(type(value))


def _make_manifest(
    *,
    info_hash: str,
    torrent_name: str,
    entries: list[tuple[int, str, int, str]],
    source_kind: str = "torrent_url",
) -> TorrentManifestResult:
    files: list[TorrentManifestFile] = []
    v_cnt = s_cnt = a_cnt = total = 0
    for idx, rel_path, size_bytes, ftype in entries:
        fname = rel_path.rsplit("/", 1)[-1]
        ext = "." + fname.rsplit(".", 1)[-1].lower() if "." in fname else ""
        files.append(
            TorrentManifestFile(
                relative_path=rel_path,
                filename=fname,
                size_bytes=size_bytes,
                size_label=format_bytes_label(size_bytes),
                file_type=ftype,
                extension=ext,
                torrent_file_index=idx,
            )
        )
        total += size_bytes
        if ftype == "video":
            v_cnt += 1
        elif ftype == "subtitle":
            s_cnt += 1
        else:
            a_cnt += 1
    return TorrentManifestResult(
        status="verified",
        info_hash=info_hash.lower(),
        source_kind=source_kind,
        cached=False,
        torrent_name=torrent_name,
        total_files=len(files),
        total_size_bytes=total,
        total_size_label=format_bytes_label(total),
        video_count=v_cnt,
        subtitle_count=s_cnt,
        auxiliary_count=a_cnt,
        files=files,
    )


def _make_item(
    rid: int,
    title: str,
    info_hash: str,
    *,
    seeders: int = 50,
    leechers: int = 4,
    provider_id: str = "nyaa",
    provider_name: str = "Nyaa",
    torrent_url: str = "",
    magnet_suffix: str = "&tr=udp://tracker.opentrackr.org:1337/announce",
    size: str = "450 MiB",
) -> ScrapedItem:
    ih = info_hash.lower()
    magnet = f"magnet:?xt=urn:btih:{ih}&dn={title.replace(' ', '+')}{magnet_suffix}"
    return ScrapedItem(
        id=rid,
        title=title,
        release_title=title,
        size=size,
        file_size=size,
        date="2025-01-01 12:00",
        stats=f"{seeders} S / {leechers} L",
        raw_link=magnet,
        resolved_link=magnet,
        is_valid=True,
        verification_msg="ok",
        magnet_link=magnet,
        magnet_uri=magnet,
        torrent_link=torrent_url,
        torrent_url=torrent_url,
        info_hash=info_hash,
        seeders=seeders,
        leechers=leechers,
        provider_id=provider_id,
        provider_name=provider_name,
    )


def _seed_inspector(manifests_by_url: dict[str, TorrentManifestResult]) -> TorrentManifestInspector:
    inspector = TorrentManifestInspector()
    now = time.monotonic()
    for url, res in manifests_by_url.items():
        if res.info_hash:
            inspector._cache_by_hash[res.info_hash.lower()] = (now, res)
        inspector._cache_by_url[url] = (now, res)
    return inspector


def _show_identity(
    canonical_id: str,
    official_title: str,
    *,
    media_type: str = "anime",
    country: str = "JP",
    alt_titles: list[str] | None = None,
    seasons: list[tuple[int, int]] | None = None,
    include_specials: bool = False,
) -> MediaIdentity:
    s_list: list[SeasonMetadata] = []
    if include_specials:
        s_list.append(
            SeasonMetadata(
                season_number=0,
                title="Specials",
                episode_count=2,
                episodes=[EpisodeMetadata(episode_number=1, season_number=0, title="OVA 1")],
            )
        )
    for s_num, count in seasons or [(1, 12)]:
        s_list.append(
            SeasonMetadata(
                season_number=s_num,
                title=f"Season {s_num}",
                episode_count=count,
                episodes=[
                    EpisodeMetadata(
                        episode_number=i,
                        season_number=s_num,
                        title=f"{official_title} Ep {i}",
                    )
                    for i in range(1, count + 1)
                ],
            )
        )
    return MediaIdentity(
        canonical_id=canonical_id,
        official_title=official_title,
        alternate_titles=alt_titles or [],
        media_type=media_type,
        country=country,
        seasons=s_list,
    )


# ---------------------------------------------------------------------------
# Stage 2 & Matching Regression Tests
# ---------------------------------------------------------------------------


def test_media_identity_populates_absolute_episodes_and_specials() -> None:
    ident = _show_identity("anilist:16498", "Attack on Titan", seasons=[(1, 25), (2, 12)], include_specials=True)
    special = next(s for s in ident.seasons if s.season_number == 0)
    s1 = next(s for s in ident.seasons if s.season_number == 1)
    s2 = next(s for s in ident.seasons if s.season_number == 2)
    assert special.episodes[0].is_special is True
    assert special.episodes[0].absolute_episode is None
    assert s1.episodes[6].absolute_episode == 7
    assert s2.episodes[3].absolute_episode == 29

    # Ensure matching.py absolute season mapping skips Season 0 Specials
    rel = TorrentRelease(
        release_id="1",
        provider_id="nyaa",
        provider_name="Nyaa",
        title="[Group] Attack on Titan - 29 [1080p].mkv",
        release_title="[Group] Attack on Titan - 29 [1080p].mkv",
        magnet_uri="magnet:?xt=urn:btih:" + "a" * 40,
        size="450 MiB",
        seeders=20,
    )
    matched = match_release_to_identity(rel, ident)
    assert matched.season_number == 2
    assert matched.episode_number == 4


def test_parse_media_path_deterministic_cases() -> None:
    ident_86 = _show_identity("anilist:116589", "86", alt_titles=["86 Eighty-Six"], seasons=[(1, 11)])
    p_86 = parse_media_path("[SubsPlease] 86 - 07 (1080p) [A1B2C3D4].mkv", ident_86)
    assert p_86.episode == 7 and not p_86.is_ambiguous_container

    p_1899 = parse_media_path("1899.S01E03.1080p.WEB-DL.x265-GRP.mkv")
    assert p_1899.season == 1 and p_1899.episode == 3

    p_multi = parse_media_path("Breaking.Bad.S01E07-E08.1080p.BluRay.mkv")
    assert p_multi.season == 1 and p_multi.episode == 7 and p_multi.episode_end == 8 and p_multi.is_multi_episode

    p_ver = parse_media_path("[SubsPlease] Dandadan - 07v2 (1080p).mkv")
    assert p_ver.episode == 7 and p_ver.version == 2

    p_extra = parse_media_path("Extras/NCOP1 [1080p].mkv")
    assert p_extra.is_extra_or_non_episode is True

    p_disc = parse_media_path("Disc 1.iso")
    assert p_disc.is_ambiguous_container is True


# ---------------------------------------------------------------------------
# All 19 Mandatory Resolution Scenarios & 10 Required Show Titles
# ---------------------------------------------------------------------------


def test_mandatory_resolution_scenarios_and_ranking() -> None:
    # Scenario 1 & 2 & 3 & 8 & 9 & 10 & 14: Multi-season pack + individual torrent + extras + subtitles + wrong ep
    bb = _show_identity("tmdb:tv:1396", "Breaking Bad", media_type="tv", country="US", seasons=[(1, 7), (2, 13)])

    h_single = "1" * 40
    h_pack = "2" * 40
    h_multi_ep = "3" * 40
    h_ambig = "4" * 40
    h_wrong = "5" * 40

    m_single = _make_manifest(
        info_hash=h_single,
        torrent_name="Breaking.Bad.S01E07.1080p.BluRay.mkv",
        entries=[
            (0, "Breaking.Bad.S01E07.1080p.BluRay.srt", 45_000, "subtitle"),
            (1, "Breaking.Bad.S01E07.1080p.BluRay.mkv", 1_450_000_000, "video"),
        ],
    )
    m_pack = _make_manifest(
        info_hash=h_pack,
        torrent_name="Breaking Bad Complete",
        entries=[
            (0, "cover.jpg", 120_000, "auxiliary"),
            (1, "Season 1/Episode 01.mkv", 420_000_000, "video"),
            (2, "Season 1/Episode 06.mkv", 430_000_000, "video"),
            (3, "Season 1/Episode 07.mkv", 450_000_000, "video"),
            (4, "Season 1/Episode 07.ass", 60_000, "subtitle"),
            (5, "Season 2/Episode 07.mkv", 510_000_000, "video"),
            (6, "Extras/NCOP1.mkv", 90_000_000, "video"),
            (7, "Extras/Trailer.mp4", 35_000_000, "video"),
        ],
    )
    m_multi_ep = _make_manifest(
        info_hash=h_multi_ep,
        torrent_name="Breaking.Bad.S01E07-E08.720p.mkv",
        entries=[(0, "Breaking.Bad.S01E07-E08.720p.mkv", 920_000_000, "video")],
    )
    m_ambig = _make_manifest(
        info_hash=h_ambig,
        torrent_name="Breaking Bad DVD",
        entries=[(0, "Disc 1.iso", 4_700_000_000, "auxiliary"), (1, "Part 1.mkv", 2_000_000_000, "video")],
    )
    m_wrong = _make_manifest(
        info_hash=h_wrong,
        torrent_name="Breaking.Bad.S01E08.1080p.mkv",
        entries=[(0, "Breaking.Bad.S01E08.1080p.mkv", 1_200_000_000, "video")],
    )

    releases = [
        _make_item(1, "Breaking.Bad.S01E07.1080p.BluRay.mkv", h_single, seeders=120, torrent_url="https://nyaa.si/d/1.torrent"),
        # Duplicate uppercase info_hash from another provider to test BTIH dedup + magnet preservation
        _make_item(2, "Breaking.Bad.S01E07.1080p.BluRay.mkv", h_single.upper(), seeders=40, provider_id="eztv", provider_name="EZTV", torrent_url="https://eztv.yt/1.torrent"),
        _make_item(3, "Breaking Bad Complete Batch (S01-S02)", h_pack, seeders=240, torrent_url="https://nyaa.si/d/2.torrent"),
        _make_item(4, "Breaking.Bad.S01E07-E08.720p.mkv", h_multi_ep, seeders=80, torrent_url="https://nyaa.si/d/3.torrent"),
        _make_item(5, "Breaking Bad Complete Pack Discs", h_ambig, seeders=90, torrent_url="https://nyaa.si/d/4.torrent"),
        _make_item(6, "Breaking.Bad.S01E08.1080p.mkv", h_wrong, seeders=300, torrent_url="https://nyaa.si/d/5.torrent"),
    ]

    inspector = _seed_inspector({
        "https://nyaa.si/d/1.torrent": m_single,
        "https://eztv.yt/1.torrent": m_single,
        "https://nyaa.si/d/2.torrent": m_pack,
        "https://nyaa.si/d/3.torrent": m_multi_ep,
        "https://nyaa.si/d/4.torrent": m_ambig,
        "https://nyaa.si/d/5.torrent": m_wrong,
    })
    resolver = EpisodeResolver(MultiProviderDiscoveryEngine(), inspector)
    res = resolver.resolve_episode(bb, season=1, episode=7, injected_releases=releases)

    # Dedup verified
    assert res.metrics["deduplicated_torrents"] == 1
    assert res.rejected_count >= 1
    assert len(res.candidates) == 3

    # Confirmed targets for S01E07 ONLY: single ep index 1 + pack index 3
    confirmed = [c for c in res.candidates if c.status == "confirmed"]
    assert len(confirmed) == 2
    assert all(c.can_auto_play for c in confirmed)
    assert {c.file_path for c in confirmed} == {
        "Breaking.Bad.S01E07.1080p.BluRay.mkv",
        "Season 1/Episode 07.mkv",
    }
    pack_cand = next(c for c in confirmed if c.file_path == "Season 1/Episode 07.mkv")
    assert pack_cand.torrent_file_index == 3
    assert pack_cand.is_batch_source is True
    # Full magnet URI with trackers preserved
    assert "&tr=udp://tracker.opentrackr.org:1337/announce" in pack_cand.magnet_uri
    assert "Season 2/Episode 07.mkv" not in {c.file_path for c in res.candidates}

    # Multi-episode is probable (NOT confirmed)
    multi = next(c for c in res.candidates if c.info_hash == h_multi_ep)
    assert multi.status == "probable" and multi.can_auto_play is False

    # Ambiguous Part 1.mkv placed in unresolved, NOT confirmed
    assert any(u.info_hash == h_ambig and u.can_auto_play is False for u in res.unresolved_candidates)

    # S02E07 picks Season 2/Episode 07.mkv at index 5
    res_s2 = resolver.resolve_episode(bb, season=2, episode=7, injected_releases=releases)
    s2_conf = [c for c in res_s2.candidates if c.status == "confirmed"]
    assert len(s2_conf) == 1
    assert s2_conf[0].torrent_file_index == 5
    assert s2_conf[0].file_path == "Season 2/Episode 07.mkv"


def test_required_10_shows_specials_spinoffs_and_title_numbers() -> None:
    inspector = TorrentManifestInspector()

    # 1. Attack on Titan (absolute Ep 29 -> S02E04 + Specials S00E01)
    aot = _show_identity("anilist:16498", "Attack on Titan", seasons=[(1, 25), (2, 12)], include_specials=True)
    h_aot = "a1" * 20
    m_aot = _make_manifest(
        info_hash=h_aot,
        torrent_name="AoT Collection",
        entries=[
            (0, "Specials/OVA 01.mkv", 300_000_000, "video"),
            (1, "Season 1/Episode 01.mkv", 350_000_000, "video"),
            (2, "[Group] Shingeki no Kyojin - 29 [1080p].mkv", 410_000_000, "video"),
        ],
    )
    inspector._cache_by_hash[h_aot] = (time.monotonic(), m_aot)
    resolver = EpisodeResolver(MultiProviderDiscoveryEngine(), inspector)

    r_s2e4 = resolver.resolve_episode(
        aot,
        2,
        4,
        injected_releases=[_make_item(1, "Attack on Titan Batch", h_aot, torrent_url="https://nyaa.si/aot.torrent")],
    )
    assert r_s2e4.candidates[0].status == "confirmed"
    assert r_s2e4.candidates[0].torrent_file_index == 2

    # Special OVA never matches S01E01, DOES match S00E01
    r_s1e1 = resolver.resolve_episode(
        aot,
        1,
        1,
        injected_releases=[_make_item(2, "Attack on Titan Batch", h_aot, torrent_url="https://nyaa.si/aot.torrent")],
    )
    assert {c.torrent_file_index for c in r_s1e1.candidates} == {1}
    r_sp1 = resolver.resolve_episode(
        aot,
        0,
        1,
        injected_releases=[_make_item(3, "Attack on Titan Batch", h_aot, torrent_url="https://nyaa.si/aot.torrent")],
    )
    assert len(r_sp1.candidates) == 1 and r_sp1.candidates[0].torrent_file_index == 0

    # 2. Naruto vs Naruto Shippuden spinoff conflict hard rejection
    naruto = _show_identity("anilist:20", "Naruto", seasons=[(1, 220)])
    h_shippuden = "b2" * 20
    inspector._cache_by_hash[h_shippuden] = (
        time.monotonic(),
        _make_manifest(
            info_hash=h_shippuden,
            torrent_name="Naruto Shippuden",
            entries=[(0, "[Group] Naruto Shippuden - 07 [1080p].mkv", 400_000_000, "video")],
        ),
    )
    r_naruto = resolver.resolve_episode(
        naruto,
        1,
        7,
        injected_releases=[_make_item(4, "[Group] Naruto Shippuden - 07 [1080p].mkv", h_shippuden)],
    )
    assert r_naruto.candidates == [] and r_naruto.rejected_count >= 1

    # 3. Bleach vs Bleach TYBW conflict + One Piece Anime vs Live Action conflict
    bleach = _show_identity("anilist:269", "Bleach", seasons=[(1, 366)])
    r_bleach = resolver.resolve_episode(
        bleach,
        1,
        7,
        injected_releases=[_make_item(5, "[SubsPlease] Bleach - Sennen Kessen-hen (TYBW) - 07 [1080p].mkv", "c3" * 20)],
    )
    assert r_bleach.candidates == [] and r_bleach.rejected_count >= 1

    one_piece = _show_identity("anilist:21", "One Piece", media_type="anime", seasons=[(1, 1100)])
    r_op_la = resolver.resolve_episode(
        one_piece,
        1,
        1,
        injected_releases=[_make_item(6, "One Piece Live Action S01E01 1080p", "d4" * 20)],
    )
    assert r_op_la.candidates == [] and r_op_la.rejected_count >= 1

    # 4. 86 Eighty-Six title number protection + Frieren + Dandadan v2 + Solo Leveling + The Office US vs UK
    show_86 = _show_identity("anilist:116589", "86", alt_titles=["86 Eighty-Six"], seasons=[(1, 11)])
    h_86 = "e5" * 20
    inspector._cache_by_hash[h_86] = (
        time.monotonic(),
        _make_manifest(
            info_hash=h_86,
            torrent_name="86 Pack",
            entries=[(0, "[SubsPlease] 86 - 07 (1080p) [ABCDEF12].mkv", 500_000_000, "video")],
        ),
    )
    r_86 = resolver.resolve_episode(show_86, 1, 7, injected_releases=[_make_item(7, "[SubsPlease] 86 - 07 (1080p)", h_86)])
    assert len(r_86.candidates) == 1 and r_86.candidates[0].status == "confirmed"

    office_us = _show_identity("tmdb:tv:2316", "The Office", media_type="tv", country="US", seasons=[(1, 6)])
    r_office_uk = resolver.resolve_episode(
        office_us,
        1,
        1,
        injected_releases=[_make_item(8, "The.Office.UK.S01E01.1080p.mkv", "f6" * 20)],
    )
    assert r_office_uk.candidates == [] and r_office_uk.rejected_count >= 1

    # Frieren, Dandadan (07v2), Solo Leveling
    for s_id, s_title, fname in [
        ("anilist:154587", "Frieren: Beyond Journey's End", "[Group] Sousou no Frieren - 07 [1080p].mkv"),
        ("anilist:171018", "Dandadan", "[SubsPlease] Dandadan - 07v2 (1080p) [1234ABCD].mkv"),
        ("anilist:151807", "Solo Leveling", "[SubsPlease] Solo Leveling - 07 (1080p).mkv"),
    ]:
        show_i = _show_identity(s_id, s_title, seasons=[(1, 12)])
        ih = hashlib.sha1(s_title.encode()).hexdigest()
        inspector._cache_by_hash[ih] = (
            time.monotonic(),
            _make_manifest(info_hash=ih, torrent_name=s_title, entries=[(0, fname, 650_000_000, "video")]),
        )
        r_show = resolver.resolve_episode(show_i, 1, 7, injected_releases=[_make_item(9, fname, ih)])
        assert len(r_show.candidates) == 1 and r_show.candidates[0].status == "confirmed"


def test_offline_magnet_remains_unresolved_and_cancellation_stops_cleanly() -> None:
    show = _show_identity("anilist:1", "Dandadan", seasons=[(1, 12)])

    started = threading.Event()

    def _slow_fetcher(magnet_uri: str, timeout: float, expected_hash: str) -> TorrentManifestResult:
        started.set()
        time.sleep(0.15)
        return TorrentManifestResult(
            status="unavailable",
            info_hash=expected_hash,
            source_kind="libtorrent_magnet",
            error="Timed out retrieving magnet metadata",
        )

    inspector = TorrentManifestInspector(magnet_metadata_fetcher=_slow_fetcher)
    resolver = EpisodeResolver(MultiProviderDiscoveryEngine(), inspector)

    h_offline = "77" * 20
    offline_rel = _make_item(1, "[SubsPlease] Dandadan - 07 (1080p)", h_offline)
    res_offline = resolver.resolve_episode(
        show,
        1,
        7,
        options={"allow_magnet_peers": False},
        injected_releases=[offline_rel],
    )
    assert res_offline.candidates == []
    assert len(res_offline.unresolved_candidates) == 1
    assert res_offline.unresolved_candidates[0].can_auto_play is False

    # Cooperative cancellation mid-flight
    results_holder: list[Any] = []

    def _run_cancelable() -> None:
        results_holder.append(
            resolver.resolve_episode(
                show,
                1,
                7,
                options={"request_id": "cancel-me-1", "allow_magnet_peers": True},
                injected_releases=[offline_rel],
            )
        )

    th = threading.Thread(target=_run_cancelable)
    th.start()
    started.wait(timeout=2.0)
    assert resolver.cancel("cancel-me-1") is True
    th.join(timeout=3.0)
    assert len(results_holder) == 1
    assert results_holder[0].metrics["cancelled"] is True
    assert results_holder[0].resolution_stage == "cancelled"


def test_ssrf_and_redirect_blocking_enforced() -> None:
    for blocked in (
        "http://127.0.0.1/test.torrent",
        "http://localhost:8080/a.torrent",
        "http://169.254.169.254/latest.torrent",
        "http://10.0.0.5/private.torrent",
        "file:///etc/passwd",
    ):
        try:
            validate_remote_torrent_url(blocked, allow_private_urls=False, resolve_dns=False)
            raise AssertionError(f"Expected SSRF block for {blocked}")
        except TorrentManifestError:
            pass


def test_bridge_exact_file_index_handoff_no_hardcoded_one(tmp_path: Path) -> None:
    bridge = ScraperBridge(store=LocalMediaStore(data_dir=tmp_path))
    magnet = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=ShowBatch&tr=udp://tracker.opentrackr.org:1337/announce"

    class _FakeProc:
        def __init__(self) -> None:
            self.stdin = self
            self.stdout = iter([])
            self.writes: list[str] = []

        def write(self, text: str) -> None:
            self.writes.append(text)

        def flush(self) -> None:
            pass

        def close(self) -> None:
            pass

        def poll(self) -> int | None:
            return 0

        def wait(self, timeout: float | None = None) -> int:
            return 0

        def terminate(self) -> None:
            pass

        def kill(self) -> None:
            pass

    captured_cmds: list[list[str]] = []
    captured_procs: list[_FakeProc] = []

    def _fake_popen(cmd: list[str], **kwargs: Any) -> _FakeProc:
        captured_cmds.append(cmd)
        proc = _FakeProc()
        captured_procs.append(proc)
        return proc

    with (
        patch("gui_scraper.app._find_intezam_binary", return_value=Path("/usr/local/bin/intezam")),
        patch("gui_scraper.app.subprocess.Popen", side_effect=_fake_popen),
    ):
        # 1. Explicit episode target handoff
        res = bridge.launch_intezam_stream(
            magnet,
            title="Breaking Bad — S01E07",
            strict_cache=True,
            timeout_seconds=90,
            torrent_file_index=6,
            expected_info_hash="0123456789abcdef0123456789abcdef01234567",
            expected_file_path="Season 1/Episode 07.mkv",
            expected_file_size=450_000_000,
        )
        assert res["ok"] is True
        cmd = captured_cmds[0]
        assert "--file-index" in cmd and cmd[cmd.index("--file-index") + 1] == "6"
        assert "--expected-info-hash" in cmd
        assert "--expected-file-path" in cmd and "Season 1/Episode 07.mkv" in cmd
        assert "--expected-file-size" in cmd and "450000000" in cmd
        # Critical regression assertion: NEVER send hardcoded \n1\n!
        assert captured_procs[0].writes == [f"{magnet}\n"]

        # 2. Legacy single-video magnet without index passes --select-single, NEVER \n1\n
        res_single = bridge.launch_intezam_stream(magnet, title="Movie", timeout_seconds=60)
        assert res_single["ok"] is True
        assert "--select-single" in captured_cmds[1]
        assert captured_procs[1].writes == [f"{magnet}\n"]

        # 3. Negative file index rejected cleanly
        bad_res = bridge.launch_intezam_stream(magnet, torrent_file_index=-1)
        assert bad_res["ok"] is False


def test_compiled_cpp_manifest_json_flag_and_frontend_ui_tests(tmp_path: Path) -> None:
    binary = _find_intezam_binary()
    assert binary is not None, "Expected compiled ./build/intezam binary"
    fetcher = make_intezam_magnet_fetcher(lambda: binary)
    # Invalid magnet invocation exercises the compiled C++ --metadata-only --manifest-json error path safely without network
    res = fetcher("magnet:?xt=invalid", 3.0, "")
    assert res.status == "error"

    # Run esbuild + node test harness covering Episode-First UI components and episodeCatalog
    ui_dir = UI_INDEX_PATH.parents[1]
    esbuild_bin = ui_dir / "node_modules" / ".bin" / "esbuild"
    test_entry = ui_dir / "src" / "lib" / "episodeResolutionUiTest.ts"
    bundle_out = tmp_path / "episodeResolutionUiTest.cjs"

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
    env = {"PATH": str(ui_dir / "node_modules" / ".bin") + ":" + "/usr/local/bin:/usr/bin:/bin", "NODE_PATH": str(ui_dir / "node_modules")}
    run_proc = subprocess.run(["node", str(bundle_out)], capture_output=True, text=True, env=env, check=False)
    assert run_proc.returncode == 0, run_proc.stderr + run_proc.stdout
    assert "ALL_EPISODE_RESOLUTION_UI_TESTS_PASSED" in run_proc.stdout
