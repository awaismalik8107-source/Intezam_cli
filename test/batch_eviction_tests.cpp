#include "synthetic_media.hpp"

#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
using test_support::require;
namespace fs = std::filesystem;

namespace {
void waitFor(intezam::TorrentCacheSession& cache, std::int64_t offset)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!cache.isRangeCached(offset, 1024)) {
        cache.pump(10ms);
        require(std::chrono::steady_clock::now() < deadline, "batch test verified range timeout");
    }
}

void checksBatchReconstruction(const fs::path& media)
{
    test_support::LocalVideoTorrent torrent(media);
    auto options = torrent.options();
    options.playback_bytes_per_second = 512 * 1024;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    waitFor(cache, 0);
    const auto initial = cache.diagnostics();
    require(initial.performance.free_slots > 0, "normal scheduling preserves urgent reserve");
    auto pinned = cache.read(0, 1024);
    const auto expected = torrent.expected(0, 1024);
    std::atomic<bool> reading_active{true};
    auto reading = std::async(std::launch::async, [&] {
        while (reading_active.load()) {
            const auto bytes = pinned->read();
            require(std::memcmp(bytes.data(), expected.data(), bytes.size()) == 0, "VLC-style read remains valid during rebuilds");
            std::this_thread::yield();
        }
    });
    struct StopReader {
        std::atomic<bool>& running;
        ~StopReader() { running.store(false); }
    } stop_reader{reading_active};
    constexpr int movements = 160;
    for (int step = 1; step <= movements; ++step) {
        const auto offset = std::int64_t{step} * 16384;
        cache.updateReadRequest(offset, intezam::VideoRange{offset, 1024});
        waitFor(cache, offset);
        auto lease = cache.read(offset, 1024);
        const auto bytes = lease->read();
        const auto reference = torrent.expected(offset, bytes.size());
        require(std::memcmp(bytes.data(), reference.data(), bytes.size()) == 0, "batch preserves piece content");
        require(cache.isRangeCached(0, 1024), "active reader survives every reconstruction");
        require(cache.diagnostics().allocated_bytes <= cache.diagnostics().budget_bytes, "batch physical limit");
    }
    reading_active.store(false);
    reading.get();
    const auto after = cache.diagnostics();
    require(after.torrent_rebuilds > 0 && after.torrent_rebuilds < movements / 2, "batching reduces rebuilds below one per movement");
    require(after.performance.pieces_evicted > after.torrent_rebuilds, "each boundary reclaims a group");
    pinned.reset();
    const auto requests_before = torrent.counts.count(1);
    require(!cache.seek(16384), "cold backward seek misses");
    waitFor(cache, 16384);
    require(torrent.counts.count(1) > requests_before, "evicted availability is cleared and data downloads again");
    std::cout << movements << " movements used " << after.torrent_rebuilds << " batch rebuilds, reclaiming "
              << after.performance.pieces_evicted << " pieces.\n";
}

void checksBitrateAdaptiveReservation(const fs::path& media)
{
    test_support::LocalVideoTorrent torrent(media);
    auto options = torrent.options();
    std::atomic<std::size_t> verified_notifications{0};
    options.on_piece_verified = [&](int) { ++verified_notifications; };
    options.playback_bytes_per_second = 4 * 1024 * 1024;
    {
        intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
        require(cache.diagnostics().performance.reserved_slots == 0,
                "sub-second cache coverage prioritizes forward buffering");
        waitFor(cache, 0);
        require(verified_notifications.load() > 0, "verified data publishes a read notification");
        cache.setPlaybackRate(512 * 1024);
        require(cache.diagnostics().performance.reserved_slots > 0, "reserve returns when coverage exceeds 1.25 seconds");
        cache.setPlaybackRate(cache.dataCapacity() / 1.1);
        require(cache.diagnostics().performance.reserved_slots > 0, "reservation hysteresis holds above one second");
        cache.setPlaybackRate(4 * 1024 * 1024);
        require(cache.diagnostics().performance.reserved_slots == 0 && cache.isRangeCached(0, 1024),
                "pressure releases reserve without invalidating verified data");
        cache.setPlaybackRate(cache.dataCapacity() / 1.1);
        require(cache.diagnostics().performance.reserved_slots == 0, "reservation hysteresis waits for 1.25 seconds");
    }
    options.reserved_slots = 3;
    intezam::TorrentCacheSession configured(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    require(configured.diagnostics().performance.reserved_slots == 3, "explicit reservation overrides automatic adaptation");
}

void distinguishesDemuxMovementFromUserSeeks(const fs::path& media)
{
    test_support::LocalVideoTorrent torrent(media);
    auto options = torrent.options();
    options.playback_bytes_per_second = 512 * 1024;
    auto source = torrent.source(options);
    source->markPlaybackActive();
    const auto middle = source->size() / 2;
    const auto read_and_wait = [&](std::int64_t offset) {
        unsigned char bytes[1024];
        require(source->read(offset, bytes, sizeof(bytes)) > 0, "read after input reposition");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (source->status().cache.playback_position != offset) {
            require(std::chrono::steady_clock::now() < deadline, "source reposition publication");
            std::this_thread::sleep_for(2ms);
        }
        return source->status().cache;
    };
    source->seek(middle);
    const auto initial = read_and_wait(middle);
    for (int step = 0; step < 10; ++step) {
        const auto offset = middle + (step % 2 ? 4096 : -4096);
        source->seekForDemux(offset);
        require(read_and_wait(offset).desired.begin_byte == initial.desired.begin_byte,
                "demux byte oscillation retains rolling window");
    }
    source->seek(middle + 16384);
    require(read_and_wait(middle + 16384).desired.begin_byte != initial.desired.begin_byte,
            "genuine small user seek recenters immediately");
    intezam::ContainerInfo header;
    header.metadata_ranges.push_back({0, 4096});
    source->setContainer(header);
    source->seek(0);
    require(read_and_wait(0).desired.begin_byte == 0, "explicit seek to a known header still moves playback");
    source->stop();
}
} // namespace

int main()
{
    try {
        test_support::SyntheticMedia media;
        checksBatchReconstruction(media.path());
        checksBitrateAdaptiveReservation(media.path());
        distinguishesDemuxMovementFromUserSeeks(media.path());
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
