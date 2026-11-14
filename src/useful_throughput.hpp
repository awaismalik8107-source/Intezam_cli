#pragma once
#include <cstdint>

namespace intezam {
struct ThroughputEstimate {
    double short_bytes_per_second = 0;
    double long_bytes_per_second = 0;
    double sustainable_bytes_per_second = 0;
    double variability = 0;
    double stalled_seconds = 0;
    double observation_seconds = 0;
    int peers = 0;
};

// Callers serialize observations. Bytes count newly verified selected-file data,
// not duplicate payload, hash failures, or reconstructed completed-piece state.
class UsefulThroughputEstimator {
public:
    void observe(std::uint64_t verified_bytes, double seconds, bool demand_pending, int peers);
    ThroughputEstimate estimate() const;
private:
    ThroughputEstimate estimate_;
    double deviation_ = 0;
    bool observed_ = false;
};
}
