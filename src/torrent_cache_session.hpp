#pragma once

#include "cache_disk_io.hpp"
#include "dynamic_segment_cache.hpp"
#include "torrent_metadata.hpp"
#include "video_files.hpp"

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>

#include <chrono>
#include <functional>

namespace intezam {

struct CacheStreamOptions {
    std::filesystem::path root = CacheFile::defaultRoot();
    double playback_bytes_per_second = 1024 * 1024;
    std::chrono::milliseconds stall_timeout{120000};
    libtorrent::settings_pack settings = metadataSessionSettings();
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
    std::optional<PieceStore::ReadLease> read(std::int64_t offset, std::int64_t length);
    bool isRangeCached(std::int64_t offset, std::int64_t length) const;
    CacheDiagnostics diagnostics() const;
    std::filesystem::path directory() const { return store_->directory(); }

private:
    using Clock = std::chrono::steady_clock;
    void drain(std::chrono::milliseconds wait,
               const std::function<void(libtorrent::alert*)>& inspect = {});
    void waitUntil(const std::function<bool()>& complete);
    void addTorrent(libtorrent::add_torrent_params params);
    void schedule();
    void rebuild(const std::vector<int>& desired);
    void observeThroughput();
    void updatePosition(std::int64_t byte_offset);
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
    std::size_t rebuilds_ = 0;
};

} // namespace intezam
