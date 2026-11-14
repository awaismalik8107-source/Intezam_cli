#include "useful_throughput.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace intezam {
void UsefulThroughputEstimator::observe(std::uint64_t bytes, double seconds, bool pending, int peers)
{
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600 || peers < 0) {
        throw std::invalid_argument("Invalid useful-throughput observation.");
    }
    estimate_.peers = peers;
    if (!pending && bytes == 0) { estimate_.stalled_seconds = 0; return; }
    const double rate = bytes / seconds;
    estimate_.stalled_seconds = bytes ? 0 : estimate_.stalled_seconds + seconds;
    estimate_.observation_seconds += seconds;
    // An initial zero is a real demand stall, but cannot establish link capacity.
    if (!observed_ && bytes == 0) return;
    if (!observed_) {
        estimate_.short_bytes_per_second = rate;
        estimate_.long_bytes_per_second = rate;
        observed_ = true;
        return;
    }
    const double short_weight = -std::expm1(-seconds / 2.0);
    const double long_weight = -std::expm1(-seconds / 12.0);
    deviation_ += long_weight * (std::abs(rate - estimate_.long_bytes_per_second) - deviation_);
    estimate_.short_bytes_per_second += short_weight * (rate - estimate_.short_bytes_per_second);
    estimate_.long_bytes_per_second += long_weight * (rate - estimate_.long_bytes_per_second);
}

ThroughputEstimate UsefulThroughputEstimator::estimate() const
{
    auto result = estimate_;
    result.variability = std::clamp(deviation_ / std::max(1.0, result.long_bytes_per_second), 0.0, 2.0);
    result.sustainable_bytes_per_second = std::max(0.0,
        std::min(result.short_bytes_per_second, result.long_bytes_per_second) - 0.5 * deviation_);
    if (result.stalled_seconds >= 2) {
        result.sustainable_bytes_per_second *= std::exp(-(result.stalled_seconds - 2) / 3);
    }
    return result;
}
}
