# Forward refill and smooth playback follow-up

The Qt/adaptive-budget version passed its existing 15 tests, but a short
forward window and conservative batch admissions could still drain the buffer.
This follow-up retains bounded disk allocation, verified pinned reads and the
supported torrent reconstruction barrier. The user approved larger safety
buffers after being told that startup could take longer.

## What was wrong

1. `shouldRebuild()` required at least two cold slots for speculative refill.
   Four-piece caches sometimes exposed only one cold slot, so the next forward
   piece stayed deferred until the reader actually missed it. A paused reader
   could also wait for a resume target whose remaining pieces were not admitted.
   The new regression reproduced this as a forward-refill timeout.
2. Every meaningful scheduling update cleared all piece deadlines and re-added
   them. Libtorrent's first time-critical request can cancel non-critical work;
   repeatedly rebuilding that set needlessly disturbed the request queue. Cached
   reads appearing/disappearing also republished unchanged priorities.
3. The Qt status treated any blocked prefetch callback as a playback stall, even
   while VLC continued rendering queued video. The displayed ahead value counted
   verified disk bytes beyond the input reader, not VLC's decoder queues.
4. The displayed download rate reused the smoothed scheduling estimate, which
   intentionally held its value while idle. This could display an old high speed
   after prefetching had stopped.

The [libtorrent streaming description](https://www.libtorrent.org/streaming.html)
and [deadline API](https://www.libtorrent.org/reference-Torrent_Handle.html)
explain time-critical requests. The public
[source implementation](https://github.com/arvidn/libtorrent/blob/RC_2_0/src/torrent.cpp)
confirms cancellation when a new time-critical set starts. Normal slot eviction
still requires the existing safe pause/snapshot/remove/re-add boundary; this
change does not claim to eliminate reconnections.

## Changes

- Refill remains batched when sufficient cold slots are available. If forward
  data is deferred and verified runway drops below two piece durations (minimum
  0.5 seconds), even one eligible cold slot can trigger an early refill. Required
  misses retain immediate handling. Protected data and active pins remain immune
  to reclamation; no empty rebuild is performed when there are no eligible slots.
- Scheduling keeps per-piece priorities and absolute deadlines. It publishes
  priority differences, adds new deadlines, tightens materially earlier deadlines
  and removes only completed/obsolete deadlines. Explicit user seeks retime
  retained pending pieces for the new playback epoch without clearing the set. A cached read does not clear
  unrelated forward work. Time-critical pieces keep the priority expected by
  libtorrent's streaming scheduler.
- Startup/resume goals are **6 seconds** for throughput at least 1.5 times
  estimated consumption, **8 seconds** at parity, and **12 seconds** for slow or
  unknown throughput. The low watermark is `min(2 seconds, high / 3)`. Existing
  250 ms debounce and manual-pause precedence remain.
- Streaming uses a throughput-dependent total horizon of roughly 24–45 seconds,
  giving up to about **30 seconds ahead** and approximately one third behind.
  At the beginning, unused backward coverage does not inflate the ahead target.
  Whole-piece working slots, urgent metadata and rolling hysteresis still bound
  the plan. Targets clip to the actual forward window, one third of data capacity
  and remaining bytes. A tiny cache may therefore hold less than six seconds.
- Planned prefix prebuffering is distinguished from decoder probing. The prefix
  remains bounded, verified and chunked into at most 16 KiB reads; it does not
  consume the separate unique-range index-scan allowance. Actual decoder/index
  probing remains guarded afterward, including with large torrent pieces.
- The UI calls the ahead value an approximate **verified ahead** estimate, with
  a tooltip explaining its byte-reader basis. Brief read waits do not label a
  playing video as buffering. The download readout uses a current half-second
  sample; the scheduler retains a separate smoothed throughput estimate.

The fixed session budget and its 25% automatic ceiling are unchanged. Filling
the bounded forward window can naturally make downloads idle until playback
makes room. These changes aim to preserve enough runway, rather than promise a
constant download rate or uninterrupted playback when peers cannot keep up.

C++ embedders can set `CacheStreamOptions::forward_buffer_target_seconds` (default
12). Zero retains the shorter legacy planning horizon. The policy class's
optional constructor argument defaults to zero to preserve existing direct
callers. Startup/resume watermarks use the smooth profile and always clip to
reachable capacity. New counters in `CachePerformance` expose priority publishes,
deadline updates/resets and proactive refill rebuilds; the benchmark prints them.

## Verification

All **16 CTest checks passed** in 40.68 seconds, including the original playback,
seek, peer-recovery, pin, hash-verification and storage tests. Added coverage:

- A four-piece cache fills its forward horizon without first receiving an urgent
  read, reproducing the previous timeout and verifying physical allocation.
- Repeated cached read demands leave unchanged priorities/deadlines alone;
  explicit seeks retime pending work without inheriting expired deadlines.
- A 256 KiB/s sequential reader continues against a 1 MiB/s throttled seed with a
  **400 ms peer pause**. One measured run's maximum read wait was **0.205 ms**;
  the regression allows up to 500 ms to tolerate host scheduling. Every returned
  byte is checked, including reads crossing unaligned piece boundaries.
- Idle download rate returns to zero while the scheduling estimate remains valid.
- A large-piece fixture successfully prebuffers beyond the decoder scan footprint,
  while subsequent excessive decoder metadata probing is still rejected.
- The smooth policy provides approximately 30 seconds ahead when capacity allows,
  adapts to throughput, and fits a four-piece cache.
- Qt readouts distinguish active rendering from a prefetch wait and use current
  transfer speed instead of a stale link estimate.

Address/undefined-behavior sanitizer runs passed the ten non-UI/non-playback
checks with leak checking enabled and the six UI/playback/fixture checks with
`ASAN_OPTIONS=detect_leaks=0`. The earlier external LibVLC/Fontconfig leak-check
limitation remains documented in the README.

## Before and after measurements

Three repetitions used the same deterministic sequential/seek workloads and the
same four-second headless H.264/AAC loopback video. The pre-change benchmark
executable was preserved before production changes. Values below are medians;
raw data: [before](performance/smooth-before.csv), [after](performance/smooth-after.csv).

| Metric | Sequential before → after | Seeking before → after | VLC before → after |
|---|---:|---:|---:|
| Rebuild count | 14 → 14 | 7 → 7 | 116 → 91 |
| Mean rebuild duration, ms | 1.018 → 0.985 | 1.584 → 1.455 | 0.673 → 0.682 |
| Maximum rebuild duration, ms | 1.443 → 1.716 | 2.394 → 2.073 | 1.754 → 1.477 |
| Window changes | 29 → 29 | 26 → 26 | 117 → 92 |
| Read hit percentage | 99.632 → 99.632 | 95.455 → 95.455 | 90.347 → 93.615 |
| Verified-data wait, ms | 0.936 → 0.972 | 30.056 → 36.755 | 229.379 → 243.784 |
| Maximum read wait, ms | 0.855 → 0.940 | 4.991 → 6.558 | 41.832 → 42.808 |
| Read stalls at least 50 ms | 0 → 0 | 0 → 0 | 0 → 0 |

VLC used about **22% fewer reconstructions** in this local workload, but its
median verified-data wait increased from 229.379 to 243.784 ms (about **6%**).
Seeking wait and some timing maxima also increased. Neither version recorded a
read stall of at least 50 ms or a post-start VLC buffering episode. These short,
host-dependent measurements support the reduction in reconstruction frequency;
they do not establish an overall latency improvement. The short fixture's cache
holds only a fraction of a second, so it cannot exercise the larger forward
cushion demonstrated by the native main-video run below. Managed allocation was
unchanged: 819,200 / 838,860 bytes for byte workloads and 327,680 / 347,812 bytes
for VLC.


## Short native public-swarm check

The user's previously supplied magnet was tested with the main video in the
native Qt/X11 window, stopping after a bounded run. Playback advanced to **25.376
seconds**, decoding **1,237 video and 4,875 audio frames**. Sampled verified ahead
peaked at **29.34 seconds**. No automatic buffer pause or torrent rebuild occurred
during this short run. Peak physical allocation was **144,703,488 bytes** under
the **146,745,052-byte** budget, and cleanup completed. Payload received by the
last snapshot was about **71.35 MB**, rather than the full 5.87 GB video.
This verifies that the new horizon can fill and play through a real swarm; it
is not a full-film or sustained-network guarantee.

## Affected files

`src/torrent_cache_session.*` implements early refill, incremental scheduling and
current transfer sampling; `src/cache_metrics.hpp` exposes counters.
`src/dynamic_segment_cache.*` and `src/stream_policy.cpp` supply the smooth profile.
`src/torrent_video_source.*` separates planned prebuffer reads from decoder probes.
`src/player_window.cpp` and `src/vlc_playback.cpp` correct playback/rate readouts.
`test/forward_refill_tests.cpp` and its CMake target cover refill and network
continuity; the local torrent/synthetic helpers support larger pieces and fixture
sizes. Policy, rolling-window, Qt and benchmark tests cover the remaining changes.
README, Guide and performance CSVs document behavior and evidence.
