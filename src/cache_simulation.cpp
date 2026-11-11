#include "cache_simulation.hpp"
#include "video_selection.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace intezam {
namespace {

volatile std::sig_atomic_t interrupted = 0;
void stopSimulation(int) { interrupted = 1; }

class SignalScope {
public:
    SignalScope() : interrupt_(std::signal(SIGINT, stopSimulation)), terminate_(std::signal(SIGTERM, stopSimulation))
    { interrupted = 0; }
    ~SignalScope()
    {
        std::signal(SIGINT, interrupt_);
        std::signal(SIGTERM, terminate_);
    }
private:
    using Handler = void (*)(int);
    Handler interrupt_;
    Handler terminate_;
};

void printDiagnostics(const TorrentCacheSession& cache, bool paused)
{
    const auto status = cache.diagnostics();
    std::cout << "\nMaximum Disk Cache: " << formatVideoSize(status.budget_bytes)
              << " (" << status.budget_bytes << " bytes)"
              << "\nCurrent Cache Usage (allocated blocks): " << status.allocated_bytes << " bytes"
              << "\nNetwork Speed (observed EWMA): " << formatVideoSize(static_cast<std::int64_t>(status.network_bytes_per_second)) << "/s"
              << "\nSimulated Video Bitrate (configured): " << formatVideoSize(static_cast<std::int64_t>(status.playback_bytes_per_second)) << "/s"
              << "\nPlayback Byte: " << status.playback_position
              << "\nPlayback Segment: S" << status.desired.current_segment
              << "\nTarget Window: S" << status.desired.first_segment << " - S" << status.desired.last_segment
              << " (" << status.desired.segmentCount() << " logical segments)"
              << "\nPositioning: approximately 1/3 behind, 2/3 ahead; clipped at file boundaries"
              << "\nVerified Segments (target): " << status.desired_verified_segments
              << "\nDownloading Segments: " << status.downloading_segments
              << "\nVerified Pieces (resident): " << status.verified_pieces
              << "\nDownloading / Queued / Deferred Pieces: " << status.downloading_pieces << " / "
              << status.queued_pieces << " / " << status.deferred_pieces
              << "\nForward Buffer: " << status.forward_buffer_seconds << " seconds"
              << "\nCache Status: " << (paused ? "PAUSED" : status.ready ? "PLAYING (simulated)" : "BUFFERING") << '\n'
              << std::flush;
}

bool inputReady()
{
    if (std::cin.rdbuf()->in_avail() > 0) return true;
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const auto result = poll(&input, 1, 0);
    if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot poll playback command input.");
    return result > 0 && (input.revents & (POLLIN | POLLHUP));
}

bool processCommand(TorrentCacheSession& cache, std::int64_t& position, double& rate,
                    bool& paused, const std::string& line)
{
    std::istringstream input(line);
    std::string command;
    input >> command;
    if (command == "q" || command == "quit") return false;
    if (command == "pause") paused = true;
    else if (command == "play") paused = false;
    else if (command == "status") printDiagnostics(cache, paused);
    else if (command == "seek" || command == "rate") {
        double value = 0;
        std::string extra;
        if (!(input >> value) || input >> extra) {
            std::cout << "Invalid command value.\n";
            return true;
        }
        try {
            if (command == "seek") {
                if (!std::isfinite(value) || value < 0 || value > 9e15 || std::floor(value) != value) {
                    throw std::invalid_argument("Seek requires a nonnegative whole byte offset.");
                }
                const bool hit = cache.seek(static_cast<std::int64_t>(value));
                position = static_cast<std::int64_t>(value);
                std::cout << (hit ? "Cache HIT: verified disk data retained.\n" : "Cache MISS: fetching the new playback window.\n");
            } else {
                cache.setPlaybackRate(value);
                rate = value;
            }
        } catch (const std::invalid_argument& error) {
            std::cout << "Invalid command: " << error.what() << '\n';
        } catch (const std::out_of_range& error) {
            std::cout << "Invalid command: " << error.what() << '\n';
        }
    } else if (!command.empty()) {
        std::cout << "Commands: seek BYTE, rate BYTES_PER_SECOND, pause, play, status, q\n";
    }
    return true;
}

} // namespace

void runCacheSimulation(const std::string& magnet, std::shared_ptr<const libtorrent::torrent_info> metadata,
                        const VideoFile& video, const CacheSimulationOptions& options)
{
    SignalScope signals;
    TorrentCacheSession cache(magnet, std::move(metadata), video, options.cache);
    std::cout << "\nSelected Video: " << video.path << "\nVideo Size: " << formatVideoSize(video.size_bytes)
              << "\nTemporary Cache: " << (cache.directory() / "stream.cache")
              << "\nSimulated playback; no codec parsing or VLC integration."
              << "\nCommands: seek BYTE, rate BYTES_PER_SECOND, pause, play, status, q\n";
    printDiagnostics(cache, false);
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto previous = started;
    auto displayed = started;
    std::int64_t position = 0;
    double rate = options.cache.playback_bytes_per_second;
    double credit = 0;
    bool paused = false;
    bool input_closed = false;
    while (!interrupted && position < video.size_bytes) {
        cache.pump(std::chrono::milliseconds{50});
        const auto now = Clock::now();
        if (options.duration.count() > 0 && now - started >= options.duration) break;
        if (!input_closed && inputReady()) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                input_closed = true;
                if (options.duration.count() == 0) break;
            } else {
                credit = 0;
                if (!processCommand(cache, position, rate, paused, line)) break;
            }
        }
        if (!paused && cache.isRangeCached(position, 1)) {
            credit += std::chrono::duration<double>(now - previous).count() * rate;
            while (credit >= 1 && position < video.size_bytes) {
                const auto length = static_cast<std::int64_t>(std::min<double>(
                    std::min<std::int64_t>(16384, video.size_bytes - position), credit));
                auto reader = cache.read(position, length);
                if (!reader) { credit = 0; break; }
                reader->read(); // exercise actual verified disk I/O, then release before advancing
                reader.reset();
                position += length;
                credit -= length;
                if (position < video.size_bytes) cache.setPlaybackPosition(position);
            }
        } else credit = 0;
        previous = now;
        if (now - displayed >= std::chrono::seconds{1}) {
            printDiagnostics(cache, paused);
            displayed = now;
        }
    }
    std::cout << "\nCache simulation ended. Temporary data is being removed.\n";
}

} // namespace intezam
