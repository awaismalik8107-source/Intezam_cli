#pragma once
#include <cstdint>
#include <optional>
namespace intezam {
struct BufferWatermarks { double low_seconds; double high_seconds; };
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
