#include "vlc_playback.hpp"
#include "torrent_video_source.hpp"
#include "playback_preparation.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <poll.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace intezam {
namespace {

volatile std::sig_atomic_t interrupted = 0;
void interruptPlayback(int) { interrupted = 1; }

class PlaybackSignals {
public:
    PlaybackSignals()
    {
        interrupted = 0;
        interrupt_ = std::signal(SIGINT, interruptPlayback);
        terminate_ = std::signal(SIGTERM, interruptPlayback);
    }
    ~PlaybackSignals()
    {
        std::signal(SIGINT, interrupt_);
        std::signal(SIGTERM, terminate_);
    }
private:
    using Handler = void (*)(int);
    Handler interrupt_;
    Handler terminate_;
};

class PlaybackKeys {
public:
    PlaybackKeys()
    {
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_) == 0) {
            auto raw = original_;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            changed_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
    }
    ~PlaybackKeys() { if (changed_) tcsetattr(STDIN_FILENO, TCSANOW, &original_); }
    int next()
    {
        if (closed_) { std::this_thread::sleep_for(std::chrono::milliseconds{50}); return -1; }
        if (std::cin.rdbuf()->in_avail() <= 0) {
            pollfd input{STDIN_FILENO, POLLIN, 0};
            const int ready = poll(&input, 1, 50);
            if (ready < 0 && errno != EINTR) throw std::runtime_error("Cannot poll playback controls.");
            if (ready <= 0 || !(input.revents & (POLLIN | POLLHUP))) return -1;
        }
        const int key = std::cin.get();
        if (key == std::char_traits<char>::eof()) { closed_ = true; return -1; }
        return key;
    }
private:
    termios original_{};
    bool changed_ = false;
    bool closed_ = false;
};

} // namespace

void runVlcPlayback(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
                    const VideoFile& video, const PlaybackOptions& options)
{
    PlaybackSignals signals;
    PlaybackKeys keys;
    auto cache_options = options.cache;
    const auto external_cancel = cache_options.cancelled;
    cache_options.cancelled = [external_cancel] { return interrupted != 0 || (external_cancel && external_cancel()); };
    CliStatusRenderer monitor(std::cout, options.monitoring);
    monitor.event("Selected Video: " + video.path);
    monitor.event("Initializing VLC and bounded torrent cache...");
    monitor.event("Controls: P/Space pause, F forward 10s, B backward 10s, S stop, Q quit");
    cache_options.header_read_mode = true;
    if (options.preparation && !options.preparation->matches(video))
        throw std::invalid_argument("Prepared headers belong to a different selected video.");
    if (options.preparation) {
        while (!interrupted && !options.preparation->snapshot().source && !options.preparation->snapshot().finished)
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        if (interrupted) { options.preparation->cancel(); monitor.event("Playback cancelled."); return; }
    }
    auto source = options.preparation ? options.preparation->waitForSource()
        : std::make_shared<TorrentVideoSource>(magnet, std::move(metadata), video, cache_options);
    VlcPlayer player(source, options.vlc);
    auto preparation = std::async(std::launch::async, [source, options] {
        const auto cached_headers = options.preparation ? options.preparation->waitForHeaders() : std::optional<ContainerInfo>{};
        auto container = cached_headers ? *cached_headers : probeContainer(*source);
        const auto rate = options.cache.estimate_bitrate_from_headers
            ? initialVideoRate(source->size(), container, options.cache.playback_bytes_per_second)
            : options.cache.playback_bytes_per_second;
        source->finishHeaderPreparation(container, rate);
        source->prebuffer();
        return container;
    });
    bool started = false;
    bool decoded = false;
    auto started_at = std::chrono::steady_clock::now();
    auto displayed = std::chrono::steady_clock::time_point{};
    try {
        while (!interrupted) {
            if (!started && preparation.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
                const auto container = preparation.get();
                monitor.event("Container: " + container.name + (container.moov_at_end ? " (moov near EOF)" : ""));
                player.play();
                started = true;
                started_at = std::chrono::steady_clock::now();
            }
            const auto media = player.status();
            const auto cache = source->status();
            if (!cache.error.empty()) throw std::runtime_error(cache.error);
            if (!media.error.empty()) throw std::runtime_error(media.error);
            if (media.state == libvlc_Error) throw std::runtime_error("VLC playback failed; check installed demuxers/codecs and media validity.");
            if (!decoded && media.decoded_video_frames > 0) monitor.event("Playback started.");
            decoded = decoded || media.decoded_video_frames > 0;
            if (started && media.state == libvlc_Ended) {
                if (!decoded) throw std::runtime_error("VLC reached end without decoding video frames.");
                monitor.event("End of video.");
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (started && !decoded && now - started_at > options.cache.stall_timeout) {
                throw std::runtime_error("VLC startup timed out before any video frame was decoded.");
            }
            if (now - displayed >= std::chrono::milliseconds{700}) {
                monitor.update(media, cache, started ? "" : "Preparing playback");
                displayed = now;
            }
            const int key = keys.next();
            if (key == 'q' || key == 'Q' || key == 's' || key == 'S') break;
            if (started) {
                if (key == 'p' || key == 'P' || key == ' ') player.togglePause();
                else if (key == 'f' || key == 'F' || key == 'b' || key == 'B') {
                    if (media.duration_ms <= 0 || !media.seekable) monitor.event("Time seeking is not available for this stream yet.");
                    else {
                        const auto target = std::clamp<std::int64_t>(media.time_ms + ((key == 'f' || key == 'F') ? 10000 : -10000),
                            0, std::max<std::int64_t>(0, media.duration_ms - 1));
                        try { player.seekTo(target); }
                        catch (const std::exception& error) { monitor.event("Seek error: " + std::string(error.what())); }
                    }
                }
            }
        }
        if (options.preparation) options.preparation->cancel();
        player.stop();
        if (preparation.valid()) {
            try { preparation.get(); }
            catch (const ReadCancelled&) {}
            catch (...) { if (!interrupted) throw; }
        }
    } catch (...) {
        const auto failure = std::current_exception();
        if (options.preparation) options.preparation->cancel();
        player.stop();
        if (preparation.valid()) { try { preparation.get(); } catch (...) {} }
        std::rethrow_exception(failure);
    }
    monitor.event("Playback stopped. Temporary cache removed.");
}

} // namespace intezam
