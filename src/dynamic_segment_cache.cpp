#include "dynamic_segment_cache.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <set>

namespace intezam {

DynamicSegmentCache::DynamicSegmentCache(PieceGeometry geometry, std::size_t slots, double bitrate)
    : geometry_(geometry), slots_(slots), segment_size_(std::max(16384, geometry.piece_length / 2)), bitrate_(bitrate)
{
    geometry_.validate();
    if (slots == 0) throw std::invalid_argument("No whole-piece cache slots available.");
    updateVideoBitrate(bitrate);
}

void DynamicSegmentCache::updatePlaybackPosition(std::int64_t byte_offset)
{
    if (byte_offset < 0 || byte_offset >= geometry_.video_size) throw std::out_of_range("Playback position");
    position_ = byte_offset;
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
    const auto current = position_ / segment_size_;
    // More throughput permits a larger horizon; instability/starvation adds a
    // cushion. All estimates are ultimately clipped by whole-piece disk capacity.
    const double ratio = observed_ ? speed_ / bitrate_ : 1.0;
    const double instability = std::min(2.0, jitter_ / std::max(speed_, bitrate_ * 0.1));
    const double seconds = 2.0 + std::min(4.0, ratio) * 2.0 + instability * 2.0
                         + (buffer_seconds_ < 1.0 ? 2.0 : 0.0);
    const auto maximum = std::min<std::int64_t>(total,
        (static_cast<std::int64_t>(slots_) * geometry_.piece_length) / segment_size_);
    auto count = static_cast<std::int64_t>(std::min<double>(maximum,
        std::max(1.0, std::ceil(seconds * bitrate_ / segment_size_))));
    for (; count >= 1; --count) {
        const auto first = std::clamp(current - count / 3, std::int64_t{0}, total - count);
        const auto begin = first * segment_size_;
        const auto end = std::min(geometry_.video_size, (first + count) * segment_size_);
        auto pieces = geometry_.piecesForRange(begin, end - begin);
        std::set<int> required(pieces.begin(), pieces.end());
        required.insert(urgent_pieces.begin(), urgent_pieces.end());
        if (required.size() <= slots_) return {first, first + count - 1, current, begin, end, std::move(pieces)};
    }
    throw std::runtime_error("2.5% cache cannot fit playback and required container/read pieces together.");
}

} // namespace intezam
