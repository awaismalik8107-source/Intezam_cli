#include "local_video_torrent.hpp"
#include "synthetic_media.hpp"
#include "vlc_player.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>

using namespace std::chrono_literals;
using test_support::require;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

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
        if (trace_) trace_ << elapsed() << ',' << cache.performance.free_slots << ',' << cache.torrent_rebuilds
            << ',' << cache.performance.window_movements << ',' << last_.waiting_reads << ','
            << last_.performance.wait_microseconds << '\n';
    }
    void samplePlayer(const intezam::PlaybackStatus& player)
    {
        require(player.error.empty() && player.state != libvlc_Error, "benchmark VLC failure: " + player.error);
        if (player.state == libvlc_Playing) playing_ = true;
        const bool buffering = playing_ && player.state == libvlc_Buffering;
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
            << ',' << vlc_stalls_ << ',' << vlc_buffering_ms_ << std::endl;
    }
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

void runByteWorkload(const fs::path& media, const fs::path& trace, int repetition, bool seeking)
{
    test_support::LocalVideoTorrent torrent(media);
    auto options = torrent.options();
    options.playback_bytes_per_second = 512 * 1024;
    auto source = torrent.source(options);
    source->markPlaybackActive();
    Measurements measurements((seeking ? "seeking-" : "sequential-") + std::to_string(repetition), trace);
    const auto started = Clock::now();
    for (int step = 0; step < 128; ++step) {
        std::int64_t offset = std::int64_t{step} * 16384;
        if (seeking) {
            static constexpr int destinations[]{0, 2, 8, 2, 16, 4, 0, 12};
            offset = destinations[step / 16] * 1024LL * 1024 + (step % 16) * 4096;
            source->seek(offset);
        }
        readChecked(*source, torrent, offset, seeking ? 4096 : 16384);
        if (step % 8 == 0) readChecked(*source, torrent, offset, 1024); // reproducible hot reads
        measurements.sample(*source);
        if (!seeking) std::this_thread::sleep_until(started + (step + 1) * 31250us);
        else std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(50ms); // publish the last worker snapshot
    measurements.sample(*source);
    measurements.print();
    const auto directory = source->directory();
    source->stop();
    require(!fs::exists(directory), "benchmark cleanup");
}

void runVlcWorkload(const fs::path& media, const fs::path& trace, int repetition)
{
    test_support::LocalVideoTorrent torrent(media);
    auto source = torrent.source();
    Measurements measurements("vlc-" + std::to_string(repetition), trace);
    const auto container = intezam::probeContainer(*source);
    source->setContainer(container);
    source->prebuffer();
    intezam::VlcPlayer player(source, {true, 100});
    player.play();
    bool decoded = false;
    const auto deadline = Clock::now() + 60s;
    for (;;) {
        const auto status = player.status();
        decoded = decoded || status.decoded_video_frames > 0;
        measurements.samplePlayer(status);
        measurements.sample(*source);
        if (status.state == libvlc_Ended) break;
        require(Clock::now() < deadline, "benchmark VLC timeout");
        std::this_thread::sleep_for(20ms);
    }
    require(decoded, "benchmark actually decoded video");
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
            "allocated,budget,vlc_buffer_stalls,vlc_buffer_ms\n";
        for (int repetition = 1; repetition <= repetitions; ++repetition) {
            runByteWorkload(synthetic.path(), trace, repetition, false);
            runByteWorkload(synthetic.path(), trace, repetition, true);
            if (!vlc_media.empty()) runVlcWorkload(vlc_media, trace, repetition);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
