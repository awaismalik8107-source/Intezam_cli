#include "stream_policy.hpp"
#include "cache_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace intezam {
std::int64_t verifiedForwardBytes(const PieceGeometry& geometry, std::int64_t position,
    const std::function<bool(int)>& is_verified)
{
    geometry.validate();
    if (position < 0 || position > geometry.video_size) throw std::out_of_range("Forward buffer position.");
    auto global = geometry.video_offset + position;
    const auto begin = global;
    const auto end = geometry.video_offset + geometry.video_size;
    while (global < end) {
        const auto piece = static_cast<int>(global / geometry.piece_length);
        if (!is_verified(piece)) break;
        global = std::min(end, std::int64_t{piece + 1} * geometry.piece_length);
    }
    return global - begin;
}

BufferState AdaptiveBufferController::update(double buffered, AdaptiveBufferTargets targets,
    bool waiting, bool seeking, double stalled)
{
    if (seeking) state_ = BufferState::seeking;
    else if (stalled >= 3 && buffered < targets.emergency_seconds) state_ = BufferState::stalled;
    else if (state_ == BufferState::initializing || state_ == BufferState::prebuffering) {
        state_ = buffered >= targets.startup_seconds ? BufferState::playing : BufferState::prebuffering;
    } else if (state_ == BufferState::recovering || state_ == BufferState::stalled || state_ == BufferState::seeking) {
        state_ = buffered >= targets.resume_seconds ? BufferState::playing : BufferState::recovering;
    } else if (waiting && buffered < targets.emergency_seconds) state_ = BufferState::recovering;
    else state_ = buffered < targets.low_seconds ? BufferState::buffer_low : BufferState::playing;
    return state_;
}

int playbackPiecePriority(bool blocking, double ahead, bool verified, double low, bool buffer_low)
{
    if (blocking && !verified) return 7;
    if (ahead < 0) return 1;
    if (verified) return 4;
    if (ahead <= std::max(2.0, low)) return 7;
    if (ahead <= std::max(8.0, low * 2)) return 6;
    // In distress, distant data remains eligible but below near-future work.
    return buffer_low ? 1 : 4;
}

BufferWatermarks bufferWatermarks(double playback_rate, double network_rate, double forward_capacity)
{
    playback_rate = std::max(1.0, playback_rate);
    const double ratio = std::max(0.0, network_rate) / playback_rate;
    const double desired = ratio >= 1.5 ? 6.0 : ratio >= 1.0 ? 8.0 : 12.0;
    const double high = std::min(desired, std::max(0.0, forward_capacity) / playback_rate);
    return {std::min(2.0, high / 3.0), high};
}
bool RebufferGate::update(bool playing, bool user_paused, bool read_waiting, double buffered,
    BufferWatermarks targets, std::int64_t elapsed_ms)
{
    if (user_paused) { low_since_.reset(); return paused_; }
    if (paused_) {
        if (buffered >= targets.high_seconds) { paused_ = false; low_since_.reset(); }
        return paused_;
    }
    if (playing && read_waiting && buffered < targets.low_seconds) {
        if (!low_since_) low_since_ = elapsed_ms;
        paused_ = elapsed_ms - *low_since_ >= 250;
    } else low_since_.reset();
    return paused_;
}
int pieceDeadlineMilliseconds(double bytes_ahead, double playback_rate)
{
    const double seconds = std::max(0.0, bytes_ahead) / std::max(1.0, playback_rate);
    // Leave half a second for verification and decoder input. Bound the API int.
    return static_cast<int>(std::clamp((seconds - 0.5) * 1000.0, 0.0, 60000.0));
}
}
