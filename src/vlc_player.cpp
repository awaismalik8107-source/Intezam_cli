#include "vlc_player.hpp"
#include "torrent_video_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace intezam {

VlcPlayer::VlcPlayer(std::shared_ptr<VideoByteSource> source, const VlcOptions& options)
    : source_(std::move(source)), headless_(options.headless), caching_ms_(options.caching_milliseconds)
{
    if (!source_ || source_->size() <= 0) throw std::invalid_argument("VLC requires a nonempty video byte source.");
    if (options.caching_milliseconds < 0 || options.caching_milliseconds > 60000) {
        throw std::invalid_argument("VLC input caching must be between 0 and 60000 milliseconds.");
    }
    std::vector<std::string> arguments{"--ignore-config", "--no-video-title-show", "--no-media-library", "--no-osd", "--stats",
        "--no-sub-autodetect-file", "--no-mkv-preload-local-dir", "--avi-index=2",
        "--prefetch-buffer-size=64", "--prefetch-read-size=16384", "--prefetch-seek-threshold=1",
        "--file-caching=" + std::to_string(options.caching_milliseconds),
        "--network-caching=" + std::to_string(options.caching_milliseconds)};
    if (options.headless) {
        headless_frame_.reset(static_cast<unsigned char*>(std::aligned_alloc(32, 320 * 240 * 4)));
        if (!headless_frame_) throw std::bad_alloc{};
        arguments.emplace_back("--vout=dummy");
        arguments.emplace_back("--aout=dummy");
        arguments.emplace_back("--avcodec-hw=none");
        arguments.emplace_back("--codec=ffmpeg");
    }
    std::vector<const char*> argv;
    if (std::getenv("INTEZAM_VLC_DEBUG")) arguments.emplace_back("--verbose=2");
    for (const auto& argument : arguments) argv.push_back(argument.c_str());
    instance_ = libvlc_new(static_cast<int>(argv.size()), argv.data());
    if (!instance_) throw std::runtime_error("Cannot initialize LibVLC; check VLC plugin installation.");
    player_ = libvlc_media_player_new(instance_);
    if (!player_) { libvlc_release(instance_); throw std::runtime_error("Cannot create LibVLC media player."); }
    if (libvlc_event_attach(libvlc_media_player_event_manager(player_), libvlc_MediaPlayerBuffering, onEvent, this) != 0) {
        libvlc_media_player_release(player_);
        libvlc_release(instance_);
        throw std::runtime_error("Cannot attach VLC buffering event.");
    }
    if (headless_) {
        libvlc_video_set_callbacks(player_, lockFrame, nullptr, displayFrame, this);
        libvlc_video_set_format(player_, "RV32", 320, 240, 320 * 4);
    }
    if (!std::getenv("INTEZAM_VLC_DEBUG")) libvlc_log_set(instance_, onLog, this);
}

VlcPlayer::~VlcPlayer()
{
    stop();
    libvlc_event_detach(libvlc_media_player_event_manager(player_), libvlc_MediaPlayerBuffering, onEvent, this);
    libvlc_media_player_release(player_);
    if (media_) libvlc_media_release(media_);
    libvlc_log_unset(instance_);
    libvlc_release(instance_);
}

void VlcPlayer::recordError(const char* message) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(errors_);
        callback_error_ = message;
    } catch (...) { /* No exception may cross LibVLC's C callback boundary. */ }
}

int VlcPlayer::openInput(void* opaque, void** cursor, std::uint64_t* size) noexcept
{
    auto& self = *static_cast<VlcPlayer*>(opaque);
    try {
        self.source_->seek(0);
        *cursor = new Cursor{&self, self.source_, 0};
        *size = self.source_->size();
        return 0;
    } catch (const ReadCancelled&) { return -1; }
    catch (const std::exception& error) { self.recordError(error.what()); return -1; }
    catch (...) { self.recordError("Unexpected VLC input-open failure"); return -1; }
}

ssize_t VlcPlayer::readInput(void* opaque, unsigned char* buffer, std::size_t length) noexcept
{
    auto& cursor = *static_cast<Cursor*>(opaque);
    if (cursor.owner->suspended_.load()) return -1;
    try {
        const auto read = cursor.source->read(static_cast<std::int64_t>(cursor.offset), buffer, length);
        cursor.offset += read;
        cursor.owner->read_bytes_ += read;
        return static_cast<ssize_t>(read);
    } catch (const ReadCancelled&) { return -1; }
    catch (const std::exception& error) { cursor.owner->recordError(error.what()); return -1; }
    catch (...) { cursor.owner->recordError("Unexpected VLC input-read failure"); return -1; }
}

int VlcPlayer::seekInput(void* opaque, std::uint64_t offset) noexcept
{
    auto& cursor = *static_cast<Cursor*>(opaque);
    if (cursor.owner->suspended_.load() || offset > static_cast<std::uint64_t>(cursor.source->size())) return -1;
    try {
        cursor.source->seek(static_cast<std::int64_t>(offset));
        cursor.offset = offset;
        ++cursor.owner->seek_count_;
        return 0;
    } catch (const ReadCancelled&) { return -1; }
    catch (const std::exception& error) { cursor.owner->recordError(error.what()); return -1; }
    catch (...) { cursor.owner->recordError("Unexpected VLC input-seek failure"); return -1; }
}

void VlcPlayer::closeInput(void* opaque) noexcept { delete static_cast<Cursor*>(opaque); }

void VlcPlayer::onEvent(const libvlc_event_t* event, void* opaque) noexcept
{
    auto& self = *static_cast<VlcPlayer*>(opaque);
    if (event->type == libvlc_MediaPlayerBuffering) self.buffering_.store(event->u.media_player_buffering.new_cache);
    // Never call LibVLC control APIs from its event thread.
}

void VlcPlayer::onLog(void* opaque, int level, const libvlc_log_t*, const char* format, va_list arguments) noexcept
{
    if (level < LIBVLC_ERROR) return;
    auto& self = *static_cast<VlcPlayer*>(opaque);
    char message[1024];
    va_list copy;
    va_copy(copy, arguments);
    std::vsnprintf(message, sizeof(message), format, copy);
    va_end(copy);
    try {
        std::lock_guard<std::mutex> lock(self.errors_);
        self.last_vlc_error_ = message;
    } catch (...) {}
}

void* VlcPlayer::lockFrame(void* opaque, void** planes) noexcept
{
    auto& self = *static_cast<VlcPlayer*>(opaque);
    planes[0] = self.headless_frame_.get();
    return nullptr;
}

void VlcPlayer::displayFrame(void* opaque, void*) noexcept
{ ++static_cast<VlcPlayer*>(opaque)->displayed_frames_; }

void VlcPlayer::stopInput()
{
    suspended_.store(true);
    source_->cancelPendingReads(); // wake callbacks BEFORE the synchronous LibVLC 3 stop
    libvlc_media_player_stop(player_);
}

void VlcPlayer::startInput(std::int64_t milliseconds)
{
    if (stopped_) throw std::runtime_error("Playback has already been stopped.");
    {
        std::lock_guard<std::mutex> lock(errors_);
        callback_error_.clear();
        last_vlc_error_.clear();
    }
    source_->resumeReads();
    if (auto torrent = std::dynamic_pointer_cast<TorrentVideoSource>(source_)) torrent->beginPlaybackEpoch();
    auto* media = libvlc_media_new_callbacks(instance_, openInput, readInput, seekInput, closeInput, this);
    if (!media) throw std::runtime_error("Cannot create LibVLC callback media.");
    libvlc_media_add_option(media, ":avi-index=2");
    libvlc_media_add_option(media, ":no-mkv-preload-local-dir");
    libvlc_media_add_option(media, ":no-mkv-preload-clusters");
    libvlc_media_add_option(media, ":prefetch-buffer-size=64");
    libvlc_media_add_option(media, ":prefetch-read-size=16384");
    // Zero makes VLC 3's prefetch thread repeatedly seek at an empty buffer.
    libvlc_media_add_option(media, ":prefetch-seek-threshold=1");
    libvlc_media_add_option(media, (":file-caching=" + std::to_string(caching_ms_)).c_str());
    libvlc_media_add_option(media, (":network-caching=" + std::to_string(caching_ms_)).c_str());
    if (headless_) {
        libvlc_media_add_option(media, ":avcodec-hw=none");
        libvlc_media_add_option(media, ":codec=ffmpeg");
    }
    if (milliseconds > 0) {
        std::ostringstream option;
        option.imbue(std::locale::classic());
        option << ":start-time=" << std::fixed << std::setprecision(3) << milliseconds / 1000.0;
        libvlc_media_add_option(media, option.str().c_str());
    }
    libvlc_media_player_set_media(player_, media);
    if (media_) libvlc_media_release(media_);
    media_ = media;
    buffering_.store(0);
    displayed_frames_.store(0);
    suspended_.store(false);
    if (libvlc_media_player_play(player_) != 0) throw std::runtime_error("LibVLC could not start playback.");
}

void VlcPlayer::play()
{
    std::lock_guard<std::mutex> lock(controls_);
    if (libvlc_media_player_get_state(player_) == libvlc_Paused) libvlc_media_player_set_pause(player_, 0);
    else if (!media_) startInput(0);
}

void VlcPlayer::pause()
{
    std::lock_guard<std::mutex> lock(controls_);
    libvlc_media_player_set_pause(player_, 1);
}

void VlcPlayer::resume()
{
    std::lock_guard<std::mutex> lock(controls_);
    libvlc_media_player_set_pause(player_, 0);
}

void VlcPlayer::togglePause()
{
    std::lock_guard<std::mutex> lock(controls_);
    libvlc_media_player_set_pause(player_, libvlc_media_player_get_state(player_) == libvlc_Paused ? 0 : 1);
}

void VlcPlayer::seekTo(std::int64_t milliseconds)
{
    std::lock_guard<std::mutex> lock(controls_);
    const auto duration = libvlc_media_player_get_length(player_);
    if (milliseconds < 0 || duration <= 0 || milliseconds >= duration) throw std::out_of_range("Seek time exceeds known media duration.");
    if (!libvlc_media_player_is_seekable(player_)) throw std::invalid_argument("VLC does not support time seeking for this stream.");
    if (auto torrent = std::dynamic_pointer_cast<TorrentVideoSource>(source_)) torrent->beginPlaybackEpoch();
    requested_seek_ = milliseconds;
    seek_count_at_request_ = seek_count_.load();
    seek_started_ = std::chrono::steady_clock::now();
    libvlc_media_player_set_time(player_, milliseconds);
}

void VlcPlayer::stop()
{
    std::lock_guard<std::mutex> lock(controls_);
    if (stopped_) return;
    stopInput();
    source_->stop();
    stopped_ = true;
}

PlaybackStatus VlcPlayer::status()
{
    std::lock_guard<std::mutex> lock(controls_);
    if (requested_seek_ && !stopped_) {
        const auto current = libvlc_media_player_get_time(player_);
        const auto torrent = std::dynamic_pointer_cast<TorrentVideoSource>(source_);
        const bool input_ready = !torrent || torrent->status().waiting_reads == 0
            || seek_count_.load() != seek_count_at_request_;
        if (input_ready && std::abs(current - *requested_seek_) <= 250) requested_seek_.reset();
        else if (std::chrono::steady_clock::now() - seek_started_ > std::chrono::milliseconds{750}) {
            // A callback may be blocked on the old location. Cancel it before
            // stopping and reopening; ordinary cached seeks retain the demuxer.
            const auto target = *requested_seek_;
            requested_seek_.reset();
            pause_after_start_ = libvlc_media_player_get_state(player_) == libvlc_Paused;
            stopInput();
            startInput(target);
        }
    }
    PlaybackStatus status;
    status.state = libvlc_media_player_get_state(player_);
    status.time_ms = std::max<libvlc_time_t>(0, libvlc_media_player_get_time(player_));
    status.duration_ms = std::max<libvlc_time_t>(0, libvlc_media_player_get_length(player_));
    status.seekable = libvlc_media_player_is_seekable(player_) != 0;
    status.buffering_percent = buffering_.load();
    libvlc_media_stats_t statistics{};
    if (media_ && libvlc_media_get_stats(media_, &statistics)) {
        status.decoded_video_frames = statistics.i_decoded_video;
        status.decoded_audio_frames = statistics.i_decoded_audio;
    }
    status.decoded_video_frames = std::max(status.decoded_video_frames, displayed_frames_.load());
    if (auto torrent = std::dynamic_pointer_cast<TorrentVideoSource>(source_)) {
        if (!requested_seek_ && (status.decoded_video_frames > 0 || status.time_ms > 0)) torrent->markPlaybackActive();
        if (status.duration_ms > 0) torrent->setEstimatedRate(source_->size() * 1000.0 / status.duration_ms);
    }
    if (pause_after_start_ && status.state == libvlc_Playing) {
        libvlc_media_player_set_pause(player_, 1);
        pause_after_start_ = false;
    }
    status.bytes_read = read_bytes_.load();
    status.seeks = seek_count_.load();
    {
        std::lock_guard<std::mutex> lock(errors_);
        status.error = callback_error_;
        if (status.error.empty() && status.state == libvlc_Error) status.error = last_vlc_error_;
    }
    if (!status.error.empty()) status.state = libvlc_Error;
    return status;
}

const char* VlcPlayer::stateName(libvlc_state_t state)
{
    switch (state) {
        case libvlc_Opening: return "OPENING";
        case libvlc_Buffering: return "BUFFERING";
        case libvlc_Playing: return "PLAYING";
        case libvlc_Paused: return "PAUSED";
        case libvlc_Stopped: return "STOPPED";
        case libvlc_Ended: return "ENDED";
        case libvlc_Error: return "ERROR";
        default: return "PREPARING";
    }
}

} // namespace intezam
