# Intezam CLI

A C++17 Linux CLI for torrent video detection and a network-adaptive, temporary
disk cache. After metadata retrieval and video selection, a simulated player
reads verified bytes while a sliding window follows its position. VLC and
container/codec parsing are not implemented.

## Build on Arch Linux / CachyOS

The custom disk interface targets **libtorrent-rasterbar 2.1.0+**, tested with
**2.1.2**. No third-party test framework is required.

```bash
sudo pacman -S --needed base-devel pkgconf cmake libtorrent-rasterbar
pkg-config --modversion libtorrent-rasterbar
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/intezam
```

Use `build/intezam`. Historical executables checked into the repository root
are not updated by this build. CMake commands work in both Bash and Fish.

Direct Bash build without CMake:

```bash
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wpedantic main.cpp src/*.cpp \
    $(pkg-config --cflags --libs libtorrent-rasterbar) -pthread -o build/intezam
```

For Fish, replace `$(pkg-config ...)` with
`(pkg-config --cflags --libs libtorrent-rasterbar | string split ' ')`.

## Usage

```bash
./build/intezam --help
./build/intezam --timeout 60 --bitrate 1048576
./build/intezam --simulate-seconds 30 --bitrate 262144
./build/intezam --metadata-only
```

Paste a magnet and select a video by its displayed number. Input is validated;
`q` or EOF cancels selection. Metadata retrieval defaults to a 120-second timeout
(`--timeout`: 1–3600 seconds). The metadata stage still makes no payload requests
and creates no files. `--metadata-only` exits after selection, preserving that
workflow without initializing storage or downloading payload.

Supported extensions, ignoring case: `.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`,
`.flv`, `.wmv`, `.m4v`, `.mpeg`, `.mpg`, `.ts`, `.m2ts`. Padding files, symlinks and
non-video extensions are excluded. Listings include full paths, formats,
MiB/GiB sizes and original zero-based torrent indexes. File extensions identify
candidates; they do not verify codecs or container compatibility.

After selection the default mode starts a cache simulation. Commands:

| Command | Effect |
|---|---|
| `seek 8388608` | Seek to a selected-file byte offset; report verified cache hit or miss |
| `rate 524288` | Change simulated consumption to 524,288 bytes/s |
| `pause` / `play` | Pause/resume simulated consumption; prefetch continues |
| `status` | Print cache, throughput, window and buffer diagnostics |
| `q` / `quit` | End the session and remove its temporary files |

The configured consumption rate defaults to 1 MiB/s. It is an explicit simulator
input, not an inferred media bitrate. Network throughput is measured from actual
payload counters and smoothed with an EWMA. `--simulate-seconds` (1–86400) ends
automatically and permits continued simulation when piped stdin reaches EOF.
Otherwise EOF exits. SIGINT/SIGTERM also unwind the cache. Missing playback data
causes buffering; a 120-second verified-data stall produces an error and cleanup.

## Bounded disk storage

Each session uses `$XDG_CACHE_HOME/intezam/session-XXXXXX/` or, by default,
`~/.cache/intezam/session-XXXXXX/`:

```text
stream.cache   # fixed-size preallocated slots, reused in place
session.lock   # empty advisory lock; mappings/resume state stay in memory
```

The budget is `floor(selected_video_size / 40)` bytes. Complete torrent pieces,
including bytes overlapping neighboring files, occupy filesystem-aligned slots.
Slot count is calculated after charging the allocated blocks of the session
directory, lock file and shared cache root directory, plus an allocation-unit
reserve. `posix_fallocate()` reserves the file before any payload requests. File
length is fixed; writes outside reserved slots are rejected. Actual allocated
blocks (`st_blocks * 512`) are checked at allocation and after writes.

If the budget cannot accommodate the required whole pieces/logical segment,
allocation overhead, or a filesystem that supports preallocation, the application
reports the limitation. ENOSPC and other filesystem errors stop the session.
Known RAM-backed tmpfs/ramfs roots are rejected; set `XDG_CACHE_HOME` to a
disk-backed filesystem if necessary.
Reader pins can temporarily defer new pieces; they never increase capacity.
There are no ordinary full-size torrent files, part files or on-disk resume files.

The disk bound covers application-owned cache file/directory allocations, not
RAM, filesystem journals, external snapshots or device-wide metadata that cannot
be attributed through `stat`. The root directory is shared and remains after
session cleanup; other live sessions have their own budgets. It is not a global
quota across concurrent sessions.

On normal exit the session directory is removed. After a crash/SIGKILL, the next
cache initialization removes recognized abandoned session directories only after
successfully acquiring their lock. Live sessions and unrecognized contents are
preserved. An outstanding playback read lease keeps its disk data and lock alive
until the reader releases it, even if the torrent session has already ended.

## Sliding window and seeking

Logical segments are half a torrent piece (minimum 16 KiB), relative to the video.
They can straddle torrent pieces when the video starts at an unaligned offset.
The segment **count is dynamic**: throughput, measured variability, configured
consumption and contiguous verified buffer health determine a target horizon.
It is clipped to the available whole-piece slots. Away from file boundaries,
`floor(window_segment_count / 3)` segments precede the current segment; the rest
contain current playback and forward prefetch. Boundary windows shift inward.

The desired window, admitted/queued pieces, actual download queue, verified
pieces and readable segments are tracked separately. Downloads are prioritized
at playback first, then ahead, then behind, with piece deadlines. Verified reads
use pins so eviction cannot overwrite active readers. Overlapping verified and
complete partial blocks are retained; unused old data can remain opportunistically
until space is needed. A target-window move does not make missing data readable.

Libtorrent 2.1 has no public single-piece availability-reset API. When a slot must
be evicted, Intezam pauses the torrent, takes an **in-memory** resume snapshot,
removes it, waits for its storage to release, reuses slots, and re-adds it with
only the resident verified pieces advertised. Complete retained blocks are
restored as unfinished pieces; peers, trackers and v2 hash state are retained.
This supported-API approach costs brief disconnections/reconnections on eviction.
It is a correctness-first foundation, not a guarantee of uninterrupted real-time
playback or instantaneous buffering after an uncached seek.

The custom disk implementation performs bounded reads/writes/hashes on the
libtorrent network thread and posts completion callbacks as required. A future
disk worker could improve throughput on slow drives; no unbounded disk job queue
is introduced here. See [Guide.md](Guide.md) for architecture and API references.

## Tests

```bash
ctest --test-dir build --output-on-failure
./build/cache_tests
./build/streaming_tests
```

- Metadata-only local peer: invalid magnets, timeouts, zero payload requests and
  zero created files/directories.
- Detection/selection: all extensions, case, original indexes, 64-bit sizes,
  invalid input, cancellation and EOF.
- Disk/window policy: actual allocated block measurements, strict budget, dynamic
  segment count, positioning and boundary cases, unaligned piece mapping,
  verified read gating, concurrent pinned readers, reuse, insufficient capacity,
  allocation failure, normal cleanup and crash recovery while preserving live locks.
- Local torrent streaming: v1 multi-file, v2 and hybrid torrents, real libtorrent
  verification, cached and uncached seeks, eviction/re-download, pinned data,
  short final pieces, network starvation, corrupted payload rejection and cleanup.
  A synthetic seeder serves zero-filled payload over loopback without writing a
  complete source video.

No public swarm is contacted by the CTest fixtures. Tests print independently
measured disk allocation and the corresponding byte budget.
Cache fixtures are created in the test working directory (normally `build/`),
which must be disk-backed, rather than relying on a potentially RAM-backed `/tmp`.

Local Btrfs measurements with libtorrent 2.1.2:

| Fixture | Maximum budget | Measured allocation |
|---|---:|---:|
| 16 MiB v1 video | 419,430 bytes | 393,216 bytes |
| 16 MiB + 123 bytes v2 video | 419,433 bytes | 393,216 bytes |
| 128 MiB slot/reader test | 3,355,443 bytes | 3,342,336 bytes |

The full CLI simulation also passed a live `du -s -B1` check on Btrfs, timed
cleanup and SIGTERM cleanup. Filesystem rounding varies; tests enforce the budget
using the actual allocation of each run. AddressSanitizer/UndefinedBehaviorSanitizer
validation can be reproduced with:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-sanitize -j2
ctest --test-dir build-sanitize --output-on-failure
```

## License

MIT; see [LICENSE](LICENSE).
