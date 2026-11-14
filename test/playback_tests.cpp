#include "local_video_torrent.hpp"
#include "vlc_player.hpp"

#include <chrono>
#include <future>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;

using namespace std::chrono_literals;
using test_support::require;
namespace fs = std::filesystem;

namespace {

void testsCli(const fs::path& executable, const fs::path& file)
{
    test_support::LocalVideoTorrent torrent(file);
    auto pattern = (fs::current_path() / "intezam-cli-test-XXXXXX").string();
    require(mkdtemp(pattern.data()) != nullptr, "CLI test directory");
    const fs::path root = pattern;
    const auto cache = root / "cache" / "intezam";
    const auto log = root / "output.log";
    int input[2];
    if (pipe(input) != 0) { fs::remove_all(root); throw std::runtime_error("CLI input pipe"); }
    pid_t child = -1;
    bool input_open = true;
    try {
        std::vector<std::string> environment;
        for (auto** entry = environ; *entry; ++entry) {
            if (std::string(*entry).find("XDG_CACHE_HOME=") != 0) environment.emplace_back(*entry);
        }
        environment.push_back("XDG_CACHE_HOME=" + (root / "cache").string());
        std::vector<char*> env;
        for (auto& entry : environment) env.push_back(entry.data());
        env.push_back(nullptr);
        auto program = executable.string();
        char headless[] = "--headless", timeout[] = "--timeout", seconds[] = "20";
        char* arguments[]{program.data(), headless, timeout, seconds, nullptr};
        posix_spawn_file_actions_t actions;
        require(posix_spawn_file_actions_init(&actions) == 0, "CLI spawn actions");
        int result = posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, input[1]);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, input[0]);
        if (!result) result = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
            log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!result) result = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!result) result = posix_spawn(&child, program.c_str(), &actions, nullptr, arguments, env.data());
        posix_spawn_file_actions_destroy(&actions);
        require(result == 0, "spawn complete CLI");
        close(input[0]);
        input[0] = -1;
        const auto selection = torrent.magnet() + "\n1\n";
        require(write(input[1], selection.data(), selection.size()) == static_cast<ssize_t>(selection.size()), "CLI magnet and selection");
        close(input[1]);
        input_open = false;
        const auto deadline = std::chrono::steady_clock::now() + 35s;
        std::int64_t peak = 0;
        int exit_status = 0;
        for (;;) {
            const auto exited = waitpid(child, &exit_status, WNOHANG);
            require(exited >= 0, "wait for CLI");
            if (exited == child) { child = -1; break; }
            std::int64_t allocated = 0;
            const auto measure = [&](const fs::path& path) {
                struct stat status{};
                if (lstat(path.c_str(), &status) == 0) allocated += status.st_blocks * 512LL;
            };
            measure(cache);
            if (fs::exists(cache)) {
                std::error_code error;
                for (fs::recursive_directory_iterator entry(cache, error), end; entry != end; entry.increment(error)) {
                    require(!error, "inspect CLI cache");
                    measure(entry->path());
                }
            }
            peak = std::max(peak, allocated);
            require(peak <= torrent.video().size_bytes / 40, "CLI actual disk bound");
            require(std::chrono::steady_clock::now() < deadline, "CLI default transport playback timeout; see " + log.string());
            std::this_thread::sleep_for(10ms);
        }
        std::ifstream output(log);
        const std::string text{std::istreambuf_iterator<char>{output}, {}};
        require(WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0, "CLI exit: " + text);
        require(text.find("moov near EOF") != std::string::npos && text.find("End of video.") != std::string::npos,
                "complete magnet/selection/container/decode/EOF flow: " + text);
        require(fs::exists(cache) && fs::is_empty(cache), "CLI removed session directory");
        std::cout << "CLI magnet-to-EOF passed; peak allocation=" << peak << "/" << torrent.video().size_bytes / 40 << " bytes\n";
        fs::remove_all(root);
    } catch (...) {
        if (input[0] >= 0) close(input[0]);
        if (input_open) close(input[1]);
        if (child > 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        fs::remove_all(root);
        throw;
    }
}

template<class Predicate>
void waitUntil(Predicate complete, const char* message, std::chrono::seconds timeout = 30s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!complete()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(20ms);
    }
}

void checkDisk(const intezam::TorrentVideoSource& source)
{
    const auto directory = source.directory();
    std::int64_t allocated = 0;
    for (const auto& path : {directory.parent_path(), directory, directory / "session.lock", directory / "stream.cache"}) {
        struct stat status{};
        require(lstat(path.c_str(), &status) == 0, "stat actual managed disk allocation");
        allocated += status.st_blocks * 512LL;
    }
    require(allocated <= source.size() / 40, "actual managed allocation <= 2.5 percent");
    for (const auto& entry : fs::directory_iterator(directory)) {
        require(entry.path().filename() == "session.lock" || entry.path().filename() == "stream.cache",
                "no full-size payload file in client cache");
    }
}

void testsBridge(const fs::path& file)
{
    test_support::LocalVideoTorrent torrent(file);
    auto source = torrent.source();
    source->markPlaybackActive();
    std::vector<unsigned char> bytes(16384);
    const auto length = source->read(0, bytes.data(), bytes.size());
    require(std::equal(bytes.begin(), bytes.begin() + length, torrent.expected(0, length).begin()), "verified bytes match fixture");
    const int first_piece = static_cast<int>(torrent.video().torrent_offset / 16384);
    const auto requests = torrent.counts.count(first_piece);
    source->seek(0);
    require(source->read(0, bytes.data(), length) == length, "cached seek returns disk bytes");
    require(torrent.counts.count(first_piece) == requests, "cached seek does not redownload its piece");
    source->beginPlaybackEpoch();
    for (int repeated = 0; repeated < 1000; ++repeated) source->read(0, bytes.data(), length);
    source->markPlaybackActive();

    torrent.seed.pause();
    std::this_thread::sleep_for(50ms);
    const auto offset = source->size() / 2;
    auto waiting = std::async(std::launch::async, [&] { return source->read(offset, bytes.data(), bytes.size()); });
    waitUntil([&] { return source->status().waiting_reads != 0; }, "read entered buffering");
    require(waiting.wait_for(200ms) == std::future_status::timeout, "network interruption blocks rather than false EOF");
    torrent.seed.resume();
    if (waiting.wait_for(10s) != std::future_status::ready) {
        const auto state = source->status();
        throw std::runtime_error("read resumes when network returns: " + state.error
            + " received=" + std::to_string(state.cache.received_bytes)
            + " queued=" + std::to_string(state.cache.queued_pieces));
    }
    const auto resumed = waiting.get();
    require(resumed > 0 && std::equal(bytes.begin(), bytes.begin() + resumed, torrent.expected(offset, resumed).begin()), "uncached seek verified bytes");
    require(source->read(source->size(), bytes.data(), bytes.size()) == 0, "true EOF");

    torrent.seed.set_upload_limit(32768);
    const auto slow_offset = source->size() / 4;
    auto slow = std::async(std::launch::async, [&] { return source->read(slow_offset, bytes.data(), bytes.size()); });
    waitUntil([&] { return source->status().waiting_reads != 0; }, "throttled read buffering");
    require(slow.wait_for(100ms) == std::future_status::timeout, "low throughput waits for a complete verified piece");
    torrent.seed.set_upload_limit(0);
    require(slow.wait_for(10s) == std::future_status::ready && slow.get() > 0, "throttled read recovers");

    torrent.seed.pause();
    std::this_thread::sleep_for(50ms);
    waiting = std::async(std::launch::async, [&] {
        try { source->read(source->size() * 3 / 4, bytes.data(), bytes.size()); }
        catch (const intezam::ReadCancelled&) { return std::size_t{0}; }
        throw std::runtime_error("pending read was not cancelled");
    });
    waitUntil([&] { return source->status().waiting_reads != 0; }, "second pending read");
    source->cancelPendingReads();
    require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == 0, "seek cancellation wakes blocked read");
    source->resumeReads();
    checkDisk(*source);
    const auto directory = source->directory();
    source->stop();
    require(!fs::exists(directory), "bridge cleanup");
    std::cout << "Bridge: cache hit/miss, interruption, cancellation, EOF and disk bound passed.\n";
}

void stopsActiveDownload(const fs::path& file)
{
    test_support::LocalVideoTorrent torrent(file);
    torrent.seed.set_upload_limit(16384);
    auto source = torrent.source();
    const auto directory = source->directory();
    intezam::VlcPlayer player(source, {true, 100});
    player.play();
    waitUntil([&] { return source->status().waiting_reads > 0; }, "VLC callback waiting on an active download");
    const auto started = std::chrono::steady_clock::now();
    player.stop();
    require(std::chrono::steady_clock::now() - started < 2s, "VLC stop cancels blocked callback without deadlock");
    require(!fs::exists(directory), "stop during download removes temporary cache");
    std::cout << "VLC stop during a blocked download callback passed.\n";
}

void playsContainer(const fs::path& file, bool controls)
{
    std::cout << "Playing " << file.filename() << std::endl;
    test_support::LocalVideoTorrent torrent(file);
    auto source = torrent.source();
    const auto container = intezam::probeContainer(*source);
    source->setContainer(container);
    source->prebuffer();
    const auto directory = source->directory();
    intezam::VlcPlayer player(source, {true, 100});
    player.play();
    int decoded = 0;
    int audio = 0;
    auto reported = std::chrono::steady_clock::now();
    const auto status = [&] {
        auto current = player.status();
        const auto cache = source->status();
        require(current.error.empty(), "VLC callback failure: " + current.error);
        require(cache.error.empty(), "cache failure: " + cache.error);
        require(current.state != libvlc_Error, "VLC reported a playback error");
        decoded = std::max(decoded, current.decoded_video_frames);
        audio = std::max(audio, current.decoded_audio_frames);
        if (std::chrono::steady_clock::now() - reported > 2s) {
            std::cout << "  " << intezam::VlcPlayer::stateName(current.state) << " time=" << current.time_ms
                      << " decoded=" << current.decoded_video_frames << " read=" << current.bytes_read
                      << " waiting=" << cache.waiting_reads << " received=" << cache.cache.received_bytes << std::endl;
            reported = std::chrono::steady_clock::now();
        }
        checkDisk(*source);
        return current;
    };
    waitUntil([&] { return status().decoded_video_frames > 1; }, "VLC decoded initial frames");
    if (controls) {
        player.pause();
        waitUntil([&] { return status().state == libvlc_Paused; }, "VLC pause");
        player.resume();
        waitUntil([&] { return status().state == libvlc_Playing; }, "VLC resume");
        waitUntil([&] { return status().duration_ms > 2000; }, "VLC duration available");
        player.seekTo(1500);
        waitUntil([&] { auto s = status(); return s.time_ms >= 1400 && s.decoded_video_frames > 0; }, "VLC forward time seek");
        player.seekTo(300);
        waitUntil([&] { auto s = status(); return s.time_ms >= 250 && s.decoded_video_frames > 0; }, "VLC backward time seek");
    }
    waitUntil([&] { return status().state == libvlc_Ended; }, "VLC reached end of media", 60s);
    require(decoded > 1, "real decoded frames, not just a PLAYING event");
    require(audio > 0, "audio packets were actually decoded");
    const auto cache = source->status().cache;
    std::cout << container.name << (container.moov_at_end ? " (tail moov)" : "")
              << ": decoded video=" << decoded << ", audio=" << audio << ", allocated=" << cache.allocated_bytes
              << "/" << cache.budget_bytes << " bytes\n";
    player.stop();
    require(!fs::exists(directory), "VLC stop removes managed cache");
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        require(argc >= 2, "provide generated fixture paths");
        if (std::string(argv[1]) == "--cli") {
            require(argc == 4, "provide CLI executable and media fixture");
            testsCli(argv[2], argv[3]);
            return 0;
        }
        int first = 1;
        if (std::string(argv[1]) == "--play-only") { first = 2; require(argc > 2, "fixture required"); }
        else { testsBridge(argv[1]); stopsActiveDownload(argv[1]); }
        for (int index = first; index < argc; ++index) playsContainer(argv[index], index == first);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
