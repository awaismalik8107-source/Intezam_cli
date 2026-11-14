#pragma once
#include "vlc_playback.hpp"
#include "torrent_video_source.hpp"
#include <condition_variable>
#include <deque>
#include <thread>

namespace intezam {
enum class PlaybackCommandType { toggle_pause, seek, skip, volume, mute };
struct PlaybackCommand { PlaybackCommandType type; std::int64_t value = 0; };
struct PlaybackSnapshot {
    PlaybackStatus player;
    VideoSourceStatus source;
    std::string phase = "Preparing playback…";
    std::string notice;
    bool ready = false;
    bool finished = false;
};
class PlaybackControls {
public:
    virtual ~PlaybackControls() = default;
    virtual void submit(PlaybackCommand command) = 0;
    virtual void requestStop() = 0;
    virtual PlaybackSnapshot snapshot() const = 0;
};
// All LibVLC control calls and cache preparation run off the UI thread.
class PlaybackController final : public PlaybackControls {
public:
    explicit PlaybackController(PlaybackOptions options);
    ~PlaybackController() override;
    void setVideoWindow(std::uint32_t window);
    void start(std::string magnet, std::shared_ptr<const libtorrent::torrent_info> metadata, VideoFile video);
    void submit(PlaybackCommand command) override;
    void requestStop() override;
    // Blocking teardown for after the UI event loop exits; keep its native surface alive.
    void stopAndWait();
    PlaybackSnapshot snapshot() const override;
private:
    void run(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata, const VideoFile& video);
    void apply(VlcPlayer& player, PlaybackCommand command);
    PlaybackOptions options_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<PlaybackCommand> commands_;
    std::shared_ptr<TorrentVideoSource> source_;
    PlaybackSnapshot snapshot_;
    std::thread worker_;
};
}
