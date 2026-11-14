#include "synthetic_media.hpp"
#include <iostream>
#include <future>
#include <thread>
#include <array>
using namespace std::chrono_literals;
using test_support::require;
namespace {
void pumpUntil(intezam::TorrentCacheSession& cache, const std::function<bool()>& complete)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!complete()) {
        cache.pump(10ms);
        require(std::chrono::steady_clock::now() < deadline, "forward refill timeout");
    }
}
void refillsBeforeTheCurrentPieceRunsOut(const std::filesystem::path& media)
{
    constexpr int piece = 1024 * 1024;
    test_support::LocalVideoTorrent torrent(media, piece);
    auto options = torrent.options();
    options.playback_bytes_per_second = piece;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    require(cache.diagnostics().performance.total_slots == 4, "four-piece sample-sized cache");
    pumpUntil(cache, [&] { const auto state = cache.diagnostics(); return state.queued_pieces == 0 && state.downloading_pieces == 0; });
    // Move in half-piece steps and let the requested forward horizon fill. No read
    // may have to become urgent before a cold slot can fund the next forward piece.
    for (int step = 1; step <= 16; ++step) {
        const auto offset = std::int64_t{step} * piece / 2;
        cache.updateReadRequest(offset, std::nullopt);
        pumpUntil(cache, [&] { return cache.isRangeCached(offset, 1024); });
        const auto end = cache.diagnostics().desired.end_byte;
        pumpUntil(cache, [&] { return cache.isRangeCached(offset, end - offset); });
        const auto state = cache.diagnostics();
        require(state.allocated_bytes <= state.budget_bytes, "forward refill obeys disk budget");
        require(state.forward_buffer_seconds >= 0.5, "at least half a piece of runway before reader advances");
    }
    std::cout << "Forward refill stayed ahead without an urgent read.\n";
}
void preservesDeadlinesForUnchangedForwardDownloads(const std::filesystem::path& media)
{
    constexpr int piece = 1024 * 1024;
    test_support::LocalVideoTorrent torrent(media, piece);
    auto options = torrent.options();
    options.playback_bytes_per_second = piece;
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    pumpUntil(cache, [&] { return cache.isRangeCached(0, 1024); });
    // Cached reads appearing/disappearing must not clear unrelated forward work.
    pumpUntil(cache, [&] { const auto state = cache.diagnostics(); return state.queued_pieces == 0 && state.downloading_pieces == 0; });
    cache.updateReadRequest(0, std::nullopt);
    const auto before = cache.diagnostics().performance;
    for (int iteration = 0; iteration < 100; ++iteration) {
        cache.updateReadRequest(0, intezam::VideoRange{0, 1024});
        cache.updateReadRequest(0, std::nullopt);
    }
    const auto after = cache.diagnostics().performance;
    require(after.deadline_resets == before.deadline_resets, "cached reads retain existing forward deadlines");
    require(after.deadline_updates == before.deadline_updates, "cached reads do not republish all deadlines");
    require(after.priority_updates == before.priority_updates, "unchanged priorities leave requests undisturbed");
}

void keepsReadingAcrossABriefPeerPause(const std::filesystem::path& media)
{
    constexpr int piece = 256 * 1024;
    test_support::LocalVideoTorrent torrent(media, piece);
    torrent.seed.set_upload_limit(1024 * 1024);
    auto options = torrent.options();
    options.playback_bytes_per_second = piece;
    auto source = torrent.source(options);
    source->markPlaybackActive();
    source->prebuffer();
    const auto started = std::chrono::steady_clock::now();
    std::future<void> peer_recovery;
    std::array<unsigned char, 16384> bytes{};
    std::uint64_t maximum_wait_us = 0;
    for (int step = 0; step < 48; ++step) {
        if (step == 16) {
            torrent.seed.pause();
            peer_recovery = std::async(std::launch::async, [&] { std::this_thread::sleep_for(400ms); torrent.seed.resume(); });
        }
        const auto offset = std::int64_t{step} * bytes.size();
        const auto reading = std::chrono::steady_clock::now();
        std::size_t count = 0;
        while (count < bytes.size()) {
            const auto read = source->read(offset + count, bytes.data() + count, bytes.size() - count);
            require(read > 0, "continuous source has not reached EOF");
            count += read;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - reading).count();
        maximum_wait_us = std::max<std::uint64_t>(maximum_wait_us, elapsed);
        require(count > 0 && std::equal(bytes.begin(), bytes.begin() + count, torrent.expected(offset, count).begin()), "smooth stream verified bytes");
        const auto state = source->status();
        require(state.cache.allocated_bytes <= state.cache.budget_bytes, "throttled stream physical bound");
        std::this_thread::sleep_until(started + (step + 1) * 62500us);
    }
    if (peer_recovery.valid()) peer_recovery.get();
    require(maximum_wait_us < 500000, "short outage is absorbed without a half-second playback read stall");
    std::cout << "Throttled stream with 400 ms peer pause: maximum read wait " << maximum_wait_us / 1000.0 << " ms.\n";
    source->stop();
    require(std::filesystem::is_empty(options.root), "smooth stream cleanup");
}

void reportsIdleDownloadsWithoutLosingTheThroughputEstimate(const std::filesystem::path& media)
{
    test_support::LocalVideoTorrent torrent(media, 1024 * 1024);
    auto options = torrent.options();
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    pumpUntil(cache, [&] { return cache.diagnostics().download_bytes_per_second > 0; });
    pumpUntil(cache, [&] { return cache.diagnostics().download_bytes_per_second == 0; });
    require(cache.diagnostics().network_bytes_per_second > 0, "idle prefetch preserves estimated link throughput");
}

void separatesPlannedPrebufferingFromDecoderProbeLimits()
{
    constexpr int piece = 8 * 1024 * 1024;
    test_support::SyntheticMedia media(64);
    test_support::LocalVideoTorrent torrent(media.path(), piece);
    auto options = torrent.options();
    options.budget.maximum_percent = 75;
    options.playback_bytes_per_second = 16 * 1024 * 1024;
    auto source = torrent.source(options);
    unsigned char header[16];
    require(source->readMetadata(0, header, sizeof(header)) > 0, "large-piece header");
    require(source->readMetadata(source->size() - piece / 2, header, sizeof(header)) > 0, "large-piece tail metadata");
    source->prebuffer(); // Intentional bounded prefix must not look like an index scan.
    bool probe_rejected = false;
    try { source->readMetadata(source->size() / 2, header, sizeof(header)); }
    catch (const std::runtime_error& error) {
        probe_rejected = std::string(error.what()).find("bounded unique-range allowance") != std::string::npos;
    }
    require(probe_rejected, "decoder probe limit remains enforced after planned prebuffering");
    source->stop();
    require(std::filesystem::is_empty(options.root), "large-piece prebuffer cleanup");
}

void retimesOutstandingDeadlinesAfterAnExplicitSeek(const std::filesystem::path& media)
{
    test_support::LocalVideoTorrent torrent(media, 1024 * 1024);
    torrent.seed.pause();
    std::this_thread::sleep_for(50ms);
    const auto options = torrent.options();
    intezam::TorrentCacheSession cache(torrent.magnet(), torrent.metadata(), torrent.video(), options);
    const auto before = cache.diagnostics().performance;
    cache.seek(0); // A new playback epoch must not inherit expired absolute deadlines.
    const auto after = cache.diagnostics().performance;
    require(after.deadline_updates > before.deadline_updates, "explicit seek retimes retained outstanding pieces");
    require(after.deadline_resets == before.deadline_resets, "retiming preserves the existing time-critical set");
}

}
int main()
{
    try {
        test_support::SyntheticMedia media;
        refillsBeforeTheCurrentPieceRunsOut(media.path());
        preservesDeadlinesForUnchangedForwardDownloads(media.path());
        keepsReadingAcrossABriefPeerPause(media.path());
        reportsIdleDownloadsWithoutLosingTheThroughputEstimate(media.path());
        separatesPlannedPrebufferingFromDecoderProbeLimits();
        retimesOutstandingDeadlinesAfterAnExplicitSeek(media.path());
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
