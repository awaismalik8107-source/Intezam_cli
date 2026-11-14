# Engine 2.0 audit and design

Baseline revision: f6a4a77058bdf17dc142f17839fb5fa49693614c.

Peers feed libtorrent's picker, then the custom disk backend writes bounded slots.
Only the piece-pass plugin marks data verified. PieceStore leases pin verified
pieces until the byte source finishes reading; VLC never receives raw disk jobs.
The video source has a worker and a serialized read-demand queue. Container probes
use the same verified path and a bounded unique-piece allowance; metadata reads do
not move playback. Duration from VLC refines the initial configured consumption
estimate. This remains an average bitrate, not a VBR packet-rate oracle.

The old automatic budget is max(video / 40, four aligned slots plus filesystem
overhead), capped by a percentage. Strict mode always uses video / 40. Allocation
uses posix_fallocate and measured filesystem blocks. Reservations, protected
pieces and pinned readers already exist. Free slots are admitted first; eviction
requires pause, resume snapshot, torrent removal, disk-storage teardown, grouped
reclamation, and re-add. Overlap, partial blocks and peers survive that boundary.
There is no supported public libtorrent operation to forget one completed piece.

The logical window uses half-piece segments, roughly one third behind and two
thirds ahead, with movement/resize hysteresis. Urgent metadata shares its slot
budget. Previous throughput is a payload EWMA; every unverified forward piece
gets a deadline/highest priority. Repeated deadlines are already suppressed.
Startup and rebuffer thresholds already use consecutive verified data.

Blocking operations: bounded synchronous disk reads, writes and hashes run on
libtorrent's network thread; slot zeroing and allocation hold the store mutex;
reconstruction synchronizes the controller worker. VLC callbacks can wait for
verification, but do not run controller synchronization. This work keeps that
architecture and instruments its costs rather than introducing another I/O queue.

Design: add a demand-aware verified-throughput estimator (short/long EWMA,
variation, pending-demand stall time, peer count), a pure adaptive budget policy,
and a verified-buffer state machine. Initial bytes derive from estimated video
consumption times a configurable forward duration, 1.5x rolling coverage, aligned
slots, deduplicated outside-window protection, reservation and stability margin.
Video length caps useful slots; it is not a percentage multiplier. Absolute bytes
and available space minus a configurable reserve cap every allocation.

Runtime desired, physical and occupied capacity are distinct. Meaningful target
changes use cooldown/hysteresis. Append-only physical expansion uses PieceStore's
I/O mutex, preserves every slot offset, and is checked against disk space and the
absolute ceiling. Existing leases remain valid. Physical shrinking is deferred
until teardown/next session; a smaller logical window never truncates live data.
Libtorrent's safe reconstruction barrier remains the only eviction boundary.
Near-term data gets deadlines/priority 7, near future 6, distant forward 4,
retained backward 1; deadlines are not assigned to the whole horizon.

Baseline instrumentation and workload were added as source changes before
policy changes: three repetitions each of paced sequential reads, repeated seeks,
a 500 ms seeder outage, and actual headless VLC decode. Raw CSVs separate read
waits from player buffering. Loopback tests are not WAN guarantees.
