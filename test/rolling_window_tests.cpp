#include "dynamic_segment_cache.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

void stabilizesSmallMovements()
{
    const intezam::PieceGeometry geometry{128LL * 1024 * 1024, 7000, 128LL * 1024 * 1024 + 7000, 65536};
    intezam::DynamicSegmentCache cache(geometry, 40, 262144);
    const auto middle = geometry.video_size / 2;
    cache.updatePlaybackPosition(middle);
    auto previous = cache.planWindow();
    const auto initial = previous;
    for (int iteration = 0; iteration < 1000; ++iteration) {
        cache.updatePlaybackPosition(middle + (iteration % 2 ? 32768 : -32768));
        const auto window = cache.planWindow();
        require(window.first_segment == initial.first_segment && window.last_segment == initial.last_segment,
                "small position oscillation keeps the rolling window");
    }
    int movements = 0;
    for (int step = 1; step <= 40; ++step) {
        cache.updatePlaybackPosition(middle + step * cache.segmentSize());
        const auto window = cache.planWindow();
        if (window.first_segment != previous.first_segment) ++movements;
        const auto behind = window.current_segment - window.first_segment;
        require(std::abs(behind - window.segmentCount() / 3) <= std::max<std::int64_t>(1, window.segmentCount() / 12),
                "rolling drift preserves approximately one-third behind");
        require(window.current_segment <= window.last_segment && window.current_segment >= window.first_segment,
                "current segment never leaves the planned window");
        previous = window;
    }
    require(movements > 0 && movements < 20, "continuous playback moves in meaningful groups");
    cache.reposition(middle + cache.segmentSize());
    const auto sought = cache.planWindow();
    require(sought.first_segment == sought.current_segment - sought.segmentCount() / 3, "explicit seek immediately recenters");
    cache.reposition(0);
    require(cache.planWindow().first_segment == 0, "seek to beginning");
    cache.reposition(geometry.video_size - 1);
    require(cache.planWindow().end_byte == geometry.video_size, "seek to EOF boundary");
}

void respectsAdaptationAndUrgency()
{
    const intezam::PieceGeometry geometry{128LL * 1024 * 1024, 7000, 128LL * 1024 * 1024 + 7000, 65536};
    intezam::DynamicSegmentCache cache(geometry, 40, 262144);
    cache.reposition(geometry.video_size / 2);
    auto initial = cache.planWindow();
    for (int sample = 0; sample < 50; ++sample) {
        cache.updateNetworkSpeed(262144 * (sample % 2 ? 1.01 : 0.99));
        require(cache.planWindow().segmentCount() == initial.segmentCount(), "throughput jitter does not resize the window");
    }
    cache.updateVideoBitrate(524288);
    const auto larger = cache.planWindow();
    require(larger.segmentCount() > initial.segmentCount(), "large bitrate changes still adapt");
    const auto urgent = cache.planWindow({0, 1});
    std::set<int> required(urgent.pieces.begin(), urgent.pieces.end());
    required.insert(0); required.insert(1);
    require(required.size() <= 40, "metadata urgency immediately overrides hysteresis capacity");
    require(urgent.segmentCount() < larger.segmentCount(), "urgent metadata reduces the active horizon");
}
void givesSmoothPlaybackAReachableForwardCushion()
{
    const intezam::PieceGeometry geometry{8LL * 1024 * 1024 * 1024, 7000, 8LL * 1024 * 1024 * 1024 + 7000, 2 * 1024 * 1024};
    intezam::DynamicSegmentCache cache(geometry, 100, 1024 * 1024, 12);
    cache.reposition(geometry.video_size / 2);
    cache.updateNetworkSpeed(4 * 1024 * 1024);
    const auto fast = cache.planWindow();
    const auto ahead = (fast.end_byte - cache.playbackPosition()) / cache.videoBitrate();
    require(ahead >= 28 && ahead <= 32, "smooth profile prefetches about thirty seconds ahead");
    for (int sample = 0; sample < 50; ++sample) cache.updateNetworkSpeed(0);
    const auto slow = cache.planWindow();
    require(slow.segmentCount() < fast.segmentCount(), "smooth horizon still adapts to throughput");
    require((slow.end_byte - cache.playbackPosition()) / cache.videoBitrate() >= 12,
        "slow profile has enough forward window for its resume target");
    cache.updateCapacity(4);
    require(cache.planWindow().pieces.size() <= 4, "smooth profile clips to small cache capacity");
}

} // namespace

int main()
{
    try {
        stabilizesSmallMovements();
        respectsAdaptationAndUrgency();
        givesSmoothPlaybackAReachableForwardCushion();
        std::cout << "Rolling hysteresis, adaptation, urgent metadata and immediate seeks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
