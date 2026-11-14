#pragma once

#include "cache_disk_io.hpp"
#include "cache_metrics.hpp"
#include "dynamic_segment_cache.hpp"
#include "torrent_metadata.hpp"
#include "video_files.hpp"

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>

#include <chrono>
#include <functional>

namespace intezam {

// Rebuilt cache torrents otherwise repeat optimistic uTP handshakes, including
// for TCP-only peers. Keep incoming uTP, but prefer reliable TCP reconnection.
libtorrent::settings_pack streamingSessionSettings();

struct CacheStreamOptions {
    std::filesystem::path root = CacheFile::defaultRoot();
    double playback_bytes_per_second = 1024 * 1024;
    std::chrono::milliseconds stall_timeout{120000};
    libtorrent::settings_pack settings = streamingSessionSettings();
    std::function<bool()> cancelled;
    std::optional<std::size_t> reserved_slots; // automatic when unset; no extra disk allocation
    std::function<void(int)> on_piece_verified; // network thread; must not throw or call control APIs
};

struct CacheDiagnostics {
    SegmentWindow desired;
    std::int64_t playback_position;
    std::int64_t budget_bytes;
    std::int64_t allocated_bytes;
    std::int64_t received_bytes;
    std::size_t resident_pieces;
    std::size_t verified_pieces;
    std::size_t downloading_pieces;
    std::size_t queued_pieces;
    std::size_t desired_verified_segments;
    std::size_t downloading_segments;
    std::size_t deferred_pieces;
    std::size_t torrent_rebuilds;
    double network_bytes_per_second;
    double playback_bytes_per_second;
    double forward_buffer_seconds;
    bool ready;
    std::size_t urgent_pieces;
    CachePerformance performance{};
};

// Control methods are serialized internally. Playback may hold/use ReadLease
// concurrently with pump/seek: PieceStore independently synchronizes disk I/O.
class TorrentCacheSession {
public:
    TorrentCacheSession(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
                        const VideoFile& video, const CacheStreamOptions& options = {});
    ~TorrentCacheSession();
    void pump(std::chrono::milliseconds wait = std::chrono::milliseconds{0});
    bool seek(std::int64_t byte_offset);
    void setPlaybackPosition(std::int64_t byte_offset);
    void setPlaybackRate(double bytes_per_second);
    void updateReadRequest(std::int64_t playback_offset, std::optional<VideoRange> demand,
                           PlaybackMovement movement = PlaybackMovement::sequential);
    std::optional<PieceStore::ReadLease> read(std::int64_t offset, std::int64_t length);
    bool isRangeCached(std::int64_t offset, std::int64_t length) const;
    CacheDiagnostics diagnostics() const;
    std::filesystem::path directory() const { return store_->directory(); }
    std::int64_t dataCapacity() const { return static_cast<std::int64_t>(store_->slotCount()) * geometry_.piece_length; }

private:
    struct AdmissionPlan {
        SegmentWindow window;
        std::vector<int> required;
        std::vector<int> prefetch;
        std::vector<int> keep;
    };
    using Clock = std::chrono::steady_clock;
    void drain(std::chrono::milliseconds wait,
               const std::function<void(libtorrent::alert*)>& inspect = {});
    void waitUntil(const std::function<bool()>& complete);
    void addTorrent(libtorrent::add_torrent_params params, const AdmissionPlan& plan);
    void schedule();
    AdmissionPlan planAdmissions();
    bool shouldRebuild(const AdmissionPlan& plan, const AdmissionResult& admission) const;
    void applyPriorities(const AdmissionPlan& plan);
    void rebuild(const AdmissionPlan& plan);
    void observeThroughput();
    void updateReservation(double bytes_per_second);
    void updatePosition(std::int64_t byte_offset);
    std::vector<int> urgentPieces() const;
    void checkCancelled() const;
    double forwardBufferSeconds() const;
    libtorrent::add_torrent_params safeParams(libtorrent::add_torrent_params params);

    mutable std::mutex control_mutex_;
    std::shared_ptr<const libtorrent::torrent_info> metadata_;
    PieceGeometry geometry_;
    CacheStreamOptions options_;
    std::shared_ptr<PieceStore> store_;
    DynamicSegmentCache controller_;
    std::shared_ptr<CacheDiskState> disk_state_;
    std::unique_ptr<libtorrent::session> session_;
    libtorrent::torrent_handle torrent_;
    std::vector<libtorrent::tcp::endpoint> known_peers_;
    std::vector<int> scheduled_;
    std::int64_t last_torrent_bytes_ = 0;
    std::int64_t received_bytes_ = 0;
    std::int64_t sampled_bytes_ = 0;
    Clock::time_point sample_time_ = Clock::now();
    Clock::time_point last_progress_ = Clock::now();
    std::vector<int> last_verified_;
    std::string last_peer_error_;
    std::optional<VideoRange> read_demand_;
    std::size_t rebuilds_ = 0;
    CachePerformance performance_;
    std::optional<VideoRange> last_window_;
    std::optional<SegmentWindow> planned_window_;
};

} // namespace intezam
