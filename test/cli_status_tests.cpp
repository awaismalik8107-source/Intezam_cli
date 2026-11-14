#include "cli_status_renderer.hpp"
#include "torrent_video_source.hpp"
#include "vlc_player.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

void formatsCurrentSnapshotsAndFitsNarrowTerminals()
{
    intezam::PlaybackStatus media;
    media.state = libvlc_Playing;
    media.time_ms = 765000;
    media.duration_ms = 6320000;
    intezam::VideoSourceStatus source;
    source.cache.download_bytes_per_second = 2.4 * 1048576;
    source.cache.network_bytes_per_second = 99 * 1048576;
    source.cache.forward_buffer_seconds = 28;
    source.cache.allocated_bytes = 45 * 1048576;
    source.cache.budget_bytes = 96 * 1048576;
    source.cache.connected_peers = 7;
    source.cache.verified_pieces = 12;
    source.cache.torrent_rebuilds = 3;
    const auto full = intezam::formatCliStatus(media, source, {}, intezam::MonitorVerbosity::compact, 120);
    require(full.find("PLAYING") != std::string::npos && full.find("12:45/1:45:20") != std::string::npos, "real playback state/time");
    require(full.find("DL 2.4 MiB/s") != std::string::npos, "current transfer rate instead of stale capacity estimate");
    require(full.find("Ahead ~28s") != std::string::npos && full.find("Cache 45/96 MiB") != std::string::npos, "honest estimated runway and physical allocation");
    const auto detailed = intezam::formatCliStatus(media, source, {}, intezam::MonitorVerbosity::detailed, 200);
    require(detailed.find("Peers 7") != std::string::npos && detailed.find("Rebuilds 3") != std::string::npos, "detailed mode");
    for (std::size_t width : {0, 1, 8, 20, 40, 60, 80, 120, 200}) {
        const auto line = intezam::formatCliStatus(media, source, {}, intezam::MonitorVerbosity::detailed, width);
        require(line.size() <= width, "formatted status cannot wrap");
    }
    media.user_paused = true;
    media.waiting_for_buffer = true;
    require(intezam::formatCliStatus(media, source, {}, intezam::MonitorVerbosity::compact, 80).find("PAUSED") != std::string::npos,
        "manual pause has precedence");
}

void separatesEventsFromTemporaryStatus()
{
    std::ostringstream output;
    {
        intezam::CliStatusRenderer renderer(output, {}, intezam::TerminalCapabilities{true, true, 90});
        renderer.update({}, {}, "Preparing playback");
        renderer.event("Playback started");
        renderer.update({}, {});
    }
    require(output.str().find("\r\033[2K") != std::string::npos, "TTY refresh clears in place");
    require(output.str().find("Playback started\n") != std::string::npos, "permanent event on its own line");
    require(output.str().back() == '\n', "shutdown returns cursor to a clean line");
    std::ostringstream redirected;
    {
        intezam::CliStatusRenderer renderer(redirected, {}, intezam::TerminalCapabilities{false, false, 80});
        for (int i = 0; i < 10; ++i) renderer.update({}, {});
        renderer.event("Playback stopped");
    }
    require(redirected.str() == "Playback stopped\n", "redirected compact output contains events only, without ANSI");
    std::ostringstream debug;
    intezam::CliStatusOptions debugging;
    debugging.verbosity = intezam::MonitorVerbosity::debug;
    {
        intezam::CliStatusRenderer renderer(debug, debugging, intezam::TerminalCapabilities{false, false, 80});
        renderer.update({}, {});
    }
    require(debug.str().find("Disk_us") != std::string::npos && debug.str().find('\033') == std::string::npos,
        "explicit debug mode provides readable redirected diagnostics");
    std::ostringstream dumb;
    {
        intezam::CliStatusRenderer renderer(dumb, {}, intezam::TerminalCapabilities{true, false, 40});
        renderer.update({}, {});
    }
    require(dumb.str().find('\033') == std::string::npos, "dumb terminals use carriage returns without ANSI");
}

void synchronizesEventsAndSanitizesControlCharacters()
{
    std::ostringstream output;
    intezam::CliStatusRenderer renderer(output, {}, intezam::TerminalCapabilities{false, false, 80});
    auto events = [&] { for (int i = 0; i < 100; ++i) renderer.event("event"); };
    std::thread first(events), second(events);
    first.join(); second.join();
    std::istringstream lines(output.str());
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) { require(line == "event", "threaded output does not interleave"); ++count; }
    require(count == 200, "all events retained");
    renderer.event("name\033[2J\nnext");
    require(output.str().find('\033') == std::string::npos, "untrusted events cannot inject terminal escapes");
}

void detectsRealTerminalWidthAndWritesExplicitDebugLogs()
{
    const auto* term = std::getenv("TERM");
    const std::string saved_term = term ? term : "";
    const int original = dup(STDOUT_FILENO);
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    require(original >= 0 && master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "open test PTY");
    const int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    require(slave >= 0, "open PTY slave");
    struct Restore {
        int original, master, slave;
        std::string term;
        bool existed;
        ~Restore() {
            dup2(original, STDOUT_FILENO); close(original); close(master); close(slave);
            if (existed) setenv("TERM", term.c_str(), 1); else unsetenv("TERM");
        }
    } restore{original, master, slave, saved_term, term != nullptr};
    require(dup2(slave, STDOUT_FILENO) >= 0, "attach PTY stdout");
    setenv("TERM", "xterm-256color", 1);
    winsize size{};
    size.ws_col = 100;
    require(ioctl(slave, TIOCSWINSZ, &size) == 0, "set initial terminal width");
    std::ostringstream output;
    {
        intezam::CliStatusRenderer renderer(output);
        renderer.update({}, {});
        size.ws_col = 20;
        require(ioctl(slave, TIOCSWINSZ, &size) == 0, "resize real terminal");
        renderer.update({}, {});
        const auto text = output.str();
        const auto position = text.rfind("\r\033[2K");
        require(position != std::string::npos && text.substr(position + 5).size() <= 19,
            "renderer re-detects TTY width on refresh and avoids last-column wrapping");
    }
    char pattern[] = "/tmp/intezam-monitor-XXXXXX";
    const int file = mkstemp(pattern);
    require(file >= 0, "create debug log fixture");
    close(file);
    struct Remove { const char* path; ~Remove() { unlink(path); } } remove{pattern};
    { std::ofstream seed(pattern); seed << "previous\n"; }
    std::ostringstream quiet;
    intezam::CliStatusOptions options;
    options.log_file = pattern;
    {
        intezam::CliStatusRenderer renderer(quiet, options, intezam::TerminalCapabilities{false, false, 80});
        renderer.update({}, {});
        renderer.event("hello");
    }
    std::ifstream log(pattern);
    const std::string contents{std::istreambuf_iterator<char>(log), {}};
    require(contents.find("previous\n") == 0 && contents.find("ReadWait_us") != std::string::npos,
        "explicit log appends detailed diagnostics without overwriting existing content");
    require(quiet.str() == "hello\n", "file diagnostics do not flood redirected compact output");
}
}
int main()
{
    try { formatsCurrentSnapshotsAndFitsNarrowTerminals(); separatesEventsFromTemporaryStatus();
        synchronizesEventsAndSanitizesControlCharacters(); detectsRealTerminalWidthAndWritesExplicitDebugLogs(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
