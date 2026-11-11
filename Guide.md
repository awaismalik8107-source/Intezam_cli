# Cache architecture and implementation notes

## Inspected repository and reused code

The [upstream repository](https://github.com/awaismalik8107-source/Intezam_cli)
began with a single `main.cpp`, documentation and historical compiled binaries.
Its original code created `./test`, dereferenced metadata before retrieval had
completed and entered an unlimited download/status loop. The preceding local
milestone already replaced that flow with tested metadata-only retrieval, video
detection, validated selection and CMake/CTest targets.

This implementation reuses `torrent_metadata.*`, `video_files.*` and
`video_selection.*`. Metadata retrieval still uses upload-only mode,
`default_dont_download` and disabled disk I/O. A separate session starts only
after a video is selected and cache allocation succeeds. `--metadata-only`
retains the previous no-payload behavior. Existing uncommitted work is retained;
the historical binary artifacts are not overwritten.

## Modules

| Module | Responsibility |
|---|---|
| `cache_geometry.*` | Validated selected-file geometry and torrent-global piece mapping |
| `cache_file.*` | Filesystem-aligned preallocation, budget, pwrite/pread bounds, lock and cleanup |
| `piece_store.*` | Piece-to-slot mapping, partial write coverage, verification and pinned read leases |
| `dynamic_segment_cache.*` | Adaptive horizon and 1/3-behind, 2/3-ahead segment positioning |
| `cache_disk_io.*` | libtorrent 2.1 `disk_interface`, disk reads/writes and SHA-1/SHA-256 hash callbacks |
| `torrent_cache_session.*` | Safe torrent lifecycle, piece priorities/deadlines, eviction barrier and diagnostics |
| `cache_simulation.*` | Timed simulated playback, interactive seeks/rate changes and signal cleanup |
| `main.cpp` | Existing magnet/detection/selection flow plus cache options and handoff |

## Budget and piece mapping

For video size `V`, maximum budget `B = floor(V / 40)`. With filesystem unit `A`
and piece size `P`, each slot has stride `ceil(P / A) * A`. Available slot count
is `floor((B - measured_directory_and_lock_blocks - A) / stride)`. All slots,
including incomplete pieces and pinned data outside the target window, fit in
this one preallocated file. There is no separate payload staging file.
Known tmpfs/ramfs roots are rejected so the cache is not placed exclusively in RAM.

The physical allocation is checked using `st_blocks * 512`, including the cache
root directory's own blocks, the session directory, lock and data file. An extra
allocation unit is reserved. File size is fixed and every pwrite is bounds
checked. Writes are rejected on disk errors and the session unwinds. Statistics
do not claim to account for unobservable filesystem journal/inode/snapshot costs
or impose a global quota on other processes or cache sessions.

For selected-file range `[offset, offset + length)`, global range starts at
`video.torrent_offset + offset`. The first piece is `global_begin / P`; the last
is `(global_end - 1) / P`. Zero-length ranges contain no pieces. Full intersecting
pieces are stored and verified, including v1 bytes belonging to an adjacent
file; the playback lease exposes only the requested selected-file bytes. The
last torrent piece and v2 per-file final-piece length can differ from `P`.

## Adaptive window policy

Logical segment size is fixed per session at `max(16 KiB, P / 2)`, so segment
indexes never masquerade as piece indexes. For a throughput sample `x`, an EWMA
updates speed `s` and deviation `j` with weight 0.2. Zero-throughput samples are
included when pieces are pending; idle fully cached periods are not treated as
network failures. Measurements use actual payload byte deltas across re-adds.

For configured consumption `r`, a candidate horizon in seconds is:

```text
2 + 2 * min(4, s/r)
  + 2 * min(2, j / max(s, 0.1*r))
  + (contiguous_verified_buffer < 1 second ? 2 : 0)
```

Before the first sample, `s/r` uses 1 as an initialization estimate, not a claim
about actual throughput. The horizon becomes a segment count via
`ceil(seconds * r / segment_size)`. The count is clipped by available slots and
then reduced until its exact intersecting piece set fits. If even the current
segment cannot fit, the limitation is reported instead of exceeding the budget.
This policy is deliberately explicit and tunable; no constant four-segment
window or claim of universal optimal buffering is involved.

For current segment `c`, count `n`, total segments `t`:

```text
first = clamp(c - floor(n/3), 0, t - n)
last  = first + n - 1
```

This recalculates on playback movement and throughput/buffer updates, rather
than waiting for the end of a window. Pins may prevent full admission; missing
target pieces remain deferred and playback buffers if the requested bytes are
not verified. Window size is never equated with availability.

## Storage and verification states

`PieceStore` tracks reserved slots, merged written ranges, verification and pin
counts. New slots are zeroed before reuse, so implicit padding cannot expose old
bytes. Partial writes never make data readable. The custom disk backend hashes
on-disk bytes; libtorrent compares them with torrent hashes and calls
`torrent_plugin::on_piece_pass()` before the store exposes a verified piece.
Hash failures invalidate the piece and let libtorrent retry. V1 uses SHA-1;
v2/hybrid validation also supplies per-block SHA-256 hashes through
`async_hash()` / `async_hash2()` with v2 file-tail lengths.

The controller gives nonzero priorities only to admitted target pieces.
Deadlines start at playback, continue through forward prefetch, then backward
retention. Starting torrents are paused and in upload-only mode until these
priorities are installed. This prevents both payload races and an early
upload-only handshake from disconnecting the seed before scheduling is ready.

A read lease pins all intersecting verified pieces under the store mutex. It
reads with pread from the slot file and releases pins on destruction. Eviction
cannot reuse a pinned slot. Torrent/controller methods have their own control
mutex; disk I/O, verification, lease reads and eviction share the store mutex.
The lock order is controller then store; libtorrent callbacks never acquire the
controller lock. A lease's shared store ownership keeps the file alive during
concurrent session teardown.

## Eviction and public libtorrent APIs

Lowering priority does not delete bytes or clear libtorrent's `have` state.
There is no public single-piece "forget" function in the installed interface.
The implementation therefore uses a quiescent torrent-generation boundary when
slots must be reused:

1. Pause and wait for `torrent_paused_alert` so pending writes/hashes settle.
2. Request `save_resume_data(save_info_dict)` and move the returned parameters
   into memory; no resume file or `.torrent` file is written.
3. Remove the torrent and wait for the custom backend's storage holder to release.
4. Reconcile slots: retain overlap and pins, reuse only unneeded unpinned slots,
   and postpone admission if pins leave insufficient space.
5. Re-add the torrent paused/upload-only. Build `have_pieces` solely from actual
   resident verified pieces; restore complete retained 16 KiB blocks as
   `unfinished_pieces`. Retain peers, trackers and v2 merkle state from the snapshot.
6. Apply new priorities/deadlines, leave upload-only mode and resume connections.

Untouched verified overlap is never copied or re-downloaded. Opportunistic old
resident pieces remain valid for hits until space is needed. Incomplete blocks
that were not fully written cannot be claimed in resume state. Cached seeking
can read immediately; outside seeking reports a miss and waits for verification.
Re-adding costs connection churn and can cause rebuffering. This avoids internal
libtorrent headers or unsupported modifications to its piece picker, but a future
upstream availability-reset API could make eviction smoother.

## Remaining playback work

The simulation consumes byte ranges at a configured rate. It does not decode
media or derive timestamps/bitrate from a container. A future player should
translate its requests into selected-file ranges, hold leases during reads, and
provide observed playback consumption. Container headers/indexes near EOF may
need separate pinned requests; these would also consume slots. Decoder startup,
seeking and minimum buffer requirements can exceed the budget and must be
reported or stalled explicitly. No guarantee of codec compatibility or
uninterrupted VLC playback is made here.

The initial disk backend performs bounded synchronous operations on libtorrent's
network thread and posts callbacks to its `io_context`, which the API permits.
A bounded worker queue could improve responsiveness on slow disks. SIGKILL and
power loss cannot run destructors; advisory-lock-based recovery handles their
recognized stale cache directories on the next initialization.

## API references

- [libtorrent custom storage / disk_interface](https://libtorrent.org/reference-Custom_Storage.html)
- [Piece priorities, deadlines and resume data](https://libtorrent.org/reference-Torrent_Handle.html)
- [Torrent flags](https://github.com/arvidn/libtorrent/blob/v2.1.0/include/libtorrent/torrent_flags.hpp)
- [Resume parameter fields](https://github.com/arvidn/libtorrent/blob/v2.1.0/include/libtorrent/add_torrent_params.hpp)
