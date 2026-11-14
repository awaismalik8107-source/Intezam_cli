#include "local_video_torrent.hpp"
#include "playback_preparation.hpp"
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
using test_support::require;

class WideHeaderFixture {
public:
    WideHeaderFixture()
    {
        auto pattern = (std::filesystem::current_path() / "intezam-wide-header-XXXXXX").string();
        const auto directory = mkdtemp(pattern.data());
        require(directory != nullptr, "wide header fixture directory");
        root_ = directory;
        path = root_ / "wide.mp4";
        std::ofstream output(path, std::ios::binary);
        output << std::string(1024 * 1024, '\0');
        const auto atom = [&](std::int64_t offset, std::uint32_t length, const char* type) {
            const char header[]{static_cast<char>(length >> 24), static_cast<char>(length >> 16),
                static_cast<char>(length >> 8), static_cast<char>(length), type[0], type[1], type[2], type[3]};
            output.seekp(offset); output.write(header, sizeof(header));
        };
        atom(0, 24, "ftyp"); atom(24, 1024 * 1024 - 24, "moov");
        for (int child = 0; child < 6; ++child) atom(32 + child * 12000, 12000, "free");
    }
    ~WideHeaderFixture() { std::error_code error; std::filesystem::remove_all(root_, error); }
    std::filesystem::path path;
private:
    std::filesystem::path root_;
};

template<class Predicate>
void waitFor(Predicate ready, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!ready()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(10ms);
    }
}

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "media fixture required");
        test_support::LocalVideoTorrent torrent(argv[1]);
        std::filesystem::path directory;
        {
            intezam::PlaybackPreparation preparation(torrent.magnet(), torrent.metadata(), torrent.video(), torrent.options());
            const auto source = preparation.waitForSource();
            directory = source->directory();
            const auto headers = preparation.waitForHeaders();
            require(headers && headers->duration_seconds, "background preparation probes duration");
            require(preparation.matches(torrent.video()), "candidate remains a manual selection");
            auto different_video = torrent.video();
            different_video.path = "another-video.mkv";
            require(!preparation.matches(different_video), "a different manual choice cannot adopt the candidate's source");
            std::this_thread::sleep_for(150ms);
            {
                std::lock_guard<std::mutex> lock(torrent.counts.mutex);
                require(!torrent.counts.pieces.empty() && torrent.counts.pieces.size() <= 4,
                    "preselection downloads only bounded header pieces, not the rolling video window");
            }
            require(source->status().cache.header_read_mode, "idle selection keeps distant prefetch disabled");
            const int first = static_cast<int>(torrent.video().torrent_offset / 16384);
            const auto requests = torrent.counts.count(first);
            source->finishHeaderPreparation(*headers, intezam::initialVideoRate(source->size(), *headers, 1048576));
            source->prebuffer();
            require(source->directory() == directory && preparation.snapshot().source == source,
                "selection adopts the same cache source and session");
            require(torrent.counts.count(first) == requests, "selection reuses verified header pieces without downloading again");
            waitFor([&] { return !source->status().cache.header_read_mode; }, "normal lookahead resumes after headers");
            require(source->status().startup_buffer_target_bytes > 0, "startup exposes its authoritative byte target");
        }
        require(!std::filesystem::exists(directory), "cancel/release removes speculative cache");

        {
            WideHeaderFixture fixture;
            test_support::LocalVideoTorrent wide(fixture.path);
            intezam::PlaybackPreparation preparation(wide.magnet(), wide.metadata(), wide.video(), wide.options());
            const auto source = preparation.waitForSource();
            require(!preparation.waitForHeaders() && preparation.snapshot().error.empty(),
                "reaching the speculative piece quota pauses preparation without failing playback");
            {
                std::lock_guard<std::mutex> lock(wide.counts.mutex);
                require(wide.counts.pieces.size() <= 4, "a sparse header scan cannot exceed the preselection piece allowance");
            }
            require(intezam::probeContainer(*source).type == intezam::ContainerType::mp4,
                "selection can finish a quota-limited probe using the same source");
        }

        torrent.seed.pause();
        {
            intezam::PlaybackPreparation preparation(torrent.magnet(), torrent.metadata(), torrent.video(), torrent.options());
            const auto source = preparation.waitForSource();
            directory = source->directory();
            waitFor([&] { return source->status().waiting_reads > 0; }, "blocked speculative header read");
            preparation.cancel();
        }
        require(!std::filesystem::exists(directory), "changing/cancelling selection wakes blocked prefetch and cleans up");
        {
            auto idle_options = torrent.options();
            idle_options.header_read_mode = true;
            idle_options.stall_timeout = 50ms;
            intezam::TorrentCacheSession idle(torrent.magnet(), torrent.metadata(), torrent.video(), idle_options);
            std::this_thread::sleep_for(80ms);
            idle.pump();
            require(idle.diagnostics().header_read_mode, "an idle selection does not trigger a playback starvation timeout");
        }
        std::cout << "Bounded selection prefetch, source reuse, bitrate bootstrap and cleanup passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
