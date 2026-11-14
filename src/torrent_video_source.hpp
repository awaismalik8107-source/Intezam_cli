#pragma once

#include "container_probe.hpp"
#include "stream_policy.hpp"
#include "torrent_cache_session.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace intezam {

struct VideoSourceStatus {
    CacheDiagnostics cache{};
    std::size_t waiting_reads = 0;
    std::uint64_t delivered_bytes = 0;
    bool stopped = false;
    std::string error;
    ReadPerformance performance;
    BufferWatermarks buffer_targets{};
};

class TorrentVideoSource final : public VideoByteSource {
public:
    TorrentVideoSource(const std::string& magnet,
        std::shared_ptr<const libtorrent::torrent_info> metadata, const VideoFile& video,
        CacheStreamOptions options = {});
    ~TorrentVideoSource() override;
    std::int64_t size() const override { return video_size_; }
    std::size_t read(std::int64_t offset, void* buffer, std::size_t length) override;
    std::size_t readMetadata(std::int64_t offset, void* buffer, std::size_t length) override;
    void seek(std::int64_t offset) override;
    void seekForDemux(std::int64_t offset) override;
    void cancelPendingReads() override;
    void resumeReads() override;
    void stop() override;
    void setContainer(ContainerInfo container);
    void beginPlaybackEpoch();
    void markPlaybackActive();
    void setEstimatedRate(double bytes_per_second);
    void prebuffer();
    BufferWatermarks bufferTargets() const;
    VideoSourceStatus status() const;
    std::filesystem::path directory() const { return directory_; }
    std::int64_t capacity() const { return capacity_; }

private:
    struct ReadSignal {
        std::mutex mutex;
        std::condition_variable changed;
        void notify()
        {
            // Synchronize with the predicate check -> wait handoff, preventing
            // a verification/stop notification from being lost between them.
            std::lock_guard<std::mutex> lock(mutex);
            changed.notify_all();
        }
    };
    struct PendingRead {
        VideoRange range;
        std::uint64_t epoch;
    };
    enum class ReadPurpose { playback, metadata, prebuffer };
    std::size_t readVerified(std::int64_t offset, void* buffer, std::size_t length, ReadPurpose purpose);
    void run();
    BufferWatermarks bufferTargetsLocked() const;
    void checkReadState(std::uint64_t epoch) const;
    void accountProbeRange(std::int64_t offset, std::size_t length);
    void seekToByte(std::int64_t offset, PlaybackMovement movement);
    const std::int64_t video_size_;
    const std::int64_t torrent_offset_;
    const int piece_length_;
    const std::chrono::milliseconds read_timeout_;
    std::atomic<bool> stopping_{false};
    std::unique_ptr<TorrentCacheSession> cache_;
    std::filesystem::path directory_;
    std::int64_t capacity_;
    std::uint64_t probe_limit_;
    std::shared_ptr<ReadSignal> read_signal_ = std::make_shared<ReadSignal>();
    std::mutex stop_mutex_;
    std::deque<std::shared_ptr<PendingRead>> pending_;
    std::uint64_t epoch_ = 0;
    std::uint64_t probe_bytes_ = 0;
    std::map<std::int64_t, std::int64_t> probe_ranges_;
    bool probing_ = true;
    bool suspended_ = false;
    std::int64_t playback_offset_ = 0;
    bool seek_requested_ = false;
    bool reposition_next_demux_seek_ = false;
    double requested_rate_;
    ContainerInfo container_;
    VideoSourceStatus status_;
    std::thread worker_;
};

} // namespace intezam
