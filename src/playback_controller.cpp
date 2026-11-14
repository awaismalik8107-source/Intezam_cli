#include "playback_controller.hpp"
#include <algorithm>
#include <stdexcept>

namespace intezam {
PlaybackController::PlaybackController(PlaybackOptions options) : options_(std::move(options)) {}
PlaybackController::~PlaybackController() { stopAndWait(); }
void PlaybackController::stopAndWait()
{
    requestStop();
    if (worker_.joinable()) worker_.join();
}
void PlaybackController::setVideoWindow(std::uint32_t window)
{
    if (worker_.joinable()) throw std::logic_error("Set the video window before starting playback.");
    options_.vlc.x11_window = window;
}
void PlaybackController::start(std::string magnet, std::shared_ptr<const libtorrent::torrent_info> metadata, VideoFile video)
{
    if (worker_.joinable()) throw std::logic_error("Playback controller has already started.");
    worker_ = std::thread([this, magnet = std::move(magnet), metadata = std::move(metadata), video = std::move(video)] {
        run(magnet, metadata, video);
    });
}
void PlaybackController::submit(PlaybackCommand command)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !snapshot_.ready || snapshot_.finished) return;
    // Keep only the newest pending slider/volume value while the worker is busy.
    if (command.type == PlaybackCommandType::seek || command.type == PlaybackCommandType::volume) {
        commands_.erase(std::remove_if(commands_.begin(), commands_.end(), [&](const auto& pending) {
            return pending.type == command.type;
        }), commands_.end());
    }
    commands_.push_back(command);
    changed_.notify_one();
}
void PlaybackController::requestStop()
{
    stopping_ = true;
    std::shared_ptr<TorrentVideoSource> source;
    { std::lock_guard<std::mutex> lock(mutex_); source = source_; }
    if (source) source->cancelPendingReads();
    changed_.notify_one();
}
PlaybackSnapshot PlaybackController::snapshot() const
{
    std::shared_ptr<TorrentVideoSource> source;
    PlaybackSnapshot result;
    { std::lock_guard<std::mutex> lock(mutex_); result = snapshot_; source = source_; }
    if (source && !result.finished) result.source = source->status();
    return result;
}
void PlaybackController::apply(VlcPlayer& player, PlaybackCommand command)
{
    switch (command.type) {
        case PlaybackCommandType::toggle_pause: player.togglePause(); break;
        case PlaybackCommandType::volume: player.setVolume(static_cast<int>(command.value)); break;
        case PlaybackCommandType::mute: player.setMuted(!player.status().muted); break;
        case PlaybackCommandType::seek:
        case PlaybackCommandType::skip: {
            const auto status = player.status();
            if (!status.seekable || status.duration_ms <= 0) throw std::invalid_argument("Seeking is not available yet.");
            const auto target = command.type == PlaybackCommandType::skip ? status.time_ms + command.value : command.value;
            player.seekTo(std::clamp<std::int64_t>(target, 0, status.duration_ms - 1));
            break;
        }
    }
}
void PlaybackController::run(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video)
{
    std::shared_ptr<TorrentVideoSource> source;
    try {
        auto cache_options = options_.cache;
        const auto external = cache_options.cancelled;
        cache_options.cancelled = [this, external] { return stopping_.load() || (external && external()); };
        if (stopping_) throw ReadCancelled{};
        source = std::make_shared<TorrentVideoSource>(magnet, std::move(metadata), video, cache_options);
        { std::lock_guard<std::mutex> lock(mutex_); source_ = source; snapshot_.phase = "Reading container and buffering…"; }
        if (stopping_) { source->cancelPendingReads(); throw ReadCancelled{}; }
        source->setContainer(probeContainer(*source));
        source->prebuffer();
        if (stopping_) throw ReadCancelled{};
        VlcPlayer player(source, options_.vlc);
        player.play();
        const auto started = std::chrono::steady_clock::now();
        bool decoded = false;
        { std::lock_guard<std::mutex> lock(mutex_); snapshot_.ready = true; snapshot_.phase.clear(); }
        while (!stopping_) {
            std::deque<PlaybackCommand> commands;
            { std::lock_guard<std::mutex> lock(mutex_); commands.swap(commands_); }
            for (const auto command : commands) {
                if (stopping_) break;
                try {
                    apply(player, command);
                    std::lock_guard<std::mutex> lock(mutex_); snapshot_.notice.clear();
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(mutex_); snapshot_.notice = error.what();
                }
            }
            const auto media = player.status();
            const auto cache = source->status();
            if (!cache.error.empty()) throw std::runtime_error(cache.error);
            if (!media.error.empty()) throw std::runtime_error(media.error);
            if (media.state == libvlc_Error) throw std::runtime_error("VLC could not play this video.");
            decoded = decoded || media.decoded_video_frames > 0;
            { std::lock_guard<std::mutex> lock(mutex_); snapshot_.player = media; snapshot_.source = cache; }
            if (media.state == libvlc_Ended) {
                if (!decoded) throw std::runtime_error("Video ended without decoding any frames.");
                break;
            }
            if (!decoded && std::chrono::steady_clock::now() - started > options_.cache.stall_timeout) {
                throw std::runtime_error("Timed out waiting for the first video frame.");
            }
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds{50}, [this] { return stopping_ || !commands_.empty(); });
        }
        player.stop();
    } catch (const std::exception& error) {
        if (!stopping_) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.player.error = error.what(); }
    }
    if (source) source->stop();
    std::lock_guard<std::mutex> lock(mutex_);
    source_.reset();
    snapshot_.finished = true;
    snapshot_.ready = false;
    snapshot_.phase = snapshot_.player.state == libvlc_Ended ? "Playback ended" : "Playback stopped";
}
}
