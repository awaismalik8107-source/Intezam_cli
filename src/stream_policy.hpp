#pragma once
#include <cstdint>
#include <optional>
#include <functional>
namespace intezam {
struct PieceGeometry;
std::int64_t verifiedForwardBytes(const PieceGeometry& geometry, std::int64_t position,
    const std::function<bool(int)>& is_verified);
struct BufferWatermarks { double low_seconds; double high_seconds; };
enum class BufferState { initializing, prebuffering, playing, buffer_low, recovering, seeking, stalled };
struct AdaptiveBufferTargets {
    double startup_seconds = 8;
    double low_seconds = 2;
    double emergency_seconds = 0.5;
    double resume_seconds = 10;
};
class AdaptiveBufferController {
public:
    BufferState update(double verified_seconds, AdaptiveBufferTargets targets,
        bool read_waiting, bool seeking, double stalled_seconds);
    BufferState state() const { return state_; }
private:
    BufferState state_ = BufferState::initializing;
};
int playbackPiecePriority(bool blocking, double seconds_ahead, bool verified,
    double low_threshold, bool buffer_low);
BufferWatermarks bufferWatermarks(double playback_rate, double network_rate, double forward_capacity);
class RebufferGate {
public:
    bool update(bool playing, bool user_paused, bool read_waiting, double buffered,
        BufferWatermarks targets, std::int64_t elapsed_ms);
    void reset() { paused_ = false; low_since_.reset(); }
private:
    bool paused_ = false;
    std::optional<std::int64_t> low_since_;
};
int pieceDeadlineMilliseconds(double bytes_ahead, double playback_rate);
}
