#include "stream_policy.hpp"
#include <algorithm>
#include <cmath>

namespace intezam {
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
