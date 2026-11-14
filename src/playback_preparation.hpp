#pragma once

#include "torrent_video_source.hpp"
#include <condition_variable>
#include <optional>

namespace intezam {

struct PreparationSnapshot {
    std::shared_ptr<TorrentVideoSource> source;
    std::optional<ContainerInfo> container;
    bool finished = false;
    std::string error;
};

// One bounded speculative job. Retain it when its candidate is selected; cancel
// and release it before preparing a different file under the same disk budget.
class PlaybackPreparation {
public:
    PlaybackPreparation(std::string magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
        VideoFile video, CacheStreamOptions options);
    ~PlaybackPreparation();
    PlaybackPreparation(const PlaybackPreparation&) = delete;
    PlaybackPreparation& operator=(const PlaybackPreparation&) = delete;
    bool matches(const VideoFile& video) const;
    PreparationSnapshot snapshot() const;
    std::shared_ptr<TorrentVideoSource> waitForSource();
    std::optional<ContainerInfo> waitForHeaders();
    void cancel();
private:
    void run(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
        const CacheStreamOptions& options);
    const VideoFile video_;
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    PreparationSnapshot snapshot_;
    std::thread worker_;
};

std::optional<VideoFile> headerPrefetchCandidate(const std::vector<VideoFile>& videos,
    int piece_length, CacheBudgetOptions budget);

} // namespace intezam
