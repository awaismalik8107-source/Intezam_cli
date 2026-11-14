# Playback and cache architecture

## Inspected repository and reused code

The [upstream repository](https://github.com/awaismalik8107-source/Intezam_cli)
began with a single `main.cpp`, documentation and historical compiled binaries.
Its original code created `./test`, dereferenced metadata before retrieval had
completed and entered an unlimited download/status loop. The preceding local
milestones replaced that flow with tested metadata-only retrieval, video
detection, validated selection, the bounded sliding disk cache and CMake/CTest
targets. The current milestone adds LibVLC playback on those local modules.

This implementation reuses `torrent_metadata.*`, `video_files.*`,
`video_selection.*` and the cache geometry, allocation, storage and disk backend.
Metadata retrieval still uses upload-only mode,
`default_dont_download` and disabled disk I/O. A separate session starts only
after a video is selected and cache allocation succeeds. `--metadata-only`
retains the previous no-payload behavior. Historical binary artifacts are not
overwritten. LibVLC 3.x is a required linked dependency; it is initialized only
for playback, after selection. The older simulation remains opt-in.

## Modules

| Module | Responsibility |
|---|---|
| `cache_geometry.*` | Validated selected-file geometry and torrent-global piece mapping |
| `cache_file.*` | Filesystem-aligned preallocation, budget, pwrite/pread bounds, lock and cleanup |
| `piece_store.*` | Piece-to-slot mapping, partial write coverage, verification and pinned read leases |
| `dynamic_segment_cache.*` | Adaptive horizon and 1/3-behind, 2/3-ahead segment positioning |
| `cache_disk_io.*` | libtorrent 2.1 `disk_interface`, disk reads/writes and SHA-1/SHA-256 hash callbacks |
| `torrent_cache_session.*` | Safe torrent lifecycle, piece priorities/deadlines, eviction barrier and diagnostics |
| `video_byte_source.hpp` | Seekable byte source contract, metadata reads and explicit cancellation |
| `container_probe.*` | Bounded ISO BMFF atom walking and EBML/AVI/MPEG signature probing |
| `torrent_video_source.*` | Worker, urgent range scheduling, verified callback reads, prebuffer and scan guard |
| `vlc_player.*` | LibVLC lifecycle, custom callbacks, decode statistics, pause and time seeks |
| `vlc_playback.*` | Startup, terminal controls, signals, status and orderly shutdown |
| `cache_simulation.*` | Timed simulated playback, interactive seeks/rate changes and signal cleanup |
| `main.cpp` | Magnet/detection/selection flow, playback options and handoff |

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
then reduced until its exact intersecting piece set, unioned with the current
urgent read's pieces, fits. A distant container metadata read therefore consumes
existing slots and temporarily reduces the playback horizon. If even the current
segment plus required metadata cannot fit, the limitation is reported instead
of exceeding the budget.
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

The controller gives nonzero priorities only to admitted target/urgent pieces.
Deadlines start with the blocked read, then playback, forward prefetch and backward
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

Default streaming settings use outgoing TCP, incoming TCP/uTP and a one-second
minimum reconnect backoff. Libtorrent optimistically retries uTP for newly added
peers, including after each eviction generation; repeated UDP-to-TCP fallback
otherwise stalls playback on a TCP-only seeder. Metadata retrieval retains its
existing transport settings. Embedders can explicitly enable outgoing uTP via
`CacheStreamOptions::settings`, accepting that cost.

## Cache-to-VLC interface and threads

`TorrentVideoSource` owns one `TorrentCacheSession` and a control worker. VLC's
open callback creates a cursor containing its selected-file offset and source
ownership. Seek validates `0 <= offset <= video_size`, changes that cursor and
supersedes old queued reads. Read clamps each request to 16 KiB, the selected-file
end and one torrent-piece boundary, including unaligned selected files.

A callback queues its exact range under the source mutex. It first attempts a
verified `ReadLease`; otherwise it sleeps on a condition variable. The worker
updates the playback anchor and urgent range, pumps libtorrent and publishes
diagnostics every 20 ms. The callback never pumps the session or requests a
rebuild itself. A lease remains pinned through the disk read and copy to VLC.
Only EOF (or a zero-length request) returns zero. Timeout, cancellation and storage
errors have separate exception paths; all C callbacks catch exceptions and return
VLC's error sentinel rather than unwinding through C.

Source mutex order is source -> cache controller -> piece store. The worker takes
source snapshots before cache calls and releases cache locks before publishing
status. LibVLC control calls run on the CLI thread, with separate control and error
mutexes; event/log/video callbacks never call player control APIs. The headless
video callback uses a 32-byte-aligned fixed frame buffer and counts displayed
frames; audio uses VLC's dummy output while still decoding audio packets.

Stop first suspends new input reads, advances a cancellation epoch and wakes
pending callbacks, then calls LibVLC 3's synchronous stop. After callbacks finish,
the cache worker is cancelled/joined and its session is destroyed. This ordering
also handles stopping during probing or an active network wait. SIGINT/SIGTERM
only set a signal flag; normal control flow performs teardown. Terminal mode is
restored by RAII. A closed stdin disables controls while media continues to EOF.

## Containers, startup and seeking

Container detection reads real bytes. MP4/MOV/M4V use bounded top-level atom-header
walking: `mdat` payload is skipped by offset and the `moov` range is recorded even
after media data. Scanning stops when `moov` is found, avoiding a walk through
every media fragment. Extended atom sizes and malformed/truncated bounds are checked.
EBML header/DocType distinguishes Matroska and WebM; RIFF AVI, TS/M2TS and MPEG
program streams have signature checks. Unknown signatures receive VLC probing.
There are no custom codecs or timestamp indexes.

Known ISO BMFF metadata reads preserve the playback byte anchor. VLC can request
any other header/cue/index range through its seek/read callbacks, prioritizing
the missing piece without allocating a separate metadata file. All such data
shares the same slots and verification path. Parsed demux metadata lives in VLC's
memory. Matroska cluster/directory preloading and AVI automatic index repair are
disabled; missing indexes do not authorize an unlimited scan.

Before playback, a bounded verified prefix is buffered: up to the smaller of
one third of data capacity and the initial 0.2-second rate estimate, with a 16 KiB
minimum where capacity allows. VLC then performs its normal demux buffering.
VLC's prefetch filter is limited to 64 KiB RAM, 16 KiB reads and a one-byte seek
threshold. Setting this threshold to zero makes VLC 3 loop on an empty prefetch
buffer; the one-byte value permits discontinuous metadata requests to seek.

During startup and user seeks, a merged range map counts the unique selected-file
footprint of requested whole torrent pieces, including for tiny header requests.
Its allowance is `min(90% of video_size, max(8 MiB, min(16 MiB, capacity * 8)))`.
Repeated reads do not consume the allowance again. Exceeding it fails explicitly
instead of fetching the complete file to reconstruct an index. Normal decoded
playback disables the startup guard. The disk bound does not cover VLC decoder
buffers or parsed in-memory metadata.

Time, duration and seekability come from LibVLC. Ordinary time seeks retain the
demuxer via `libvlc_media_player_set_time()`. If a seek remains blocked after
750 ms, the controller cancels the old callback before stopping and reopening
with `start-time`; pause state is preserved. VLC determines the byte offsets and
keyframe dependencies. A cache hit serves existing verified bytes immediately;
an uncached request updates priorities and waits for verification. Parsed indexes
and keyframes can still require additional bytes before a frame is displayed.

The dynamic window follows demux byte consumption, which can lead displayed time
because VLC buffers packets. Duration supplies an approximate average bitrate;
it does not model arbitrary variable bitrate streams precisely. Network EWMA,
verified forward availability and capacity continue to determine the horizon.
Seeking and codec support depend on valid media and installed plugins. Expensive
startup metadata or too few slots can fail under the strict budget.

## Validation and limitations

The real-media tests generate eight fixtures and play them over a loopback torrent
through the custom cache backend and LibVLC memory-video/dummy-audio outputs.
Executed checks include front/tail MP4, MKV, WebM, MOV, M4V, AVI, MPEG-TS, actual
decoded video/audio, pause/resume, time seeks, cached byte reuse, uncached reads,
peer outage/recovery, throttling, blocked-read cancellation, EOF and cleanup.
A subprocess test exercises the complete CLI with its default network settings,
from magnet input and selection through tail-metadata playback to EOF.
Each client cache's actual allocated file/directory blocks are measured against
`floor(video_size / 40)`, separately from the seeder's fixture files. The existing
v1/v2/hybrid cache and metadata-only tests remain active. See README for commands.

Desktop image presentation, audible output, hardware decoding, arbitrary damaged
or encrypted media, public swarms and sustained WAN playback still require manual
or broader compatibility testing. Local decode success is not a guarantee of
uninterrupted playback on every torrent.

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
- [LibVLC 3 custom media callbacks](https://videolan.videolan.me/vlc-3.0/group__libvlc__media.html)
- [LibVLC 3 player controls and video callbacks](https://videolan.videolan.me/vlc-3.0/group__libvlc__media__player.html)
- [VLC 3 prefetch implementation and option units](https://code.videolan.org/videolan/vlc/-/blob/3.0.x/modules/stream_filter/prefetch.c)
