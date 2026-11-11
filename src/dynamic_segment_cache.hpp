#pragma once

#include "cache_geometry.hpp"

#include <cstddef>

namespace intezam {

struct SegmentWindow {
    std::int64_t first_segment;
    std::int64_t last_segment;
    std::int64_t current_segment;
    std::int64_t begin_byte;
    std::int64_t end_byte;
    std::vector<int> pieces;
    std::int64_t segmentCount() const { return last_segment - first_segment + 1; }
};

// Policy only: callers serialize updates. Disk writes/read leases use PieceStore's
// independent mutex; the libtorrent thread never accesses this policy object.
class DynamicSegmentCache {
public:
    DynamicSegmentCache(PieceGeometry geometry, std::size_t slots, double bitrate);
    void updatePlaybackPosition(std::int64_t byte_offset);
    void updateNetworkSpeed(double bytes_per_second);
    void updateVideoBitrate(double bytes_per_second);
    void updateBufferHealth(double seconds);
    SegmentWindow window() const;
    double networkSpeed() const { return speed_; }
    double videoBitrate() const { return bitrate_; }
    std::int64_t playbackPosition() const { return position_; }
    std::int64_t segmentSize() const { return segment_size_; }

private:
    PieceGeometry geometry_;
    std::size_t slots_;
    std::int64_t segment_size_;
    std::int64_t position_ = 0;
    double bitrate_;
    double speed_ = 0;
    double jitter_ = 0;
    double buffer_seconds_ = 0;
    bool observed_ = false;
};

} // namespace intezam
