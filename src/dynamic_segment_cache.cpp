#include "dynamic_segment_cache.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <set>

namespace intezam {

DynamicSegmentCache::DynamicSegmentCache(PieceGeometry geometry, std::size_t slots, double bitrate, double forward_target_seconds)
    : geometry_(geometry), slots_(slots), segment_size_(std::max(16384, geometry.piece_length / 2)), bitrate_(bitrate),
      forward_target_seconds_(forward_target_seconds)
{
    geometry_.validate();
    if (slots == 0) throw std::invalid_argument("No whole-piece cache slots available.");
    if (!std::isfinite(forward_target_seconds) || forward_target_seconds < 0 || forward_target_seconds > 120) {
        throw std::invalid_argument("Forward buffer target must be between 0 and 120 seconds.");
    }
    updateVideoBitrate(bitrate);
}

void DynamicSegmentCache::updatePlaybackPosition(std::int64_t byte_offset)
{
    if (byte_offset < 0 || byte_offset >= geometry_.video_size) throw std::out_of_range("Playback position");
    position_ = byte_offset;
}

void DynamicSegmentCache::reposition(std::int64_t byte_offset)
{
    updatePlaybackPosition(byte_offset);
    rolling_window_.reset();
}

void DynamicSegmentCache::updateCapacity(std::size_t slots)
{
    if (slots == 0) throw std::invalid_argument("No whole-piece cache slots available.");
    if (slots_ != slots) {
        slots_ = slots;
        rolling_window_.reset();
    }
}

void DynamicSegmentCache::updateVideoBitrate(double bytes_per_second)
{
    if (!std::isfinite(bytes_per_second) || bytes_per_second <= 0 || bytes_per_second > 1e12) {
        throw std::invalid_argument("Playback consumption rate must be finite and positive (at most 1 TB/s).");
    }
    bitrate_ = bytes_per_second;
}

void DynamicSegmentCache::updateNetworkSpeed(double bytes_per_second)
{
    if (!std::isfinite(bytes_per_second) || bytes_per_second < 0) throw std::invalid_argument("Invalid throughput sample");
    if (!observed_) {
        speed_ = bytes_per_second;
        observed_ = true;
    } else {
        jitter_ = 0.8 * jitter_ + 0.2 * std::abs(bytes_per_second - speed_);
        speed_ = 0.8 * speed_ + 0.2 * bytes_per_second;
    }
}

void DynamicSegmentCache::updateBufferHealth(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("Invalid buffer health");
    buffer_seconds_ = seconds;
}

SegmentWindow DynamicSegmentCache::window(const std::vector<int>& urgent_pieces) const
{
    const auto total = (geometry_.video_size - 1) / segment_size_ + 1;
    // More throughput permits a larger horizon; instability/starvation adds a
    // cushion. All estimates are ultimately clipped by whole-piece disk capacity.
    const double ratio = observed_ ? speed_ / bitrate_ : 1.0;
    const double instability = std::min(2.0, jitter_ / std::max(speed_, bitrate_ * 0.1));
    double seconds = 2.0 + std::min(4.0, ratio) * 2.0 + instability * 2.0
                         + (buffer_seconds_ < 1.0 ? 2.0 : 0.0);
    if (forward_target_seconds_ > 0) {
        // Leave room for backward retention and rolling drift even on a slow
        // swarm. Faster peers fund a larger horizon, within the same slot file.
        seconds = std::max(seconds, std::min(forward_target_seconds_ * 3.75,
            forward_target_seconds_ * 2.0 + std::min(3.0, ratio) * forward_target_seconds_ * 7.0 / 12.0));
    }
    const auto maximum = std::min<std::int64_t>(total,
        (static_cast<std::int64_t>(slots_) * geometry_.piece_length) / segment_size_);
    auto count = static_cast<std::int64_t>(std::min<double>(maximum,
        std::max(1.0, std::ceil(seconds * bitrate_ / segment_size_))));
    if (forward_target_seconds_ > 0) {
        // At the beginning there is no behind region to retain; avoid turning
        // that unused region into an unnecessarily long startup download.
        const auto ahead = static_cast<std::int64_t>(std::max(1.0,
            std::ceil(forward_target_seconds_ * 2.5 * bitrate_ / segment_size_)));
        count = std::min(count, position_ / segment_size_ + ahead);
    }
    return fitWindow(count, urgent_pieces);
}

SegmentWindow DynamicSegmentCache::fitWindow(std::int64_t count, const std::vector<int>& urgent_pieces) const
{
    const auto total = (geometry_.video_size - 1) / segment_size_ + 1;
    const auto current = position_ / segment_size_;
    for (; count >= 1; --count) {
        const auto first = std::clamp(current - count / 3, std::int64_t{0}, total - count);
        const auto begin = first * segment_size_;
        const auto end = std::min(geometry_.video_size, (first + count) * segment_size_);
        auto pieces = geometry_.piecesForRange(begin, end - begin);
        std::set<int> required(pieces.begin(), pieces.end());
        required.insert(urgent_pieces.begin(), urgent_pieces.end());
        if (required.size() <= slots_) return {first, first + count - 1, current, begin, end, std::move(pieces)};
    }
    throw std::runtime_error("Effective cache cannot fit playback and required container/read pieces together.");
}

SegmentWindow DynamicSegmentCache::planWindow(const std::vector<int>& urgent_pieces)
{
    auto candidate = window(urgent_pieces);
    if (rolling_window_) {
        const auto& previous = *rolling_window_;
        const auto resize_threshold = std::max<std::int64_t>(2, previous.segmentCount() / 8);
        if (std::abs(candidate.segmentCount() - previous.segmentCount()) < resize_threshold) {
            candidate = fitWindow(previous.segmentCount(), urgent_pieces);
        }
        std::set<int> required(previous.pieces.begin(), previous.pieces.end());
        required.insert(urgent_pieces.begin(), urgent_pieces.end());
        const bool current_inside = candidate.current_segment >= previous.first_segment
            && candidate.current_segment <= previous.last_segment;
        // Move after exceeding the allowed drift; a 12..23 segment horizon can
        // retain one segment of drift rather than recentering at every boundary.
        const auto movement_threshold = candidate.segmentCount() / 12 + 1;
        if (candidate.segmentCount() == previous.segmentCount() && required.size() <= slots_ && current_inside
            && std::abs(candidate.first_segment - previous.first_segment) < movement_threshold) {
            rolling_window_->current_segment = candidate.current_segment;
            return *rolling_window_;
        }
    }
    rolling_window_ = candidate;
    return candidate;
}

} // namespace intezam
