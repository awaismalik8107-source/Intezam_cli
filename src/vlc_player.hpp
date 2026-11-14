#pragma once

#include "video_byte_source.hpp"
#include "stream_policy.hpp"

#include <vlc/vlc.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace intezam {

struct VlcOptions {
    bool headless = false;
    int caching_milliseconds = 100;
    std::uint32_t x11_window = 0;
};

struct PlaybackStatus {
    libvlc_state_t state = libvlc_NothingSpecial;
    std::int64_t time_ms = 0;
    std::int64_t duration_ms = 0;
    float buffering_percent = 0;
    int decoded_video_frames = 0;
    int decoded_audio_frames = 0;
    bool seekable = false;
    bool user_paused = false;
    bool waiting_for_buffer = false;
    int volume = 100;
    bool muted = false;
    std::uint64_t bytes_read = 0;
    std::uint64_t seeks = 0;
    std::string error;
};

class VlcPlayer {
public:
    explicit VlcPlayer(std::shared_ptr<VideoByteSource> source, const VlcOptions& options = {});
    ~VlcPlayer();
    VlcPlayer(const VlcPlayer&) = delete;
    VlcPlayer& operator=(const VlcPlayer&) = delete;
    void play();
    void pause();
    void resume();
    void togglePause();
    void seekTo(std::int64_t milliseconds);
    void setVolume(int percent);
    void setMuted(bool muted);
    void stop();
    PlaybackStatus status();
    static const char* stateName(libvlc_state_t state);

private:
    struct Cursor {
        VlcPlayer* owner;
        std::shared_ptr<VideoByteSource> source;
        std::uint64_t offset = 0;
    };
    static int openInput(void* opaque, void** cursor, std::uint64_t* size) noexcept;
    static ssize_t readInput(void* opaque, unsigned char* buffer, std::size_t length) noexcept;
    static int seekInput(void* opaque, std::uint64_t offset) noexcept;
    static void closeInput(void* opaque) noexcept;
    static void onEvent(const libvlc_event_t* event, void* opaque) noexcept;
    static void onLog(void* opaque, int level, const libvlc_log_t* context, const char* format, va_list arguments) noexcept;
    static void* lockFrame(void* opaque, void** planes) noexcept;
    static void displayFrame(void* opaque, void* picture) noexcept;
    void recordError(const char* message) noexcept;
    void startInput(std::int64_t milliseconds);
    void stopInput();
    void updateBuffering(const PlaybackStatus& status);

    std::shared_ptr<VideoByteSource> source_;
    libvlc_instance_t* instance_ = nullptr;
    libvlc_media_player_t* player_ = nullptr;
    libvlc_media_t* media_ = nullptr;
    std::mutex controls_;
    std::mutex errors_;
    std::string callback_error_;
    std::string last_vlc_error_;
    std::atomic<bool> suspended_{true};
    std::atomic<float> buffering_{0};
    std::atomic<std::uint64_t> read_bytes_{0};
    std::atomic<std::uint64_t> seek_count_{0};
    bool stopped_ = false;
    bool user_paused_ = false;
    bool buffer_paused_ = false;
    int volume_ = 100;
    bool muted_ = false;
    bool audio_settings_pending_ = true;
    RebufferGate rebuffer_gate_;
    bool pause_after_start_ = false;
    bool headless_ = false;
    int caching_ms_ = 100;
    std::unique_ptr<unsigned char, decltype(&std::free)> headless_frame_{nullptr, &std::free};
    std::atomic<int> displayed_frames_{0};
    std::optional<std::int64_t> requested_seek_;
    std::uint64_t seek_count_at_request_ = 0;
    std::chrono::steady_clock::time_point seek_started_;
};

} // namespace intezam
