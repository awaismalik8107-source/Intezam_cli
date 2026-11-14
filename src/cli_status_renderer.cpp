#include "cli_status_renderer.hpp"
#include "torrent_video_source.hpp"
#include "vlc_player.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace intezam {
namespace {
std::mutex console_mutex;

std::string safeEvent(const std::string& text)
{
    auto result = text;
    for (auto& character : result) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 32 || value == 127) character = ' ';
    }
    return result;
}

std::string timeText(std::int64_t milliseconds)
{
    const auto seconds = std::max<std::int64_t>(0, milliseconds / 1000);
    std::ostringstream result;
    if (seconds >= 3600) result << seconds / 3600 << ':' << std::setfill('0') << std::setw(2);
    result << seconds / 60 % 60 << ':' << std::setfill('0') << std::setw(2) << seconds % 60;
    return result.str();
}

std::string number(double value, int decimals)
{
    std::ostringstream result;
    result << std::fixed << std::setprecision(decimals) << std::max(0.0, value);
    return result.str();
}

std::string stateText(const PlaybackStatus& player, const VideoSourceStatus& source, const std::string& phase)
{
    if (!player.error.empty() || !source.error.empty()) return "ERROR";
    if (player.user_paused) return "PAUSED";
    if (player.waiting_for_buffer || player.state == libvlc_Buffering) return "BUFFERING";
    if (phase.find("ended") != std::string::npos) return "ENDED";
    if (phase.find("stopped") != std::string::npos) return "STOPPED";
    if (!phase.empty()) return "PREPARING";
    return VlcPlayer::stateName(player.state);
}

std::string debugDetails(const VideoSourceStatus& source)
{
    const auto& cache = source.cache;
    return " | Queued " + std::to_string(cache.queued_pieces)
        + " | Desired " + std::to_string(cache.desired_capacity_bytes)
        + " | Physical " + std::to_string(cache.physical_capacity_bytes)
        + " | ReadWait_us " + std::to_string(source.performance.wait_microseconds)
        + " | Rebuild_us " + std::to_string(cache.performance.rebuild_microseconds)
        + " | Disk_us " + std::to_string(cache.performance.disk_microseconds);
}
}

std::string formatCliStatus(const PlaybackStatus& player, const VideoSourceStatus& source,
    const std::string& phase, MonitorVerbosity verbosity, std::size_t columns)
{
    const auto state = stateText(player, source, phase);
    const auto time = timeText(player.time_ms) + '/' + (player.duration_ms > 0 ? timeText(player.duration_ms) : "--:--");
    std::string line = "INTEZAM | " + state + " | " + time;
    if (line.size() > columns) line = state + ' ' + time;
    if (line.size() > columns) return columns > 3 ? line.substr(0, columns - 3) + "..." : line.substr(0, columns);
    const auto append = [&](const std::string& field) {
        if (line.size() + field.size() + 3 <= columns) line += " | " + field;
    };
    const auto& cache = source.cache;
    append("DL " + number(cache.download_bytes_per_second / 1048576.0, 1) + " MiB/s");
    append("Ahead ~" + number(cache.forward_buffer_seconds, 0) + 's');
    const auto mib = [](std::int64_t bytes) { const auto value = bytes / 1048576.0; return number(value, value < 10 ? 1 : 0); };
    append("Cache " + mib(cache.allocated_bytes) + '/' + mib(cache.budget_bytes) + " MiB");
    if (verbosity != MonitorVerbosity::compact) {
        append("Peers " + std::to_string(cache.connected_peers));
        append("Verified " + std::to_string(cache.verified_pieces));
        append("Rebuilds " + std::to_string(cache.torrent_rebuilds));
    }
    return line;
}

CliStatusRenderer::CliStatusRenderer(std::ostream& output, CliStatusOptions options,
    std::optional<TerminalCapabilities> terminal)
    : output_(output), options_(std::move(options)), terminal_override_(terminal)
{
    if (!options_.log_file.empty()) {
        log_.open(options_.log_file, std::ios::app);
        if (!log_) throw std::runtime_error("Cannot open monitoring log: " + options_.log_file.string());
    }
}

CliStatusRenderer::~CliStatusRenderer() { try { finish(); } catch (...) {} }

TerminalCapabilities CliStatusRenderer::terminal() const
{
    if (terminal_override_) return *terminal_override_;
    TerminalCapabilities result;
    result.interactive = isatty(STDOUT_FILENO);
    const auto* term = std::getenv("TERM");
    result.ansi = result.interactive && term && *term && std::string(term) != "dumb";
    winsize dimensions{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &dimensions) == 0 && dimensions.ws_col) result.columns = dimensions.ws_col;
    return result;
}

void CliStatusRenderer::clearLine(const TerminalCapabilities& capabilities)
{
    if (!visible_) return;
    if (capabilities.ansi) output_ << "\r\033[2K";
    else output_ << '\r' << std::string(std::min(last_line_.size(), capabilities.columns > 0 ? capabilities.columns - 1 : 0), ' ') << '\r';
    visible_ = false;
}

void CliStatusRenderer::update(const PlaybackStatus& player, const VideoSourceStatus& source, const std::string& phase)
{
    const std::lock_guard<std::mutex> lock(console_mutex);
    const auto capabilities = terminal();
    if (log_) {
        log_ << formatCliStatus(player, source, phase, MonitorVerbosity::detailed, 4096) << debugDetails(source) << '\n';
        log_.flush();
    }
    if (!capabilities.interactive) {
        if (options_.verbosity == MonitorVerbosity::debug) {
            output_ << formatCliStatus(player, source, phase, MonitorVerbosity::detailed, 4096) << debugDetails(source) << '\n' << std::flush;
        }
        return;
    }
    if (!capabilities.ansi) clearLine(capabilities);
    last_line_ = formatCliStatus(player, source, phase, options_.verbosity, capabilities.columns > 0 ? capabilities.columns - 1 : 0);
    if (capabilities.ansi) output_ << "\r\033[2K";
    else output_ << '\r';
    output_ << last_line_ << std::flush;
    visible_ = true;
}

void CliStatusRenderer::event(const std::string& message)
{
    const std::lock_guard<std::mutex> lock(console_mutex);
    clearLine(terminal());
    const auto text = safeEvent(message);
    output_ << text << '\n' << std::flush;
    if (log_) { log_ << "EVENT " << text << '\n'; log_.flush(); }
}

void CliStatusRenderer::finish()
{
    const std::lock_guard<std::mutex> lock(console_mutex);
    if (!visible_) return;
    output_ << '\n' << std::flush;
    visible_ = false;
}
}
