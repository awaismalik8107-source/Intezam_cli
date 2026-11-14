#pragma once
#include "cache_budget.hpp"
#include "cache_geometry.hpp"
#include "useful_throughput.hpp"
#include <cstddef>
#include <optional>

namespace intezam {
struct AdaptiveCacheInput {
    double consumption_bytes_per_second = 1024 * 1024;
    ThroughputEstimate throughput;
    double verified_forward_seconds = 0;
    std::size_t outside_protected_pieces = 0; // union outside rolling range, counted once
    std::size_t capacity_limit_slots = 0;
    std::size_t minimum_required_slots = 0; // essential pieces can bypass target cooldown
};
struct AdaptiveCacheTarget {
    std::size_t desired_slots = 0;
    std::size_t required_slots = 0; // uncapped requirement, for diagnostics
    double forward_seconds = 0;
    double startup_seconds = 0;
    double low_seconds = 0;
    double emergency_seconds = 0;
    double resume_seconds = 0;
    double depletion_seconds = 0; // infinity if sustainable; zero if no buffer
    bool throughput_deficit = false;
    bool capacity_limited = false;
};

// Pure policy. Times are monotonic seconds supplied by the caller/test clock.
class AdaptiveCacheBudgetController {
public:
    AdaptiveCacheBudgetController(PieceGeometry geometry, AdaptiveCacheConfig config);
    AdaptiveCacheTarget update(const AdaptiveCacheInput& input, double elapsed_seconds);
private:
    PieceGeometry geometry_;
    AdaptiveCacheConfig config_;
    std::optional<std::size_t> desired_slots_;
    double last_change_ = 0;
    double pending_since_ = 0;
    int pending_direction_ = 0;
};
}
