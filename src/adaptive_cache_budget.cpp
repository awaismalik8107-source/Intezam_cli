#include "adaptive_cache_budget.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace intezam {
AdaptiveCacheBudgetController::AdaptiveCacheBudgetController(PieceGeometry geometry, AdaptiveCacheConfig config)
    : geometry_(geometry), config_(config)
{
    geometry.validate();
    if (config.maximum_bytes <= 0 || config.free_space_reserve_bytes < 0 || config.minimum_slots < 4
        || config.minimum_slots > 1000000 || !std::isfinite(config.minimum_forward_seconds)
        || config.minimum_forward_seconds < 1 || !std::isfinite(config.maximum_forward_seconds)
        || config.maximum_forward_seconds > 600 || config.maximum_forward_seconds < config.minimum_forward_seconds
        || !std::isfinite(config.initial_forward_seconds) || config.initial_forward_seconds < config.minimum_forward_seconds
        || config.initial_forward_seconds > config.maximum_forward_seconds
        || !std::isfinite(config.growth_interval_seconds) || config.growth_interval_seconds < 1
        || !std::isfinite(config.shrink_interval_seconds) || config.shrink_interval_seconds < config.growth_interval_seconds) {
        throw std::invalid_argument("Invalid adaptive cache limits, buffer durations, or resize intervals.");
    }
}

AdaptiveCacheTarget AdaptiveCacheBudgetController::update(const AdaptiveCacheInput& input, double elapsed)
{
    const auto rate = input.consumption_bytes_per_second;
    if (!std::isfinite(rate) || rate <= 0 || rate > 1e12 || !std::isfinite(elapsed) || elapsed < last_change_
        || !std::isfinite(input.verified_forward_seconds) || input.verified_forward_seconds < 0) {
        throw std::invalid_argument("Invalid adaptive cache observation.");
    }
    const auto& network = input.throughput;
    if (!std::isfinite(network.short_bytes_per_second) || network.short_bytes_per_second < 0
        || !std::isfinite(network.long_bytes_per_second) || network.long_bytes_per_second < 0
        || !std::isfinite(network.sustainable_bytes_per_second) || network.sustainable_bytes_per_second < 0
        || !std::isfinite(network.variability) || network.variability < 0
        || !std::isfinite(network.stalled_seconds) || network.stalled_seconds < 0) {
        throw std::invalid_argument("Invalid useful-throughput estimate.");
    }
    const bool measured = network.long_bytes_per_second > 0;
    const double ratio = measured ? network.sustainable_bytes_per_second / rate : 0;
    const double variability = std::clamp(network.variability, 0.0, 2.0);
    double forward = measured ? config_.minimum_forward_seconds * (ratio >= 2 ? 1.0 : ratio >= 1.1 ? 1.5 : 2.5)
        : config_.initial_forward_seconds;
    if (measured && ratio < 1.1 && input.verified_forward_seconds < config_.minimum_forward_seconds / 3) {
        forward += config_.minimum_forward_seconds / 2;
    }
    forward += config_.minimum_forward_seconds * variability + std::min(20.0, network.stalled_seconds * 2);
    forward = std::clamp(forward, config_.minimum_forward_seconds, config_.maximum_forward_seconds);
    // Video length is only a useful-data ceiling. The extra alignment piece
    // covers arbitrary selected-file offset; inflight slots inside this range
    // are already counted, and external protection is supplied as a union.
    const auto useful = static_cast<std::size_t>(
        (geometry_.video_offset % geometry_.piece_length + geometry_.video_size - 1) / geometry_.piece_length + 1);
    const double rolling_slots = std::ceil(rate * forward * 1.5 / geometry_.piece_length);
    auto working = static_cast<std::size_t>(std::min<double>(useful, rolling_slots)) + 1;
    working = std::min(useful, working);
    working = std::min(useful, working + std::min(input.outside_protected_pieces, useful - working));
    // Reservation is ceil(total/8), so solve for total rather than adding
    // working/8 (which would underfund the working window). Two staging slots
    // leave room for the current piece and a partial write.
    const auto staging = working + 2;
    const auto required = std::max({config_.minimum_slots, staging + (staging + 6) / 7, input.minimum_required_slots});
    auto candidate = std::min(required, input.capacity_limit_slots);
    if (!desired_slots_) desired_slots_ = candidate;
    else {
        const auto previous = *desired_slots_;
        const auto difference = candidate > previous ? candidate - previous : previous - candidate;
        const auto interval = candidate > previous ? config_.growth_interval_seconds : config_.shrink_interval_seconds;
        const bool meaningful = difference >= std::max<std::size_t>(2, previous / 4);
        const int direction = meaningful ? (candidate > previous ? 1 : -1) : 0;
        if (pending_direction_ != direction) { pending_direction_ = direction; pending_since_ = elapsed; }
        if (previous < std::min(input.minimum_required_slots, input.capacity_limit_slots) || previous > input.capacity_limit_slots ||
            (meaningful && elapsed - last_change_ >= interval && elapsed - pending_since_ >= 2)) {
            desired_slots_ = candidate;
            last_change_ = elapsed;
            pending_direction_ = 0;
        }
    }
    candidate = *desired_slots_;
    const double available_forward = std::max(0.0,
        double(candidate > 3 ? candidate - 3 : 1) * geometry_.piece_length * (2.0 / 3.0) / rate);
    forward = std::min(forward, available_forward);
    const double startup = std::min(forward * 0.6,
        measured ? (ratio >= 2 ? 3 : ratio >= 1.1 ? 6 : 16) + variability * 4 : 8);
    const double deficit = measured ? std::max(0.0, rate - network.sustainable_bytes_per_second) : 0;
    return {candidate, required, forward, startup, std::min(3.0, startup / 2), std::min(0.5, startup / 4),
        std::min(forward * 0.8, startup * 1.25),
        deficit > 0 ? input.verified_forward_seconds * rate / deficit : std::numeric_limits<double>::infinity(),
        deficit > 0, required > candidate};
}
}
