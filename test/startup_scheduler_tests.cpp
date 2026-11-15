#include "synthetic_media.hpp"

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
using test_support::require;

namespace {
constexpr int piece_bytes = 1024 * 1024;

void prioritizesTheWholeStartupRunway(const std::filesystem::path& media)
{
    test_support::LocalVideoTorrent torrent(media, piece_bytes);
    torrent.seed.pause();
    std::this_thread::sleep_for(50ms);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.budget.adaptive.maximum_bytes = 24LL * piece_bytes;
    options.playback_bytes_per_second = piece_bytes;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    const auto before = cache.diagnostics();
    const std::int64_t prefix = 8 * piece_bytes;
    const intezam::VideoRange index{torrent.video().size_bytes - 4096, 4096};
    cache.setStartupReadAhead(prefix, {index});
    const auto startup = cache.diagnostics();
    require(startup.performance.deadline_updates >= before.performance.deadline_updates + 5,
        "all startup prefix pieces and the distant index receive time-critical requests before a blocking read");
    require(startup.allocated_bytes == before.allocated_bytes && startup.budget_bytes == before.budget_bytes,
        "startup scheduling uses the existing cache allocation and budget");
    for (int repeat = 0; repeat < 20; ++repeat) cache.setStartupReadAhead(prefix, {index});
    const auto unchanged = cache.diagnostics();
    require(unchanged.performance.deadline_updates == startup.performance.deadline_updates
        && unchanged.performance.deadline_resets == startup.performance.deadline_resets
        && unchanged.performance.priority_updates == startup.performance.priority_updates,
        "unchanged startup plans preserve outstanding peer requests");
    cache.setStartupReadAhead(0);
    const auto normal = cache.diagnostics();
    require(normal.performance.deadline_resets >= unchanged.performance.deadline_resets + 5,
        "playback transition clears startup-only deadlines");
}

void fetchesAnIndexWithoutMovingTheOpeningWindow(const std::filesystem::path& media)
{
    test_support::LocalVideoTorrent torrent(media, piece_bytes);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.budget.adaptive.maximum_bytes = 24LL * piece_bytes;
    options.header_read_mode = true;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    const intezam::VideoRange index{torrent.video().size_bytes - 4096, 4096};
    cache.setHeaderReadMode(false);
    cache.setStartupReadAhead(4 * piece_bytes, {index});
    const auto timeout = std::chrono::steady_clock::now() + 10s;
    while (!cache.isRangeCached(0, 4 * piece_bytes) || !cache.isRangeCached(index.offset, index.length)) {
        cache.pump(10ms);
        require(std::chrono::steady_clock::now() < timeout, "prefix and hinted index prefetch without a decoder demand");
    }
    const auto state = cache.diagnostics();
    require(state.playback_position == 0 && state.torrent_rebuilds == 0,
        "index prefetch preserves the opening window and peer session");
    require(state.allocated_bytes <= state.budget_bytes, "index prefetch respects the existing disk bound");
}
}

int main()
{
    try {
        test_support::SyntheticMedia media(32);
        prioritizesTheWholeStartupRunway(media.path());
        fetchesAnIndexWithoutMovingTheOpeningWindow(media.path());
        std::cout << "Startup runway deadlines and bounded index prefetch passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
