# Sliding cache optimization

## Investigation

Baseline revision: `6a3505e` (before this optimization). The supported local
libraries are libtorrent 2.1.2 and LibVLC 3.0.24, using C++17 on Linux.

The existing pipeline is metadata-only retrieval -> video selection ->
`TorrentCacheSession` -> custom `CacheDiskIO` -> `PieceStore` -> verified
`ReadLease` -> `TorrentVideoSource` -> LibVLC custom callbacks. Metadata retrieval
uses disabled disk I/O. Only the selected file's intersecting whole pieces enter
the fixed-size cache file.

The bottleneck is `TorrentCacheSession::schedule()`. It recomputes the centered
window on every playback/rate/pump update. `DynamicSegmentCache::window()` moves
its beginning by one segment as the playback position moves. When occupied slots
leave no room for even one new target piece, `PieceStore::needsEviction()` triggers
`TorrentCacheSession::rebuild()`. `PieceStore::reconcile()` then evicts only enough
pieces to satisfy that immediate request, leaving little space for the next move.
Minor position/rate changes can repeat this cycle.

`rebuild()` pauses, waits for pending work, snapshots resume parameters in RAM,
removes the torrent, waits for backend storage release, reconciles slots, and
re-adds with reconstructed availability and priorities. This is necessary because
the supported public libtorrent interface cannot forget one completed piece.
Rebuilding also reconnects peers. Piece prioritization alone cannot invalidate
libtorrent's completed-piece state.

Safety constraints that must remain:

- `CacheFile` preallocates one file within `floor(video_size / 40)` including
  measurable directory/lock allocation and filesystem alignment. No extra slots,
  payload files or on-disk resume state may be created.
- `PieceStore` serializes mappings, disk reads/writes, verification and pins with
  its mutex. Only verified pieces can be leased. A lease pins every intersecting
  piece and owns the store, so it survives session shutdown.
- Controller operations use a separate mutex. Lock order is source -> controller
  -> store; the cache worker releases controller locks before publishing source
  status. Callbacks acquire leases without taking the controller mutex.
- Live torrent availability must never outlive the corresponding stored piece.
  Removing resident mappings requires the existing quiescent rebuild boundary.
- Desired window, verified availability and physically occupied slots are distinct.
  Partial writes and container metadata occupy the same bounded slots.
- VLC callbacks must continue to wait/cancel through the source's condition
  variable; rebuilds and priority changes remain on its worker.

Affected boundaries: `PieceStore::{needsEviction,reconcile,acquire}`,
`TorrentCacheSession::{schedule,rebuild,safeParams,updateReadRequest,diagnostics}`,
`DynamicSegmentCache::{window,updatePlaybackPosition}` and
`TorrentVideoSource::{readVerified,run,status}`. The disk backend and source are
also the measurement points needed to distinguish I/O from rebuild and peer wait.

The existing regression suite covers disk allocation and pins, adaptive window
geometry, v1/v2/hybrid reconstruction, cached/uncached seeks, failed hashes,
network starvation, local-peer media decoding and full CLI playback. Existing
tests will remain intact; additional cases must exercise reservation, batch
reclamation and oscillation.

## Baseline measurement

The unchanged Release build passed 9/9 CTest checks before production changes.
Instrumentation-only cache/stream/metadata regressions then passed 3/3. Raw
three-run measurements are in `performance/before.csv`; the instrumented original
sources and executable were preserved in the ignored `build-cache-baseline/`.

The sequential workload reads 2 MiB at 512 KiB/s from a deterministic 32 MiB
fixture, with a hot reread every eight iterations. The seek workload executes
128 reads in eight forward/backward regions. Both use an unaligned selected
file, real loopback libtorrent peers and the real custom disk backend. A third
workload decodes the generated four-second front-moov MP4 with headless LibVLC.
All returned synthetic bytes are checked against the seed input. The seed's
source fixture is separate from the measured client cache.

Sequential runs produced 111 rebuilds each; seeking produced 24–25; VLC produced
96–98. Every sampled free-slot count was zero. Average rebuilds took 0.588–0.899
ms, with maxima of 1.008–1.970 ms. Rebuild timing ends after re-add, priorities
and resume; asynchronous peer reconnection/download/verification appears in the
read-wait metric instead. These are local control-operation measurements, not
WAN performance estimates. None of these baseline runs had a read wait >=50 ms or
an observed post-start LibVLC `Buffering` state.

Counters classify each source read once as an immediate cache hit or a miss
that sleeps for verified bytes. Wait time includes scheduling, rebuild, peer and
verification delay; a completed/cancelled wait >=50 ms is a byte-consumer stall.
This is distinct from decoded-player buffering, whose count/duration is sampled
from LibVLC states after playback first starts. Backend work measures synchronous
disk jobs including hashing; lease-read time measures verified disk reads. These
timings are not additive exclusive slices: backend activity can overlap a source
wait, and quiescent slot zeroing is included in rebuild duration. Measurements
exclude initial cache allocation; VLC measurements include probing/prebuffering.
Traces record free slots,
window changes, rebuild counts and waiting reads over time.

## Reserved-slot and batch design

For `N` physical slots, default reservation `R` is zero below four slots and
`ceil(N/8)` otherwise, capped to leave at least two working slots. An optional
explicit reservation can override this, within the same bound. Reservation never
changes `CacheFile` size: it is an admission policy over already budgeted slots.
Automatic reservation is disabled when whole-cache coverage at the current rate
falls below one second; it re-enables only above 1.25 seconds. The initial target
uses the one-second threshold. This hysteresis avoids sacrificing critical
forward buffer in small/high-bitrate caches. Explicit reservations override it.
The adaptive logical window uses at most `N-R` pieces, maintaining its existing
approximately one-third-behind/two-thirds-ahead geometry. Metadata is included in
that union. Small caches retain a zero-reserve fallback.

`PieceStore` will expose a coherent inventory of occupied/free slots, reserved
free slots and protected/pinned slots. New `admit(required, prefetch)` admits only
into unused slots. Required pieces (current playback segment and exact urgent
read) may consume the reserve; speculative pieces stop when only `R` free slots
remain. This never discards a piece known to a live torrent. A protected set and
read pins prevent reclamation; overlapping target pieces are retained.

The session builds one admission plan, ordered urgent read -> playback -> forward
prefetch -> backward retention. Small speculative deficits are queued. When at
least `max(2,R)` expired pieces can be reclaimed and prefetch is pending, one
rebuild removes **all** eligible pieces outside the new window, restores reserve
headroom and admits the grouped requests. A missing required piece triggers that
boundary immediately if any eligible victim exists. Pins can defer admission,
but cannot be evicted or cause repeated empty rebuilds. New availability is built
only from actual resident verified pieces after reclamation, as before.

Window stability will wrap the original network-dependent size calculation.
Resizing needs a change of at least `max(2, old_segment_count/8)` segments; rolling
movement needs roughly one twelfth of the horizon. Small positional oscillations
therefore stay within the current window. Capacity/urgent constraints and leaving
the current window bypass hysteresis; explicit seeks reposition immediately.
The maximum positional drift is documented and tested against the intended ratio.
The actual roll occurs after drift exceeds `floor(segment_count/12)` segments;
the resulting behind-position deviation stays within that allowance away from
file edges. `seekForDemux()` distinguishes container-internal movement from user
seeks, so demux oscillation cannot accidentally defeat the latch. A playback/seek
epoch forces the next meaningful demux seek to reposition immediately.

Implementation sequence: store inventory/admission/reclamation with dedicated
tests; session batch planning/reconstruction; then stateful window stability and
explicit seek propagation. Source callbacks continue using pinned reads while the
control worker coordinates the torrent. Metrics remain enabled for comparison.

The original streaming test assumed a released pin was reclaimed on the next
pump. Batching deliberately retains such a piece until space is needed. The test
now adds a second distant cold window before its existing eviction/miss and peer
re-download assertions; those assertions and v1/v2/hybrid hash checks remain.

## Preventing notification delay

The first measured candidate reduced sequential rebuilds but increased VLC read
waits. Its raw data remains in `performance/after-initial.csv`. A coverage-aware
reserve fixed the loss of short-video forward buffering. Intermediate data in
`performance/after-before-notifications.csv` then exposed another bottleneck:
verified bytes could wait for the next 20 ms controller pump before waking input.

The verified-piece plugin now notifies the source immediately after marking the
piece readable. A shared notification object owns the reader mutex/CV, so it
outlives callbacks even if source construction or teardown fails. Verification
notification synchronizes with the predicate-check/wait handoff. The worker uses
a nonblocking libtorrent pump and a CV wait: new blocked demands and genuine seeks
wake it immediately, while cached sequential movement remains coalesced at 20 ms.
Neither downloading nor rebuilding runs in the read callback.

Stop first wakes/cancels readers and joins the worker. It moves the session out
under the reader mutex, then destroys it **without** holding that mutex. Libtorrent
can therefore finish a verification callback while its network thread joins.

## Concrete example

Assume eight slots, one-piece logical segments and enough bitrate coverage to
reserve one slot. At piece 4 the target is `[2,3,4,5,6,7,8]`: two behind, the
current piece, four ahead, and one unused urgent slot.

1. At piece 5 the target becomes `[3,4,5,6,7,8,9]`. Piece 9 is speculative;
   admitting it would consume the reserve. Piece 2 is the only expired piece,
   so 9 waits. Playback data is already resident.
2. At piece 6 the target becomes `[4,5,6,7,8,9,10]`. Two expired pieces, 2 and 3,
   meet the batch threshold. One safe rebuild removes both, keeps verified 4–8,
   admits 9 and 10, and restores one free slot.
3. An urgent metadata/read request can borrow that free slot immediately. It
   consumes the same physical budget. The logical window contracts if necessary
   to include distant metadata. No second file or staging buffer is allocated.
4. If an expired piece is pinned or explicitly protected, it survives. The plan
   uses other eligible pieces or defers admission. Availability after re-add is
   reconstructed from the actual remaining mappings, never the discarded state.

A rebuild is still needed for a required cold piece when no suitable free slot
exists, or when queued prefetch and enough expired pieces justify a batch. A
distant seek often requires one. A cache hit or admission into a free slot does
not. Pins can reduce effective capacity and cause buffering; tiny capacities can
require frequent immediate rebuilds. Pin eligibility is rechecked at reclamation.
Synchronous slot zeroing and bounded disk jobs still share the store mutex, so
slow disks can briefly delay readers; this change does not add an asynchronous
disk queue or remove libtorrent reconnect cost.

## Configuration and diagnostics

- `CacheStreamOptions::reserved_slots`: unset for automatic capacity/coverage
  adaptation; zero disables reservation; an explicit value must leave at least
  two working slots (one-slot caches support only zero). This C++ option does not
  change the disk budget. Large explicit reservations can leave insufficient
  working space for container metadata; admission then reports the limitation.
- `CacheStreamOptions::on_piece_verified`: optional fast notification on the
  network thread, after verified storage publication. It must not throw or call
  libtorrent control APIs. `TorrentVideoSource` installs its own synchronized
  notifier and preserves an external callback.
- Batch threshold: `max(2, R)` eligible expired pieces for speculative work;
  required misses bypass it. Resize threshold: `max(2, old_segment_count/8)`.
  Rolling drift allowance: `floor(segment_count/12)`.
- `CacheDiagnostics::performance` exposes rebuild timing, piece churn, window
  movements, slot inventory and backend work. `VideoSourceStatus::performance`
  exposes once-per-read hits/misses, waits, >=50 ms stalls and lease-read time.
  CLI status includes free/reserve slots, rebuilds and verified read wait.

## Reproducing the comparison

Use a disk-backed working directory, the same installed libraries and Release
builds. Run performance jobs separately from compilers/sanitizers. The baseline
instrumentation patch recreates the measured original source snapshot exactly;
this was checked by comparing its files with the saved snapshot.

```bash
mkdir -p build-cache-baseline-repro/source
git archive 6a3505e | tar -x -C build-cache-baseline-repro/source
patch -d build-cache-baseline-repro/source -p1 < docs/performance/baseline-instrumentation.patch
cmake -S build-cache-baseline-repro/source -B build-cache-baseline-repro/build -DCMAKE_BUILD_TYPE=Release
cmake --build build-cache-baseline-repro/build --target cache_benchmark -j2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target cache_benchmark -j2
cmake -DFFMPEG=/usr/bin/ffmpeg -DDESTINATION="$PWD/build/optimization-media" -P test/generate_media.cmake
./build-cache-baseline-repro/build/cache_benchmark --vlc build/optimization-media/front.mp4 --trace build-cache-baseline-repro/traces --repetitions 3 > build-cache-baseline-repro/before.csv
./build/cache_benchmark --vlc build/optimization-media/front.mp4 --trace build-cache-optimized/traces --repetitions 3 > build-cache-optimized/after.csv
python3 tools/compare_cache_benchmarks.py build-cache-baseline-repro/before.csv build-cache-optimized/after.csv --output build-cache-optimized/comparison.md
cmake -DDESTINATION="$PWD/build/optimization-media" -DCLEANUP=ON -P test/generate_media.cmake
```

The synthetic inputs are generated and removed by the benchmark. `--vlc` is
optional; omitting it runs only the byte workloads. CSV traces record sampled
free slots over time. All source bytes, client allocation and cleanup are checked.

## Results, validation and limits

[The measured tables](cache-performance.md) use three-run medians, except maximum
rebuild duration. Sequential rebuilds fell 111 -> 14 (87.4%), seeking 25 -> 7
(72.0%), and short-clip VLC 97 -> 91 (6.2%). Median verified-data wait fell in all
three workloads: 4.181 -> 1.002 ms, 98.256 -> 31.393 ms and 501.417 -> 209.668 ms.
Actual managed allocations were unchanged and below budget. No >=50 ms consumer
stall or post-start VLC buffering state occurred in either final workload set;
there is consequently no measured stall-count reduction to claim.

Larger batches cost more per individual rebuild. Backend-work timing increased,
and VLC's immediate-hit rate fell from 96.448% to 93.716%: earlier wakeups let
consumers reach the next still-downloading piece sooner, while total waiting
decreased. This is not a general disk-I/O speedup. Timings are local, small-sample
observations; the strong sequential count reduction should not be extrapolated
to all media. The four-second VLC fixture has sub-second cache coverage, so its
automatic reserve is disabled. Long-video reserve behavior is exercised by the
synthetic sequential/seek workloads, not a long-duration decoded WAN stream.

Executed final checks: 12/12 Release CTest checks; 8/8 ASan/UBSan non-playback checks
with leak detection enabled; 4/4 playback/fixture checks with `detect_leaks=0`;
3/3 ThreadSanitizer storage/concurrent reconstruction checks. The previous
standalone-reproduced LibVLC/Fontconfig LeakSanitizer report remains separate and
is not called leak-clean. No new invalid-access, UB or race report was observed.
GUI presentation, audible output, hardware decoding and public/WAN swarms remain
unverified. Rebuilds still reconnect peers; strict capacity cannot guarantee
uninterrupted playback under slow peers, missing indexes or insufficient slots.

## Changed files

| Files | Purpose |
|---|---|
| `src/piece_store.hpp`, `src/piece_store.cpp` | Reservation inventory, free-only admission, protected pieces and quiescent batch reclamation |
| `src/dynamic_segment_cache.hpp`, `src/dynamic_segment_cache.cpp` | Original sizing policy plus rolling/resize hysteresis and capacity/seek resets |
| `src/torrent_cache_session.hpp`, `src/torrent_cache_session.cpp` | Admission plans, batch rebuild gate, bitrate-adaptive reserve, exact priority restoration and verification notification |
| `src/torrent_video_source.hpp`, `src/torrent_video_source.cpp` | Immediate verification/urgent wakeups, coalesced worker, shared signal ownership, seek classification, cancellation and metrics |
| `src/video_byte_source.hpp`, `src/vlc_player.cpp` | Distinguish container byte seeks from explicit repositioning |
| `src/cache_geometry.hpp` | Typed sequential/seek movement |
| `src/cache_metrics.hpp`, `src/cache_disk_io.hpp`, `src/cache_disk_io.cpp` | Read, rebuild, slot and backend timing/counters |
| `src/vlc_playback.cpp` | Live reservation/rebuild/read-wait diagnostics |
| `test/reserved_slot_tests.cpp` | Small capacities, reservation debt, protected/pinned reclamation and verification tests |
| `test/batch_eviction_tests.cpp` | Real-peer batch reconstruction, concurrent reads, re-download consistency, coverage hysteresis and seek/notification regressions |
| `test/rolling_window_tests.cpp` | Drift bounds, resizing, urgency, oscillation and explicit seeks |
| `test/cache_benchmark.cpp`, `test/synthetic_media.hpp` | Reproducible checked sequential/seek/VLC workloads and CSV traces |
| `test/local_video_torrent.hpp` | Configurable source creation and metadata access for tests |
| `test/playback_tests.cpp`, `test/streaming_tests.cpp` | Read-metric assertions, real VLC outage recovery, and batch-aware reclamation pressure while retaining integrity assertions |
| `CMakeLists.txt` | New regression and benchmark targets |
| `tools/compare_cache_benchmarks.py` | Reproducible comparison-table generation |
| `docs/performance/before.csv`, `docs/performance/after.csv` | Raw three-run baseline/final measurements |
| `docs/performance/after-initial.csv`, `docs/performance/after-before-notifications.csv` | Rejected candidate measurements that prompted revision |
| `docs/performance/baseline-instrumentation.patch` | Exact original instrumentation for reproduction |
| `docs/cache-optimization.md`, `docs/cache-performance.md`, `README.md`, `Guide.md` | Investigation, design, configuration, examples, validation and results |
