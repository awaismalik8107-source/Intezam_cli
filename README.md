# Intezam CLI

A C++17 Linux torrent video player powered by libtorrent and LibVLC. It retrieves
metadata, lists videos, and plays the selected video through custom media callbacks
reading **verified bytes from a bounded temporary disk cache**. It never creates
an ordinary full-size or sparse video file for VLC.

## Build on Arch Linux / CachyOS

Targets libtorrent-rasterbar **2.1.0+** and the **LibVLC 3.x** callback/lifecycle
API. Tested locally with libtorrent **2.1.2**, LibVLC **3.0.24**, and GCC 16.2.1.
LibVLC 4 has different lifecycle APIs and is rejected by this CMake configuration.

```bash
sudo pacman -S --needed base-devel pkgconf cmake libtorrent-rasterbar libvlc vlc vlc-plugins-all
pkg-config --modversion libtorrent-rasterbar libvlc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/intezam
```

`vlc-plugins-all` supplies the runtime demuxers, codecs and output modules. A
minimal installation can use the corresponding individual VLC plugin packages;
headers/libvlc alone do not guarantee codec availability. No FFmpeg development
library, HTTP framework, Qt application or external player process is required.
The [Arch plugin package](https://archlinux.org/packages/extra/x86_64/vlc-plugins-all/)
documents its runtime dependencies.

Use `build/intezam`; historical binaries in the repository root are unchanged.
CMake commands work in Bash and Fish. A direct Bash build is also supported:

```bash
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wpedantic main.cpp src/*.cpp \
    $(pkg-config --cflags --libs libtorrent-rasterbar libvlc) -pthread -o build/intezam
```

In Fish, replace the command substitution with
`(pkg-config --cflags --libs libtorrent-rasterbar libvlc | string split ' ')`.

## Playback

```bash
./build/intezam
./build/intezam --timeout 60 --bitrate 1048576
./build/intezam --headless
./build/intezam --metadata-only
./build/intezam --simulate
./build/intezam --simulate-seconds 30
```

Paste a magnet and choose a video by its displayed number. Original zero-based
torrent indexes, full paths, extensions and sizes are retained. Metadata retrieval
remains payload-free and writes no files. `--metadata-only` exits after selection.
Playback skips candidates whose 2.5% budget is below one torrent piece. If only
one candidate remains from a multi-video list, it is selected automatically using
its original number. This avoids selecting an undersized sample instead of the
main film. Other lists keep manual selection; filesystem/container capacity is
still checked when playback starts. Metadata-only mode keeps all candidates.
The default metadata timeout is 120 seconds (`--timeout`: 1–3600 seconds).

Default mode probes the actual container bytes, buffers a capacity-limited startup
range, then starts LibVLC. Terminal controls do not require Enter on a TTY:

| Key | Action |
|---|---|
| P | Pause / resume |
| F | Seek forward 10 seconds, bounded by known duration |
| B | Seek backward 10 seconds |
| S | Stop playback and remove the cache |
| Q | Quit and remove the cache |

SIGINT/SIGTERM also cancel reads and unwind the session. Terminal settings are
restored on exit. Closing stdin disables controls while playback continues to
media EOF; a piped magnet/selection can therefore play unattended. Status displays
VLC's time/duration, buffering, observed network speed, allocated cache bytes and
the desired/verified/downloading window state.

`--bitrate` supplies an initial bytes-per-second estimate (default 1 MiB/s).
Once VLC exposes duration, the cache uses file-size/duration as an approximate
average consumption rate. VBR and container overhead make this an estimate, not
a codec guarantee. Byte scheduling follows actual demux byte consumption; VLC
timestamps provide the displayed playback position.

`--headless` uses software decoding, a fixed 320×240 memory video output and a
dummy audio output. It is intended for automated validation, not watching a film.
`--simulate` preserves the previous disk-cache harness with `seek BYTE`,
`rate BYTES_PER_SECOND`, `pause`, `play`, `status`, and `q` commands.

## Containers and codecs

Extension filtering still supports `.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`, `.flv`,
`.wmv`, `.m4v`, `.mpeg`, `.mpg`, `.ts`, and `.m2ts`, ignoring case. Padding files,
symlinks and non-video extensions are excluded. Extensions identify candidates;
they do not establish container or codec compatibility.

- MP4/MOV/M4V: bounded top-level ISO BMFF atom-header reads locate `moov`, including
  after `mdat`. Payload boxes are skipped by byte offset and scanning stops once
  `moov` is found. VLC can then request the
  actual metadata in bounded chunks, including near EOF.
- MKV/WebM: EBML signatures and DocType identify the family. VLC's demuxers request
  headers, cues/indexes and media through the same seekable verified-byte interface.
- AVI, MPEG-TS/M2TS and MPEG program streams are signature-probed and delegated
  to VLC. Unknown signatures also receive normal VLC probing.

No decoder or timestamp-to-byte index is reimplemented. Supported codecs depend
on the installed VLC plugins. Automatic AVI index repair, Matroska directory
preloading and cluster preloading are disabled. Excessive startup/index scanning
is rejected instead of allowing an automatic whole-file scan.

## Verified reads and buffering

`TorrentVideoSource` owns a cache worker. Missing read ranges become urgent piece
requests; callback threads sleep on a condition variable while libtorrent runs
independently. Verification wakes readers immediately, and blocked demands wake
the controller without waiting for its normal 20 ms coalescing interval.
Reads are limited to 16 KiB and one torrent piece at a time. A pinned
read lease protects verified disk data while it is copied into VLC's buffer.
Incomplete or unverified pieces are never exposed. Zero means actual EOF; waiting,
timeout, cancellation and errors are separate paths.

Playback data keeps the dynamic 1/3-behind, 2/3-ahead policy. A container metadata
request outside that window reduces the playback window until its exact union of
pieces fits the existing slots. It does not allocate a second cache or move the
playback anchor to a known `moov` range. Other demux seek/read requests update the
byte-consumption position, while already verified overlapping data is reused.

Ordinary time seeks use `libvlc_media_player_set_time()` and retain parsed demux
metadata. If the seek does not progress within 750 ms, the controller suspends
old reads, stops the old input and reopens it with `start-time`. Suspension wakes
blocked callbacks before LibVLC 3's synchronous stop, avoiding a shutdown deadlock.
Seeking is available when VLC has a usable duration and seek support. A cache hit
does not guarantee an instant decoded frame if keyframe dependencies are missing.

VLC's own prefetch filter is explicitly bounded to **64 KiB in RAM**, with 16 KiB
reads and a one-byte seek threshold. Its default large read-ahead is not used.
During startup and user seeks, a unique-range guard charges the complete
selected-file footprint of requested torrent pieces. It permits at most 90% of the
file and an absolute capacity-derived 8–16 MiB allowance before playback progress.
Repeated reads of the same piece do not consume that allowance again. The guard
rejects unusually expensive probing/index reconstruction rather than fetching the
whole video. Decoder buffers and parsed metadata in VLC remain separate from
the managed on-disk video cache.

## Disk budget and cleanup

The disk cache architecture is retained: one preallocated slot file under
`$XDG_CACHE_HOME/intezam/session-XXXXXX/`, or `~/.cache/intezam/session-XXXXXX/`:

```text
stream.cache   # fixed size; whole-piece slots reused in place
session.lock   # empty advisory lock; mappings/resume state stay in memory
```

Maximum budget is `floor(video_size / 40)` bytes. Complete pieces, filesystem
alignment, session/root directory blocks, lock allocation and an allocation-unit
reserve are accounted for before requesting payload. Actual allocated blocks
(`st_blocks * 512`) are checked after preallocation and writes. Container metadata,
incomplete pieces and pinned readers consume the same slots. Writes cannot grow
the file. Insufficient capacity, ENOSPC and other storage failures are reported.
Known tmpfs/ramfs roots are rejected.

The bound covers managed cache file/directory allocation, not RAM, filesystem
journals, external snapshots or device-wide metadata. Concurrent sessions each
have their own budget. No normal full-size torrent payload, part file, sparse
video, on-disk resume file or external streaming staging file is created.

Normal exit, stop, EOF and handled failures remove the session directory.
Outstanding read leases retain their data until released. After SIGKILL/crash,
the next cache initialization removes recognized abandoned session directories
only after acquiring their lock; live sessions and unknown contents are preserved.
The empty shared root directory remains.

Libtorrent 2.1 has no public single-piece availability-reset operation. Slot
eviction still uses the existing pause/snapshot/remove/re-add barrier, retaining
verified overlap, complete partial blocks and peer/hash state in memory. This
connection churn can cause rebuffering. Strict capacity and successful test
fixtures do not imply uninterrupted playback for every torrent/network/container.
Streaming sessions use outbound TCP by default and allow incoming uTP. Libtorrent
otherwise repeats optimistic uTP negotiation after each re-add, even for a TCP-only
peer. Embedders can override `CacheStreamOptions::settings` when uTP-only outbound
access is needed, with the corresponding reconnect cost.

## Reserved slots and rebuilds

The cache normally keeps `ceil(slot_count / 8)` slots available for urgent reads
(zero below four slots, capped to leave two working slots). Prefetch uses the
remaining window; urgent playback/container reads may borrow the reserve.
Automatic reservation turns off when the whole cache holds less than one second
at the current consumption estimate, and returns above 1.25 seconds. This preserves
forward buffering for short/high-bitrate media. C++ callers may set
`CacheStreamOptions::reserved_slots` explicitly; zero disables reservation.

Expired pieces are reclaimed together at the existing pause/snapshot/remove/re-add
boundary. Verified overlap, active pins and required playback pieces survive.
Small speculative deficits wait for a batch; required misses bypass that delay.
Rolling and resize hysteresis limit window oscillation, while explicit user seeks
reposition immediately. Every occupied/reserved slot belongs to the same fixed
2.5% allocation; no additional payload file is created.

Local three-run medians: sequential rebuilds **111 -> 14**, seeking **25 -> 7**,
and short-clip VLC **97 -> 91**. VLC verified-data waiting fell **501 -> 210 ms**.
Individual batch rebuilds cost more, and these figures are not WAN guarantees.
See the [measured comparison](docs/cache-performance.md) and
[algorithm, configuration, examples and reproduction steps](docs/cache-optimization.md).

## Tests

FFmpeg is optional for the application and is used only to generate test media:

```bash
sudo pacman -S --needed ffmpeg
ctest --test-dir build --output-on-failure
```

When FFmpeg is available, CTest generates short noisy H.264/AAC and VP9/Opus
fixtures, tests them through a real loopback torrent and LibVLC, then removes the
generated media. The seeder's source fixtures are test inputs, not client cache
files. Client data is checked separately on a disk-backed filesystem.

Executed automated scenarios include MP4 with front/tail `moov`, MKV, WebM, MOV,
M4V, AVI, MPEG-TS, decoded video/audio counters, pause/resume, forward/backward time
seeks, byte-cache hits/misses, peer interruption/recovery, throttled reads, blocked
callback cancellation, stop during download, EOF, cleanup and actual allocated
disk blocks. A subprocess test also drives the complete CLI from magnet input
through selection, tail-metadata playback and EOF with default network settings.
Container unit tests verify bounded atom scanning and malformed
headers. Existing metadata-only, file-selection, v1/v2/hybrid cache, reader-pin,
allocation-failure and stale-session tests remain in CTest.

The automated output test uses memory video and dummy audio outputs. Visual
desktop rendering, audible sound, hardware decoding, arbitrary codecs, damaged
or encrypted media, public swarms and sustained WAN performance are **not**
validated by that headless test. These require separate manual/compatibility tests.
FFmpeg absence is reported at configure time and skips the real-media test;
container/cache/unit tests still run.

The local normal run passed all **12 CTest checks**. These include reserved-slot,
batch reconstruction, window stability and VLC peer-outage recovery regressions.
Separate CLI smoke runs also
verified quitting during startup and SIGTERM cleanup. Tail-MP4 playback peaked at
**327,680 allocated bytes against a 347,812-byte budget**, including managed
directories and lock/cache files.

For callback/lifetime checks:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-sanitize -j2
ctest --test-dir build-sanitize --output-on-failure
```

On this system the default LeakSanitizer run completes playback but fails its
exit check: 1,048 bytes in 21 allocations originate in dynamically loaded plugin
initialization and Fontconfig. A standalone LibVLC file player, linked without
Intezam or libtorrent, reproduces the same allocation families (216 bytes in five
allocations for one player). No invalid-memory or undefined-behavior report was
observed. This is **not** a leak-clean sanitizer result. To run the address/UB
checks separately while retaining leak checking on the non-playback tests:

```bash
ctest --test-dir build-sanitize --output-on-failure -E 'generate_vlc_media|vlc_cache_playback|cli_magnet_to_playback|cleanup_vlc_media'
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure -R 'vlc_cache_playback|cli_magnet_to_playback'
```

ThreadSanitizer checks for storage and concurrent reconstruction also passed:

```bash
cmake -S . -B build-thread-sanitize -DCMAKE_BUILD_TYPE=Debug \
    '-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread'
cmake --build build-thread-sanitize --target reserved_slot_tests batch_eviction_tests cache_tests -j2
ctest --test-dir build-thread-sanitize --output-on-failure \
    -R 'reserved_slot_admission|batched_torrent_reconstruction|bounded_disk_cache'
```

Set `INTEZAM_VLC_DEBUG=1` for verbose VLC diagnostics. Normal playback reports
callback/cache failures and fatal VLC errors through the CLI instead of printing
every unsuccessful demuxer probe. See [Guide.md](Guide.md) for implementation
boundaries, synchronization and API references.

## License

MIT; see [LICENSE](LICENSE).
