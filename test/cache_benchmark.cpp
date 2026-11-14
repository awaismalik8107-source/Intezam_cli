#include "local_video_torrent.hpp"
#include "synthetic_media.hpp"
#include "vlc_player.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <future>
#include <thread>

using namespace std::chrono_literals;
using test_support::require;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t verifiedVideoBytes(const test_support::LocalVideoTorrent& torrent, int index)
{
    const auto piece = torrent.metadata()->piece_length();
    const auto begin = std::max<std::int64_t>(0, std::int64_t{index} * piece - torrent.video().torrent_offset);
    const auto end = std::min(torrent.video().size_bytes, std::int64_t{index + 1} * piece - torrent.video().torrent_offset);
    return std::max<std::int64_t>(0, end - begin);
}

class Measurements {
public:
    Measurements(std::string name, const fs::path& trace_directory)
        : name_(std::move(name))
    {
        if (!trace_directory.empty()) {
            fs::create_directories(trace_directory);
            trace_.open(trace_directory / (name_ + ".csv"));
            trace_ << "seconds,free_slots,rebuilds,window_changes,waiting_reads,wait_us\n";
        }
    }
    void sample(const intezam::TorrentVideoSource& source)
    {
        last_ = source.status();
        require(last_.error.empty(), last_.error);
        const auto& cache = last_.cache;
        require(cache.allocated_bytes <= cache.budget_bytes, "benchmark disk bound");
        minimum_free_ = std::min(minimum_free_, cache.performance.free_slots);
        maximum_free_ = std::max(maximum_free_, cache.performance.free_slots);
        peak_allocation_ = std::max(peak_allocation_, cache.allocated_bytes);
        forward_sum_ += cache.forward_buffer_seconds;
        utilization_sum_ += cache.performance.total_slots ?
            1.0 - double(cache.performance.free_slots) / cache.performance.total_slots : 0;
        ++samples_;
        if (trace_) trace_ << elapsed() << ',' << cache.performance.free_slots << ',' << cache.torrent_rebuilds
            << ',' << cache.performance.window_movements << ',' << last_.waiting_reads << ','
            << last_.performance.wait_microseconds << '\n';
    }
    void samplePlayer(const intezam::PlaybackStatus& player)
    {
        require(player.error.empty() && player.state != libvlc_Error, "benchmark VLC failure: " + player.error);
        if (first_frame_ms_ < 0 && player.decoded_video_frames > 0) first_frame_ms_ = elapsed() * 1000;
        if (player.state == libvlc_Playing) playing_ = true;
        const bool buffering = playing_ && (player.state == libvlc_Buffering || player.waiting_for_buffer);
        if (buffering && !buffering_since_) { buffering_since_ = Clock::now(); ++vlc_stalls_; }
        if (!buffering && buffering_since_) {
            vlc_buffering_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - *buffering_since_).count();
            buffering_since_.reset();
        }
    }
    void print() const
    {
        const auto& cache = last_.cache;
        const auto& metrics = cache.performance;
        const auto& reads = last_.performance;
        const auto attempts = reads.cache_hits + reads.cache_misses;
        std::cout << std::fixed << std::setprecision(3) << name_ << ',' << elapsed() << ',' << cache.torrent_rebuilds
            << ',' << (cache.torrent_rebuilds ? metrics.rebuild_microseconds / 1000.0 / cache.torrent_rebuilds : 0)
            << ',' << metrics.maximum_rebuild_microseconds / 1000.0 << ',' << metrics.pieces_evicted
            << ',' << metrics.pieces_admitted << ',' << metrics.window_movements << ',' << reads.cache_hits
            << ',' << reads.cache_misses << ',' << (attempts ? 100.0 * reads.cache_hits / attempts : 0)
            << ',' << reads.wait_microseconds / 1000.0 << ',' << reads.maximum_wait_microseconds / 1000.0
            << ',' << reads.stalls << ',' << reads.stall_microseconds / 1000.0
            << ',' << metrics.disk_microseconds / 1000.0 << ',' << reads.disk_read_microseconds / 1000.0
            << ',' << minimum_free_ << ',' << maximum_free_ << ',' << cache.allocated_bytes << ',' << cache.budget_bytes
            << ',' << vlc_stalls_ << ',' << vlc_buffering_ms_ << ',' << metrics.priority_updates
            << ',' << metrics.deadline_updates << ',' << metrics.deadline_resets << ',' << metrics.proactive_refills
            << ',' << first_frame_ms_ << ',' << forward_sum_ / std::max<std::size_t>(1, samples_)
            << ',' << peak_allocation_ << ',' << utilization_sum_ / std::max<std::size_t>(1, samples_)
            << ',' << seek_ms_ / std::max<std::size_t>(1, seeks_) << ',' << verified_bytes / std::max(0.001, elapsed())
            << ',' << startup_ms_ << ',' << (reads.wait_microseconds - startup_wait_us_) / 1000.0
            << ',' << reads.stalls - startup_stalls_
            << ',' << (reads.stall_microseconds - startup_stall_us_) / 1000.0 << std::endl;
    }
    void recordSeek(double milliseconds) { seek_ms_ += milliseconds; ++seeks_; }
    void ready(const intezam::TorrentVideoSource& source)
    {
        startup_ms_ = elapsed() * 1000;
        startup_wait_us_ = source.status().performance.wait_microseconds;
        startup_stalls_ = source.status().performance.stalls;
        startup_stall_us_ = source.status().performance.stall_microseconds;
    }
    std::uint64_t verified_bytes = 0;
private:
    double elapsed() const { return std::chrono::duration<double>(Clock::now() - started_).count(); }
    std::string name_;
    std::ofstream trace_;
    Clock::time_point started_ = Clock::now();
    intezam::VideoSourceStatus last_;
    std::size_t minimum_free_ = std::numeric_limits<std::size_t>::max();
    std::size_t maximum_free_ = 0;
    bool playing_ = false;
    std::optional<Clock::time_point> buffering_since_;
    std::size_t vlc_stalls_ = 0;
    double vlc_buffering_ms_ = 0;
    double first_frame_ms_ = -1, forward_sum_ = 0, utilization_sum_ = 0, seek_ms_ = 0;
    std::int64_t peak_allocation_ = 0;
    std::size_t samples_ = 0, seeks_ = 0;
    double startup_ms_ = 0;
    std::uint64_t startup_wait_us_ = 0;
    std::uint64_t startup_stalls_ = 0, startup_stall_us_ = 0;
};

void readChecked(intezam::TorrentVideoSource& source, test_support::LocalVideoTorrent& torrent,
                 std::int64_t offset, std::size_t length)
{
    std::array<unsigned char, 16384> bytes{};
    while (length) {
        const auto count = source.read(offset, bytes.data(), std::min(length, bytes.size()));
        require(count > 0, "benchmark unexpected EOF");
        const auto expected = torrent.expected(offset, count);
        require(std::equal(bytes.begin(), bytes.begin() + count, expected.begin()), "benchmark verified bytes");
        offset += count;
        length -= count;
    }
}

void runByteWorkload(const fs::path& media, const fs::path& trace, int repetition, bool seeking, bool unstable = false)
{
    test_support::LocalVideoTorrent torrent(media);
    if (unstable) torrent.seed.set_upload_limit(1024 * 1024);
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.playback_bytes_per_second = 512 * 1024;
    std::atomic<std::uint64_t> verified{0};
    options.on_piece_verified = [&](int index) { verified += verifiedVideoBytes(torrent, index); };
    Measurements measurements((unstable ? "unstable-" : seeking ? "seeking-" : "sequential-") + std::to_string(repetition), trace);
    auto source = torrent.source(options);
    source->markPlaybackActive();
    source->prebuffer();
    measurements.ready(*source);
    const auto started = Clock::now();
    std::future<void> recovery;
    for (int step = 0; step < 128; ++step) {
        if (unstable && step == 32) {
            torrent.seed.pause();
            recovery = std::async(std::launch::async, [&] { std::this_thread::sleep_for(500ms); torrent.seed.resume(); });
        }
        std::int64_t offset = std::int64_t{step} * 16384;
        if (seeking) {
            static constexpr int destinations[]{0, 2, 8, 2, 16, 4, 0, 12};
            offset = destinations[step / 16] * 1024LL * 1024 + (step % 16) * 4096;
            source->seek(offset);
        }
        const auto reading = Clock::now();
        readChecked(*source, torrent, offset, seeking ? 4096 : 16384);
        if (seeking) measurements.recordSeek(std::chrono::duration<double, std::milli>(Clock::now() - reading).count());
        if (step % 8 == 0) readChecked(*source, torrent, offset, 1024); // reproducible hot reads
        measurements.sample(*source);
        if (!seeking) std::this_thread::sleep_until(started + (step + 1) * 31250us);
        else std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(50ms); // publish the last worker snapshot
    measurements.sample(*source);
    measurements.verified_bytes = verified.load();
    measurements.print();
    const auto directory = source->directory();
    source->stop();
    require(!fs::exists(directory), "benchmark cleanup");
}

void runVlcWorkload(const fs::path& media, const fs::path& trace, int repetition)
{
    test_support::LocalVideoTorrent torrent(media);
    std::atomic<std::uint64_t> verified{0};
    auto options = torrent.options();
    options.budget.mode = intezam::CacheBudgetMode::adaptive;
    options.on_piece_verified = [&](int index) { verified += verifiedVideoBytes(torrent, index); };
    Measurements measurements("vlc-" + std::to_string(repetition), trace);
    auto source = torrent.source(options);
    const auto container = intezam::probeContainer(*source);
    source->setContainer(container);
    source->prebuffer();
    measurements.ready(*source);
    intezam::VlcPlayer player(source, {true, 100});
    player.play();
    bool decoded = false;
    const auto deadline = Clock::now() + 60s;
    for (;;) {
        const auto status = player.status();
        if (!status.error.empty()) {
            const auto state = source->status();
            std::cerr << "VLC failure diagnostics: position=" << state.cache.playback_position
                << " window=" << state.cache.desired.begin_byte << ':' << state.cache.desired.end_byte
                << " forward=" << state.cache.forward_buffer_seconds << " urgent=" << state.cache.urgent_pieces
                << " queued=" << state.cache.queued_pieces << " downloading=" << state.cache.downloading_pieces
                << " deferred=" << state.cache.deferred_pieces << " rebuilds=" << state.cache.torrent_rebuilds << '\n';
        }
        decoded = decoded || status.decoded_video_frames > 0;
        measurements.samplePlayer(status);
        measurements.sample(*source);
        if (status.state == libvlc_Ended) break;
        require(Clock::now() < deadline, "benchmark VLC timeout");
        std::this_thread::sleep_for(20ms);
    }
    require(decoded, "benchmark actually decoded video");
    measurements.verified_bytes = verified.load();
    measurements.print();
    player.stop();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        int repetitions = 3;
        fs::path vlc_media, trace;
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            require(index + 1 < argc, "benchmark option value required");
            if (option == "--vlc") vlc_media = argv[++index];
            else if (option == "--trace") trace = argv[++index];
            else if (option == "--repetitions") repetitions = std::stoi(argv[++index]);
            else throw std::invalid_argument("unknown benchmark option: " + option);
        }
        require(repetitions > 0 && repetitions <= 20, "benchmark repetitions 1..20");
        test_support::SyntheticMedia synthetic;
        std::cout << "workload,seconds,rebuilds,average_rebuild_ms,maximum_rebuild_ms,evicted,admitted,window_changes,"
            "hits,misses,hit_percent,wait_ms,max_wait_ms,stalls,stall_ms,backend_ms,lease_read_ms,min_free,max_free,"
            "allocated,budget,vlc_buffer_stalls,vlc_buffer_ms,priority_updates,deadline_updates,deadline_resets,proactive_refills,"
            "first_frame_ms,average_forward_s,peak_allocated,average_utilization,seek_read_ms,verified_bytes_per_s,"
            "startup_ms,post_start_wait_ms,post_start_stalls,post_start_stall_ms\n";
        for (int repetition = 1; repetition <= repetitions; ++repetition) {
            runByteWorkload(synthetic.path(), trace, repetition, false);
            runByteWorkload(synthetic.path(), trace, repetition, true);
            runByteWorkload(synthetic.path(), trace, repetition, false, true);
            if (!vlc_media.empty()) runVlcWorkload(vlc_media, trace, repetition);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
