# Adaptive Streaming Engine 2.0

The default streaming cache is based on playback time and observed verified
delivery, replacing the video-size percentage target. Explicit legacy modes
remain compatible. GUI files, searching and metadata retrieval were not changed.

## Pipeline and existing safeguards

See [the repository audit](engine2-audit.md). Peers → libtorrent picker → bounded
disk writes → libtorrent SHA-1/SHA-256 verification → PieceStore verified mappings
→ pinned read leases → video source → existing LibVLC callbacks. A piece-pass
plugin is the authority for verification. Raw disk jobs are never playback data.
Partial writes, stale cache contents and scattered verified pieces do not count
as consecutive playable buffer.

Container probing still uses verified reads with its unique-piece scan guard.
Adaptive sessions retain the detected metadata pieces across seeks. Metadata and
playback use the same physical slots; they are not separate hidden allocations.

## Configuration and compatibility

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/intezam --cache-max-mib 128 --cache-free-reserve-mib 512 --buffer-seconds 30
```

| Option | Meaning | Default |
|---|---|---|
| `--cache-max-mib N` | Absolute managed allocation ceiling, including filesystem overhead | 256 MiB |
| `--cache-free-reserve-mib N` | Free filesystem bytes to retain when allocating or growing | 256 MiB |
| `--buffer-seconds N` | Initial forward horizon before useful throughput is known, 12–600 seconds | 24 seconds |
| `--bitrate N` | Initial estimated selected-video consumption in bytes/s | 1,048,576 |
| `--legacy-cache` | Previous automatic video/40 target with four-piece minimum | off |
| `--cache-max-percent N` | Select legacy automatic mode and set its 3–99% expansion ceiling | 25% in legacy mode |
| `--strict-cache` | Original video/40 budget with no automatic expansion | off |

The two MiB CLI options accept 1–1,048,576. C++ callers can set a zero free-space
reserve explicitly. Options are processed in order; the last explicit legacy
mode selector wins. Absolute options configure adaptive mode and do not change
the behavior of an explicitly selected legacy mode.

`CacheStreamOptions` defaults to `CacheBudgetMode::adaptive`. Low-level
`CacheBudgetOptions{}` remains the previous automatic mode, and the default
`CacheFile::create`/`PieceStore::create` arguments remain strict. Existing callers
that explicitly use strict or automatic mode retain their percentage semantics.
Tests of those historical invariants now select them explicitly; they retain
their original bounds and eviction assertions. CLI selection receives the
configured policy, keeps manual choice in adaptive mode, and checks allocation
again on the real filesystem.

`AdaptiveCacheConfig` also exposes minimum slots (four by default), forward
duration bounds (12–90 seconds), growth interval (15 seconds) and shrink interval
(60 seconds). The minimum allowed growth interval is one second. Increasing the
initial CLI horizon above 90 also raises its maximum to that value.

## Useful delivery and consumption estimates

The piece-pass plugin counts newly verified selected-file bytes, excluding
neighboring-file bytes in boundary pieces. Restoring already verified pieces
after reconstruction adds no delivery. Hash failures and duplicate payload do
not increase this counter. A genuinely re-downloaded evicted piece is useful
delivery again. Raw payload throughput remains a separate display statistic.

For each demand-bearing observation interval `dt`, the estimator updates:

```text
short_weight = 1 - exp(-dt / 2 seconds)
long_weight  = 1 - exp(-dt / 12 seconds)
short_rate  += short_weight * (verified_bytes / dt - short_rate)
long_rate   += long_weight  * (verified_bytes / dt - long_rate)
deviation   += long_weight  * (abs(sample - previous_long_rate) - deviation)
variation    = clamp(deviation / max(1, long_rate), 0, 2)
sustainable  = max(0, min(short_rate, long_rate) - 0.5 * deviation)
```

Pending demand without verification accumulates stall time; after two seconds,
the sustainable estimate receives an additional exponential penalty. Idle time
when the cache is full does not lower the capacity estimate. Peer count is kept
separately. Verification arrives in piece-sized bursts, so large pieces and slow
hashing can make this estimate conservative. Raw payload rate, disk-job timing,
peer count and verified throughput remain separate diagnostics; zero verified
rate alone does not prove that the physical network is idle.

Configured consumption is the initial estimate. Existing LibVLC integration
refines it from selected-file bytes / detected media duration. This is an average
bitrate, not a precise VBR packet forecast. Buffer seconds therefore remain an
estimate beyond the byte reader, separate from VLC's decoder queues.

## Sizing and the rolling window

With sustainable/consumption ratio `r`, the default base forward duration is
12 seconds for `r >= 2`, 18 for `1.1 <= r < 2`, and 30 below 1.1. Before the first
useful delivery estimate it is the configured initial duration. Add
`12 * variation + min(20, 2 * stalled_seconds)`, then clip to configured duration
bounds. These durations describe the desired safety horizon, not a guaranteed
buffer or a fixed startup wait.

Marginal/insufficient delivery (`r < 1.1`) with less than four verified seconds
also adds six seconds to the requirement before clipping. Thus a depleted
consecutive buffer influences sizing independently of observed link capacity.

```text
forward_bytes = consumption_bytes_per_second * forward_seconds
rolling_bytes = forward_bytes * 1.5
working_slots = ceil(rolling_bytes / torrent_piece_bytes) + alignment allowance
working_slots += union(protected/pinned/unfinished pieces outside rolling range)
staging_slots = working_slots + 2
required_slots = max(minimum_slots, staging_slots + ceil(staging_slots / 7))
```

Useful selected-video coverage caps working slots. Metadata overlapping the
rolling window is counted once. Unfinished pieces already inside that window
are counted once. Two staging slots cover immediate reads/partial work.
Reservation is `ceil(total_slots / 8)`; solving for total slots requires the
`ceil(staging_slots / 7)` term, rather than adding working/8 and underfunding the
window. The physical stride is rounded to the filesystem allocation unit.
Measured root/session/lock overhead and one allocation-unit reserve also count
toward the absolute ceiling.

The controller reports uncapped requirements, capped desired slots, physical
slot capacity and occupied slot capacity separately. Initial allocation must
fit at least four aligned pieces, the absolute maximum, and available bytes
minus the free-space reserve. Impossible requests fail with a storage-specific
error rather than exceeding a cap. A small video can receive more than 2.5%;
increasing video size alone does not increase a low-bitrate stream's target.

Runtime target changes require at least two slots or 25% change, two seconds of
directional confirmation, and the corresponding cooldown. A reduced hard limit
is applied immediately to the logical target. Single late spikes after a long
idle cooldown still require confirmation. Capacity changes do not force torrent
reconstruction by themselves.

Essential protected metadata can bypass speculative resize cooldowns. The
minimum protected/current union can also borrow reservation space in a tiny
cache. Neither exception bypasses the absolute or available-space limit.

`DynamicSegmentCache` receives the adaptive forward duration and logical slot
limit. Half-piece segments, approximately one third behind/two thirds ahead,
overlap retention, movement/resize hysteresis and immediate explicit repositioning
remain. Near file boundaries the window is clipped. Urgent metadata participates
in the fit, and the final piece union cannot exceed available working slots.

## Buffer states and scheduling

The controller distinguishes Initializing, Prebuffering, Playing, Buffer Low,
Recovering, Seeking and Stalled. Forward buffer walks consecutive verified
pieces from the byte reader until the first gap; scattered verified data is
excluded. Initial readiness uses an adaptive startup target; a low-water warning
does not pause playback. A sustained waiting read below the emergency threshold
can pause through the existing 250 ms gate. Resumption requires its separate
higher threshold. Manual pause remains authoritative.

Before capacity clipping, startup targets are 3 seconds on a fast stable swarm,
6 near sustainable throughput, 16 under a measured deficit, plus a variability
allowance; unknown throughput uses 8. Startup is capped at 60% of the available
forward target; resume at 80% or 1.25x startup, whichever is smaller. Warning is
at most 3 seconds and emergency at most 0.5 seconds. The video source additionally
clips startup/resume to the actual planned range, remaining file and one third
of physical data capacity. Small or constrained caches can provide less.

If sustainable throughput is below consumption, diagnostics expose the deficit
and `verified_buffer_bytes / (consumption - sustainable)` depletion estimate.
No finite cache guarantees continuous playback through an unlimited deficit.

| Category | Priority |
|---|---:|
| Blocking read/metadata and imminent playback | 7 plus deadline |
| Near future, normally within about 8 seconds | 6 |
| Distant forward prefetch | 4, reduced to 1 during low buffer |
| Backward retention | 1 |
| Outside the admitted plan | 0 |

Only changed priorities are published. Absolute deadlines remain stable unless
a piece becomes materially more urgent or a genuine seek starts a new epoch.
Libtorrent promotes deadline pieces internally, so deadlines are limited to the
imminent set. Priority 7 changes selection order, not internet bandwidth.
[Libtorrent's public priority/deadline API](https://www.libtorrent.org/reference-Torrent_Handle.html)
describes these controls.

Adaptive sessions disable `close_redundant_connections`: completing a temporary
window must not announce upload-only and disconnect useful seeds before the
next window needs them. The new small-video playback regression exposed this
idle-window stall. Existing configured bandwidth limits, transport behavior and
connection-count limits remain intact.

## Physical adaptation and eviction safety

Expansion appends preallocated slots to the same file while holding PieceStore's
I/O mutex. That mutex also covers writes, hashes, admissions and lease reads.
Slot offsets, verification state and pins remain valid. Published slot count is
atomic for observers. Newly allocated file ranges are zero-filled by the OS;
reused slots are cleared. A free-index set avoids repeatedly scanning all occupied
slots during admission. Disk space and actual blocks are checked before publishing
new slots. ENOSPC rolls back only an unpublished suffix and leaves the previous
working cache available; the capacity constraint is exposed in diagnostics.

Physical shrinking is **deferred to teardown/next session**. A smaller logical
target reduces future demand; it never truncates live referenced extents. Peak
allocation can therefore stay above a later logical target, while remaining
within the configured physical maximum. File allocation is per session; it is
not a global quota against other processes or snapshots. Concurrent consumers
can exhaust free space after a successful check; allocation/write errors remain
explicit. tmpfs/ramfs remain unsupported.

Eviction still uses the supported pause → snapshot → remove → storage teardown
→ grouped reclaim → re-add barrier. Free slots are used first, overlapping
verified data and partial blocks survive, pinned/protected pieces are preserved,
and peer endpoints are restored. Normal refills batch cold slots; a short verified
runway can fund an early single-slot refill in a tiny cache. No completed piece
is overwritten while libtorrent still advertises it. Larger useful windows and
retained peers reduce churn; they do not remove the need for that barrier.

Synchronous bounded disk jobs and expansion can briefly hold the network/store
path. This update does not introduce an asynchronous disk queue. Rebuild and
disk-job costs are measured rather than assumed negligible.

## Diagnostics

`CacheDiagnostics` exposes consumption, raw payload rate, short/long/sustainable
verified delivery, variability, demand-stall duration, peers, consecutive buffer,
buffer targets/state, uncapped/capped sizing, desired/physical/occupied bytes,
deferred shrinking and storage constraints. Existing `CachePerformance` reports
slots/protection/pins/reservations, rebuild count/timing, evictions, admissions,
priority/deadline changes and disk jobs/timing. Actual versus planned blocking
priority, finished state and last peer error help diagnose stopped scheduling.

`VideoSourceStatus::playback` adds observed first-frame time from source creation,
player buffering duration/stall count and time-seek acknowledgement latency.
These are polled observations, not exact display presentation timestamps.
`ReadPerformance` remains separate: waits of at least 50 ms are read stalls,
which can occur during startup and are not necessarily visible player stalls.
Simulation `status` prints engine metrics; no logging loop or UI redesign was
added. Full validation and benchmark results follow below.

## Validation

The normal build passes **20/20 CTest checks**. The original 16 checks remain;
new checks cover adaptive policy, adaptive storage, adaptive CLI playback, and
adaptive LibVLC playback across the eight generated container fixtures. Qt tests
still pass without changes to the GUI implementation.

Deterministic coverage includes equal-bitrate small/large files, different
consumption and network rates, fluctuating/stalled delivery, idle versus demand
stalls, deficits and depletion, alignment, minimum/absolute/free-space bounds,
target growth/shrink hysteresis, late isolated spikes, essential metadata,
reservation borrowing, scattered versus consecutive verification, priorities,
buffer states/watermarks, and adaptive rolling alignment.

Real loopback storage tests cover pinned leases and concurrent writes during
expansion, failed over-cap expansion, deferred shrinking, retained metadata,
forward/backward/rapid seeks, bounded reconstructions and idle-window peer
retention. Existing tests still reject corrupt payload and test v1/v2 hashing,
stale-session cleanup, cancellation, partial blocks, slot reuse, probes and
decoder recovery. Tiny budgets fitting two protected metadata pieces plus
playback and larger metadata requiring immediate safe expansion have explicit
regressions. The hard-cap preflight also deduplicates metadata overlapping the
current playback segment instead of charging a separate fixed playback allowance.

ASan/UBSan: 12 core checks passed with default leak checking; the eight
media/Qt/fixture checks passed with `ASAN_OPTIONS=detect_leaks=0`. Final controller,
metadata and source changes were also rechecked in the three targeted sanitized
adaptive/refill checks. The exception is for previously documented external
LibVLC/Fontconfig leak reports; it is not a claim that those libraries are
leak-free. No invalid-memory or undefined-behavior report appeared in these runs.
The Qt-free CLI also builds successfully. `git diff --check` passes.

```bash
ctest --test-dir build --output-on-failure -j2
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build-sanitize -j2
ctest --test-dir build-sanitize -E 'qt_|vlc_|cli_' --output-on-failure
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize -R 'qt_|vlc_|cli_' --output-on-failure
```

## Benchmark method and limits

Raw data: [baseline](performance/engine2-before.csv),
[adaptive](performance/engine2-after.csv), and
[complete measured tables](performance/engine2-comparison.md). Each workload has
three repetitions. Before/after runs are sequential, with builds/tests finished.
The baseline is revision `f6a4a77058bdf17dc142f17839fb5fa49693614c`, rebuilt from
an archive with the same benchmark instrumentation; only the two adaptive mode
assignments are omitted because that enum does not exist in the baseline.

The byte fixture is 32 MiB, 16 KiB torrent pieces with a 7,000-byte preceding
file, consumed at 512 KiB/s. Sequential/unstable reads cover 128 × 16 KiB with
hot re-reads, paced at 31.25 ms. The unstable seeder is limited to 1 MiB/s and
pauses independently for 500 ms after step 32. Both engines prebuffer using
their own policy before the same playback workload. Seeking uses the identical
eight destinations and 128 small reads; recovery measures read completion after
each seek request, not a rendered-frame timestamp. It is a synthetic byte-reader
workload, not a codec test. First-frame rows decode the same noisy four-second
H.264/AAC MP4 through headless LibVLC, sampled every 20 ms.

First frame includes source construction, container probing, prebuffer and VLC
setup, excluding seeder fixture creation. Read stalls are waits >=50 ms;
post-start columns subtract prebuffer waits. Player buffering counts observed
LibVLC Buffering or automatic-pause episodes separately. Buffer/utilization are
sample averages, not exact time integrals. Verified throughput counts selected
bytes of completed pieces, including useful re-downloads and prefetch, divided
by workload elapsed time. More prefetch can raise that number without increasing
physical bandwidth. Peak physical allocation includes managed filesystem blocks.

These are real local peers and real decode, with a reproducible simulated outage.
They do not validate public swarms, WAN congestion, every VBR pattern, every
filesystem, audible output or desktop rendering. No public-magnet or full-film
claim is made for this update. Larger cache and startup buffering are explicit
costs; low-throughput streaming can still stall after its buffer is depleted.

Reproduce the current workload and tables with:

```bash
./build/cache_benchmark --repetitions 3 --vlc build/ui-fixtures/front.mp4 > docs/performance/engine2-after.csv
python3 tools/compare_adaptive_engine.py docs/performance/engine2-before.csv \
  docs/performance/engine2-after.csv --output docs/performance/engine2-comparison.md
```

Use a retained fixture, or generate one with `test/generate_media.cmake`;
CTest removes its own fixture directory at cleanup. To rebuild the baseline,
archive the baseline revision's CMake/main/C++ sources and tests into a separate
directory, copy current `test/cache_benchmark.cpp`, remove the two lines assigning
`CacheBudgetMode::adaptive`, configure with `INTEZAM_QT_UI=OFF`, and build its
`cache_benchmark`. Do not replace the working checkout. Both measurements use
the same build type, media bytes, settings, pacing and seeder behavior.

## Measured outcomes and costs

Medians of three identical workloads:

| Metric | Baseline | Adaptive |
|---|---:|---:|
| VLC first decoded frame (ms) | 58.827 | 81.687 |
| VLC post-start verified-read waiting (ms) | 211.746 | 2.207 |
| VLC rebuilds | 102.000 | 0.000 |
| Outage post-start read stalls | 1.000 | 0.000 |
| Outage prebuffer readiness (s) | 2.001 | 5.503 |
| Outage verified buffer (s) | 0.622 | 7.246 |
| Byte-reader peak allocation (MiB) | 0.781 | 20.625 |
| VLC peak allocation (MiB) | 0.312 | 15.219 |
| Seek-request read recovery (ms) | 0.240 | 0.216 |
| Seeking rebuilds | 7.000 | 3.000 |
| Seeking mean rebuild duration (ms) | 1.031 | 6.152 |

The outage workload absorbed its brief disconnection after a longer initial
buffer. The short VLC workload had zero observed player-buffer episodes in both
versions; this does not establish fewer visible stalls on that clip. First-frame
time increased, and the time-based cache used more disk than the tiny legacy
percentage cache. Seek read recovery decreased in this run; fewer reconstructions
do not guarantee cheaper individual reconstructions. Larger admission batches
increased mean seeking rebuild duration. Useful verified delivery in the seek
workload also includes extra prefetch/re-downloads and is not a bandwidth gain.

The goal is a smoother sustainable stream within an explicit ceiling. These
measurements support reduced local refill churn and outage tolerance, with
startup and allocation costs; they do not establish a universal latency win.

## Changed files

```text
CMakeLists.txt
Guide.md
README.md
docs/adaptive-engine-2.md
docs/engine2-audit.md
docs/performance/engine2-after.csv
docs/performance/engine2-before.csv
docs/performance/engine2-comparison.md
main.cpp
src/adaptive_cache_budget.cpp
src/adaptive_cache_budget.hpp
src/cache_budget.cpp
src/cache_budget.hpp
src/cache_disk_io.hpp
src/cache_file.cpp
src/cache_file.hpp
src/cache_metrics.hpp
src/cache_simulation.cpp
src/dynamic_segment_cache.cpp
src/dynamic_segment_cache.hpp
src/piece_store.cpp
src/piece_store.hpp
src/stream_policy.cpp
src/stream_policy.hpp
src/torrent_cache_session.cpp
src/torrent_cache_session.hpp
src/torrent_video_source.cpp
src/torrent_video_source.hpp
src/useful_throughput.cpp
src/useful_throughput.hpp
src/vlc_player.cpp
src/vlc_player.hpp
test/adaptive_engine_tests.cpp
test/adaptive_storage_tests.cpp
test/cache_benchmark.cpp
test/local_video_torrent.hpp
test/playback_tests.cpp
test/streaming_tests.cpp
tools/compare_adaptive_engine.py
```
