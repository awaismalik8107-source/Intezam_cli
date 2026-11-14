#include "synthetic_media.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
#include <cstring>

using namespace std::chrono_literals;
using test_support::require;
namespace fs = std::filesystem;
constexpr std::int64_t MiB = 1024 * 1024;

void expandsWithoutMovingPinnedOrWrittenData()
{
    const auto root = intezam::CacheFile::defaultRoot() / ("growth-test-" + std::to_string(getpid()));
    intezam::CacheBudgetOptions budget{intezam::CacheBudgetMode::adaptive, 25};
    budget.initial_consumption_bytes_per_second = 32 * 1024;
    budget.adaptive.maximum_bytes = 16 * MiB;
    {
        const intezam::PieceGeometry geometry{512 * MiB, 0, 512 * MiB, int(MiB)};
        auto store = intezam::PieceStore::create(geometry, root, {}, budget);
        const auto original_slots = store->slotCount();
        require(original_slots < 12, "growth fixture begins below target");
        store->setProtectedPieces({0, 1});
        store->admit({0, 1}, {});
        std::vector<char> content(MiB, 'v');
        store->write(0, 0, content.data(), content.size());
        store->markVerified(0);
        auto lease = store->acquire(0, 16384);
        std::atomic<bool> running{true};
        auto reader = std::async(std::launch::async, [&] {
            while (running.load()) require(lease->read() == std::vector<char>(16384, 'v'), "active lease during growth");
        });
        struct Stop { std::atomic<bool>& running; ~Stop() { running = false; } } stop{running};
        auto writer = std::async(std::launch::async, [&] {
            for (int offset = 0; offset < MiB; offset += 16384) store->write(1, offset, content.data(), 16384);
        });
        require(store->growToSlots(12), "append expansion succeeds");
        writer.get();
        running = false;
        reader.get();
        require(store->slotCount() == 12 && store->isVerified(0), "expansion preserves verification and slot offsets");
        require(store->readRaw(1, 0, int(MiB)) == content, "concurrent disk writes preserved");
        const auto before = store->allocatedBytes();
        require(!store->growToSlots(20), "absolute cap rejects expansion");
        require(store->allocatedBytes() == before && store->slotCount() == 12, "rejected growth publishes no slots");
        require(store->growToSlots(4) && store->slotCount() == 12, "shrink is safely deferred");
        require(store->reclaimOutside({}).empty(), "pinned/protected pieces cannot be reclaimed");
        require(store->allocatedBytes() <= budget.adaptive.maximum_bytes, "actual block allocation within cap");
    }
    require(fs::is_empty(root), "growth session cleaned up");
    fs::remove(root);
}

void pumpUntil(intezam::TorrentCacheSession& cache, const std::function<bool()>& ready)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!ready()) {
        cache.pump(10ms);
        require(std::chrono::steady_clock::now() < deadline, "adaptive torrent test timeout");
    }
}

void adaptsDuringTorrentIoAndRapidSeeks()
{
    test_support::SyntheticMedia media(64);
    test_support::LocalVideoTorrent torrent(media.path(), 256 * 1024);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.budget.adaptive.maximum_bytes = 24 * MiB;
    options.budget.adaptive.growth_interval_seconds = 1;
    options.budget.adaptive.shrink_interval_seconds = 2;
    options.playback_bytes_per_second = 64 * 1024;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    pumpUntil(cache, [&] { return cache.isRangeCached(0, 16384); });
    auto pinned = cache.read(0, 16384);
    const auto before = cache.diagnostics();
    const auto expected = torrent.expected(0, 16384);
    const auto tail = torrent.video().size_bytes - 16384;
    cache.setProtectedMetadata({{tail, 16384}});
    pumpUntil(cache, [&] { return cache.isRangeCached(tail, 16384); });
    cache.setPlaybackRate(2 * MiB);
    pumpUntil(cache, [&] { return cache.diagnostics().physical_capacity_bytes > before.physical_capacity_bytes; });
    const auto grown = cache.diagnostics();
    const auto retained = pinned->read();
    require(std::memcmp(retained.data(), expected.data(), expected.size()) == 0, "pinned verified bytes survive torrent growth");
    require(grown.allocated_bytes <= options.budget.adaptive.maximum_bytes, "torrent growth absolute bound");
    // Coalesced user seeks must immediately move the policy without oscillatory
    // capacity growth. Complete the last requested range in each direction.
    for (auto destination : {12 * MiB, 13 * MiB, 12 * MiB, 30 * MiB, 2 * MiB}) cache.seek(destination);
    pumpUntil(cache, [&] { return cache.isRangeCached(2 * MiB, 16384); });
    cache.seek(30 * MiB);
    pumpUntil(cache, [&] { return cache.isRangeCached(30 * MiB, 16384); });
    cache.seek(2 * MiB);
    pumpUntil(cache, [&] { return cache.isRangeCached(2 * MiB, 16384); });
    require(cache.isRangeCached(tail, 16384), "container metadata remains protected through forward/backward seeks");
    const auto state = cache.diagnostics();
    require(state.desired.pieces.size() <= state.performance.total_slots, "logical window fits physical slots");
    require(state.useful_throughput.peers >= 0, "peer diagnostics available");
    cache.setPlaybackRate(32 * 1024);
    pumpUntil(cache, [&] { return cache.diagnostics().shrink_deferred; });
    require(cache.diagnostics().physical_capacity_bytes == grown.physical_capacity_bytes,
        "smaller logical target never truncates live data");
    const auto after = pinned->read();
    require(std::memcmp(after.data(), expected.data(), expected.size()) == 0, "old lease remains valid after shrink request");
    require(cache.diagnostics().torrent_rebuilds < 12, "rapid seeks avoid rebuilding on every scheduling update");
}

void keepsPeersAfterTheCurrentWindowFinishes()
{
    test_support::SyntheticMedia media(64);
    test_support::LocalVideoTorrent torrent(media.path(), 256 * 1024);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.budget.adaptive.maximum_bytes = 8 * MiB;
    options.playback_bytes_per_second = 256 * 1024;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    pumpUntil(cache, [&] { const auto s = cache.diagnostics(); return s.queued_pieces == 0 && s.downloading_pieces == 0; });
    const auto end = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < end) cache.pump(10ms);
    const auto state = cache.diagnostics();
    require(state.desired.end_byte < torrent.video().size_bytes, "only a rolling subset has completed");
    require(state.torrent_finished && state.connected_peers > 0, "completed rolling window retains useful seed connection");
}

void fitsProtectedMetadataWithinATinyAbsoluteBudget()
{
    constexpr int piece = 256 * 1024;
    test_support::SyntheticMedia media(16);
    test_support::LocalVideoTorrent torrent(media.path(), piece);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.budget.adaptive.maximum_bytes = 4 * piece + 65536;
    options.playback_bytes_per_second = 64 * 1024;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    require(cache.diagnostics().performance.total_slots == 4, "four-slot adaptive hard cap");
    const auto first = 8LL * piece - torrent.video().torrent_offset;
    const auto second = 10LL * piece - torrent.video().torrent_offset;
    cache.setProtectedMetadata({{first, piece}, {second, piece}});
    pumpUntil(cache, [&] { return cache.isRangeCached(first, piece) && cache.isRangeCached(second, piece) && cache.isRangeCached(0, 1024); });
    require(cache.diagnostics().allocated_bytes <= options.budget.adaptive.maximum_bytes,
        "two metadata pieces and current playback fit without expanding the cap");
    cache.setProtectedMetadata({{0, 3LL * piece - torrent.video().torrent_offset}});
    pumpUntil(cache, [&] { return cache.isRangeCached(0, 3LL * piece - torrent.video().torrent_offset); });
    require(cache.diagnostics().performance.total_slots == 4,
        "metadata overlapping current playback is not counted twice in the hard-cap preflight");
}

void growsImmediatelyForEssentialMetadata()
{
    constexpr int piece = 256 * 1024;
    test_support::SyntheticMedia media(16);
    test_support::LocalVideoTorrent torrent(media.path(), piece);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.budget.adaptive.maximum_bytes = 8 * MiB;
    options.playback_bytes_per_second = 8 * 1024;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    const auto begin = 8LL * piece - torrent.video().torrent_offset;
    cache.setProtectedMetadata({{begin, 6LL * piece}});
    pumpUntil(cache, [&] { return cache.isRangeCached(begin, 6LL * piece) && cache.isRangeCached(0, 1024); });
    require(cache.diagnostics().performance.protected_slots >= 7,
        "essential metadata can grow the logical target before the normal resize cooldown");
}

int main()
{
    try { expandsWithoutMovingPinnedOrWrittenData(); adaptsDuringTorrentIoAndRapidSeeks();
        keepsPeersAfterTheCurrentWindowFinishes(); fitsProtectedMetadataWithinATinyAbsoluteBudget(); growsImmediatelyForEssentialMetadata(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
