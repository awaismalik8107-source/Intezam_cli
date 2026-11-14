#include "vlc_playback.hpp"
#include "torrent_video_source.hpp"
#include "video_selection.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <future>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
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

std::string timestamp(std::int64_t milliseconds)
{
    const auto seconds = milliseconds / 1000;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
           << std::setw(2) << (seconds / 60) % 60 << ':' << std::setw(2) << seconds % 60;
    return output.str();
}

void displayPlayback(const VideoSourceStatus& source, const PlaybackStatus& player, bool prepared, std::int64_t video_size)
{
    const auto& cache = source.cache;
    std::cout << "\nStatus: " << (player.waiting_for_buffer ? "BUFFERING" : VlcPlayer::stateName(player.state))
              << (prepared ? "" : " (container/startup)")
              << "\nPosition: " << timestamp(player.time_ms) << " / " << timestamp(player.duration_ms)
              << "\nVLC Buffering: " << std::clamp(player.buffering_percent, 0.0f, 100.0f) << "%"
              << "\nNetwork Speed: " << formatVideoSize(static_cast<std::int64_t>(cache.download_bytes_per_second)) << "/s"
              << "\nCache Usage: " << cache.allocated_bytes << " / " << cache.budget_bytes << " bytes ("
              << std::fixed << std::setprecision(2) << cache.budget_bytes * 100.0 / video_size << "% of video)"
              << "\nFree / Reserved Slots: " << cache.performance.free_slots << " / " << cache.performance.reserved_slots
              << "\nTorrent Rebuilds: " << cache.torrent_rebuilds
              << "\nVerified Read Wait: " << source.performance.wait_microseconds / 1000 << " ms; stalls >=50ms: "
              << source.performance.stalls
              << "\nCached Window (target): S" << cache.desired.first_segment << " - S" << cache.desired.last_segment
              << "\nVerified / Downloading / Queued Pieces: " << cache.verified_pieces << " / "
              << cache.downloading_pieces << " / " << cache.queued_pieces
              << "\nForward Buffer (estimated): " << cache.forward_buffer_seconds << " seconds\n" << std::flush;
}

} // namespace

void runVlcPlayback(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
                    const VideoFile& video, const PlaybackOptions& options)
{
    PlaybackSignals signals;
    PlaybackKeys keys;
    auto cache_options = options.cache;
    const auto external_cancel = cache_options.cancelled;
    cache_options.cancelled = [external_cancel] { return interrupted != 0 || (external_cancel && external_cancel()); };
    std::cout << "\nSelected Video: " << video.path << "\nInitializing VLC and bounded torrent cache...\n"
              << "Controls: P pause/resume, F forward 10s, B backward 10s, S stop, Q quit\n" << std::flush;
    auto source = std::make_shared<TorrentVideoSource>(magnet, std::move(metadata), video, cache_options);
    VlcPlayer player(source, options.vlc);
    auto preparation = std::async(std::launch::async, [source] {
        auto container = probeContainer(*source);
        source->setContainer(container);
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
                std::cout << "Container: " << container.name << (container.moov_at_end ? " (moov near EOF)" : "") << '\n';
                player.play();
                started = true;
                started_at = std::chrono::steady_clock::now();
            }
            const auto media = player.status();
            const auto cache = source->status();
            if (!cache.error.empty()) throw std::runtime_error(cache.error);
            if (!media.error.empty()) throw std::runtime_error(media.error);
            if (media.state == libvlc_Error) throw std::runtime_error("VLC playback failed; check installed demuxers/codecs and media validity.");
            decoded = decoded || media.decoded_video_frames > 0;
            if (started && media.state == libvlc_Ended) {
                if (!decoded) throw std::runtime_error("VLC reached end without decoding video frames.");
                std::cout << "\nEnd of video.\n";
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (started && !decoded && now - started_at > options.cache.stall_timeout) {
                throw std::runtime_error("VLC startup timed out before any video frame was decoded.");
            }
            if (now - displayed >= std::chrono::seconds{1}) {
                displayPlayback(cache, media, started, video.size_bytes);
                displayed = now;
            }
            const int key = keys.next();
            if (key == 'q' || key == 'Q' || key == 's' || key == 'S') break;
            if (started) {
                if (key == 'p' || key == 'P') player.togglePause();
                else if (key == 'f' || key == 'F' || key == 'b' || key == 'B') {
                    if (media.duration_ms <= 0 || !media.seekable) std::cout << "Time seeking is not available for this stream yet.\n";
                    else {
                        const auto target = std::clamp<std::int64_t>(media.time_ms + ((key == 'f' || key == 'F') ? 10000 : -10000),
                            0, std::max<std::int64_t>(0, media.duration_ms - 1));
                        player.seekTo(target);
                    }
                }
            }
        }
        player.stop();
        if (preparation.valid()) {
            try { preparation.get(); }
            catch (const ReadCancelled&) {}
            catch (...) { if (!interrupted) throw; }
        }
    } catch (...) {
        const auto failure = std::current_exception();
        player.stop();
        if (preparation.valid()) { try { preparation.get(); } catch (...) {} }
        std::rethrow_exception(failure);
    }
    std::cout << "Playback stopped. Temporary cache removed.\n";
}

} // namespace intezam
