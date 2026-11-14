# Adaptive budgets and native playback controls

This document records the initial Qt/adaptive-budget milestone. The newer
[buffering and refill follow-up](smooth-playback.md) increases the safety targets
and preserves downloads across transient reads.

This milestone builds on revision `cc8af88`. Before changes, all 12 existing
CTest checks passed (40.89 seconds). The cache already had protected verified
leases, reserved-slot admission, batch reconstruction, stable rolling windows
and metrics. Those mechanisms remain the basis of streaming.

## Problem and decisions

The old video filter compared `video_size / 40` with one torrent piece. In the
provided torrent, the 60,879,760-byte sample had a 1,521,994-byte target and
2,097,152-byte pieces. It could not fit one piece, so the only remaining main
video was selected automatically. The default now keeps a manual choice and
raises undersized budgets to a useful four-slot minimum.

For filesystem allocation unit `A`, measured managed overhead `O`, selected video
size `V` and whole-piece stride `S = ceil(piece_length / A) * A`:

```text
target    = floor(V / 40)
minimum   = 4 * S + O + A
effective = max(target, minimum)
ceiling   = floor(V * maximum_percent / 100)
```

Automatic allocation fails if effective exceeds ceiling. The default ceiling
is 25%; `--cache-max-percent 3..99` configures it. The session file is preallocated
once. Growth during playback is never used to work around eviction. Actual block
allocation, including managed directories/lock/cache files, must stay within the
effective budget. `--strict-cache` preserves the original 2.5% behavior, including
its only-eligible-video selection. Low-level `CacheFile` and `PieceStore` APIs
retain strict defaults for existing embedders; `CacheStreamOptions` explicitly
uses automatic mode by default. Video-list eligibility uses a conservative
4 KiB-filesystem estimate; the real allocator's measured overhead is authoritative.

On the tested Btrfs filesystem the sample received **8,392,704 bytes (13.79%)**,
with four 2 MiB slots and one 4 KiB allocation reserve. The main video's original
146,745,052-byte target remains sufficient. The physical cache limit still covers
whole torrent pieces, adjacent-file bytes, partial pieces, metadata and pinned
readers. Unverified bytes never reach playback. Reclamation still requires the
supported pause/snapshot/remove/re-add barrier; no unsupported piece reset is used.

## Smoothing policy

- Startup and resume targets are 1 second when observed throughput is at least
  1.5 times estimated consumption, 2 seconds at or above consumption, and 3 seconds
  for slow/unknown throughput. Targets are clipped by the planned forward window,
  one third of data capacity and remaining video bytes, so small caches can fill
  them. Prefix prebuffering retains bounded 16 KiB read chunks.
- The low watermark is `min(0.5 seconds, high / 3)`. A verified-data miss below low
  for 250 ms pauses playback until high returns. Manual pause takes precedence;
  one recovered read cannot bypass the high watermark. Explicit seeks reset this
  state and keep the user's pause setting.
- Forward-piece deadlines use `max(0, distance / consumption - 0.5 seconds)`,
  capped at 60 seconds. Exact urgent reads have immediate deadlines. Behind-window
  retention receives lower priority. Changes in demand, estimated consumption or
  meaningful position progress refresh deadlines even when admission is unchanged.
- Slider tracking is disabled, snapshot updates block signals, and pending
  absolute seeks coalesce. A drag sends one seek when released. ±10-second
  buttons and keyboard shortcuts remain available.

These changes do not add asynchronous disk I/O, a larger RAM cache, parallel
payload files or runtime cache growth. Short VBR samples and slow public swarms
can still stall. Size/duration is an average consumption estimate, and native
hardware decoding remains dependent on the installed VLC plugins/drivers.

## Qt lifecycle and controls

The terminal retrieves metadata and asks for a video. The Qt 6 window then opens
while a background controller probes and buffers. It embeds the native LibVLC 3
X11 output and offers pause/play, stop, ±10 seconds, a timeline, elapsed/duration,
volume/mute, fullscreen and buffer/network/budget readouts. Volume and mute choices
persist across asynchronous output initialization and input reopening.

`XInitThreads()` runs before Qt/LibVLC. Qt uses its process-local `xcb` backend,
including on XWayland desktops; `DISPLAY` is required. Video frames remain in the
native renderer. Closing or stopping requests cancellation immediately, leaves
the output window alive until shutdown finishes, and then closes it. Final teardown also joins the worker before destroying the native
surface if an external application quit bypasses the close event. Commands
and LibVLC calls run on the controller worker; the UI polls snapshots every
200 ms. Terminal-only playback remains available through `--no-ui`; `--headless`
uses software/memory output for automation. `-DINTEZAM_QT_UI=OFF` builds without Qt.

## Measured comparison

Raw single-run results: [before](performance/adaptive-before.csv),
[after](performance/adaptive-after.csv). Both used the existing deterministic
loopback byte workload (`build/cache_benchmark --repetitions 1`). These runs are
regression evidence, not a statistically significant speed claim.

| Metric | Sequential before / after | Seeking before / after |
|---|---:|---:|
| Torrent rebuilds | 14 / 14 | 7 / 7 |
| Average rebuild milliseconds | 0.950 / 0.857 | 1.399 / 1.314 |
| Maximum rebuild milliseconds | 1.306 / 1.236 | 1.592 / 1.928 |
| Window changes | 29 / 29 | 26 / 26 |
| Read hit percentage | 99.632 / 99.632 | 95.455 / 95.455 |
| Verified-data wait milliseconds | 0.983 / 1.021 | 30.824 / 27.052 |
| Read stalls at least 50 ms | 0 / 0 | 0 / 0 |
| Managed allocated bytes | 819,200 / 819,200 | 819,200 / 819,200 |

The new changes target undersized-budget eligibility, user control and recovery
policy, rather than another reduction in reconstruction frequency. Some measured
maxima increased; no overall WAN smoothness improvement is inferred from these
short, fast-peer workloads. The earlier reserved-slot performance comparison
remains in [cache-performance.md](cache-performance.md).

## Validation

- All **15 CTest checks** passed after integration (40.74 seconds), including all
  previous tests and adaptive budget, Qt control and Qt/torrent/VLC checks.
- Budget tests cover strict rejection, ceiling rejection, configurable ceiling,
  baseline retention, real filesystem allocation, cleanup, buffering debounce,
  resume hysteresis, manual pause and rate/distance deadline ordering.
- Qt tests cover seek-on-release, loading-disabled controls, command dispatch,
  narrow-window controls, pause while seeking, volume/mute, fullscreen, stop while
  playing, cancellation while startup reads are blocked and nonblocking close.
- Native X11/XWayland integration decoded **39 video and 71 audio frames** from
  the local torrent, with **327,680 / 347,812 bytes** of managed allocation.
- A bounded native Qt playback test using the user's magnet and the **sample**
  decoded **333 video and 1,327 audio frames**, advancing beyond 6.6 seconds.
  The first decoded snapshot appeared about 28 seconds after source startup.
  Peak allocation was **8,388,608 / 8,392,704 bytes**; cleanup completed. Payload
  received by the final snapshot was about 29.4 MB, including startup/index reads
  and cache replacement. This was a short live check; it does not establish
  sustained playback quality or promise that every swarm will start that quickly.
- Address/UB sanitizer checks passed: nine non-UI/non-playback tests with leak
  checking enabled, and the six Qt/playback/fixture checks with
  `ASAN_OPTIONS=detect_leaks=0`. Prior documented external LibVLC/Fontconfig leak
  reports still prevent a blanket leak-clean claim for plugin/UI playback.

## Changed files

| Files | Purpose |
|---|---|
| `src/cache_budget.*` | Pure aligned budget planning and ceiling enforcement |
| `src/cache_file.*`, `src/piece_store.*` | Apply measured policy to the fixed allocation |
| `src/stream_policy.*` | Watermarks, rebuffer state and consumption deadlines |
| `src/torrent_cache_session.*` | Pass budget options and schedule deadlines/priorities |
| `src/torrent_video_source.*` | Adaptive prefix buffering and reachable target diagnostics |
| `src/dynamic_segment_cache.cpp` | Report the effective cache limit in capacity errors |
| `src/vlc_player.*` | Native handle, volume/mute, user pause and automatic rebuffer control |
| `src/vlc_playback.cpp` | Show the effective budget percentage in terminal playback |
| `src/playback_controller.*` | Worker, command queue, snapshots and cancellation |
| `src/player_window.*`, `src/qt_playback.cpp` | Native Qt controller and safe window lifetime |
| `src/video_selection.*`, `main.cpp` | Manual adaptive choice, CLI policy options and Qt default |
| `CMakeLists.txt` | Qt/X11 option, new modules and test targets |
| `test/cache_policy_tests.cpp`, `test/qt_*tests.cpp`, `test/video_tests.cpp` | Budget, recovery, controls and integration regressions |
| `README.md`, `Guide.md`, `docs/cache-optimization.md`, this document and CSVs | Build, configuration, architecture and validation evidence |
