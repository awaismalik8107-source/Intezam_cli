#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <optional>
#include <string>

namespace intezam {
struct PlaybackStatus;
struct VideoSourceStatus;
enum class MonitorVerbosity { compact, detailed, debug };
struct CliStatusOptions {
    MonitorVerbosity verbosity = MonitorVerbosity::compact;
    std::filesystem::path log_file;
};
struct TerminalCapabilities {
    bool interactive = false;
    bool ansi = false;
    std::size_t columns = 80;
};

std::string formatCliStatus(const PlaybackStatus& player, const VideoSourceStatus& source,
    const std::string& phase, MonitorVerbosity verbosity, std::size_t columns);

// The playback owner calls update at 500–1000 ms intervals. All output from
// these renderers shares one console mutex, including permanent event writes.
class CliStatusRenderer {
public:
    explicit CliStatusRenderer(std::ostream& output, CliStatusOptions options = {},
        std::optional<TerminalCapabilities> terminal = {});
    ~CliStatusRenderer();
    CliStatusRenderer(const CliStatusRenderer&) = delete;
    CliStatusRenderer& operator=(const CliStatusRenderer&) = delete;
    void update(const PlaybackStatus& player, const VideoSourceStatus& source, const std::string& phase = {});
    void event(const std::string& message);
    void finish();
private:
    TerminalCapabilities terminal() const;
    void clearLine(const TerminalCapabilities& terminal);
    std::ostream& output_;
    CliStatusOptions options_;
    std::optional<TerminalCapabilities> terminal_override_;
    std::ofstream log_;
    std::string last_line_;
    bool visible_ = false;
};
}
