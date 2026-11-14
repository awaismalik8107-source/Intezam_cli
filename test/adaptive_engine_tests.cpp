#include "adaptive_cache_budget.hpp"
#include "stream_policy.hpp"
#include "cache_file.hpp"
#include "dynamic_segment_cache.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace intezam;
constexpr std::int64_t MiB = 1024 * 1024;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

ThroughputEstimate stable(double rate)
{
    UsefulThroughputEstimator estimator;
    for (int i = 0; i < 40; ++i) estimator.observe(rate * 0.5, 0.5, true, 4);
    return estimator.estimate();
}

void estimatesDemandInsteadOfIdleTime()
{
    UsefulThroughputEstimator estimator;
    for (int i = 0; i < 40; ++i) estimator.observe(MiB / 2, 0.5, true, 3);
    const auto baseline = estimator.estimate();
    require(std::abs(baseline.sustainable_bytes_per_second - MiB) < MiB / 10, "stable useful throughput");
    estimator.observe(20 * MiB, 0.5, true, 3);
    require(estimator.estimate().sustainable_bytes_per_second < 2 * MiB, "single spike cannot inflate sustainable rate");
    for (int i = 0; i < 40; ++i) estimator.observe(0, 0.5, false, 0);
    require(estimator.estimate().stalled_seconds == 0, "full-cache idle is not a network outage");
    for (int i = 0; i < 12; ++i) estimator.observe(0, 0.5, true, 0);
    require(estimator.estimate().stalled_seconds >= 6, "pending-demand stall detected");
    require(estimator.estimate().sustainable_bytes_per_second < baseline.sustainable_bytes_per_second / 2, "sustained outage lowers estimate");
    require(estimator.estimate().peers == 0, "peer scarcity retained separately");
    UsefulThroughputEstimator fluctuating;
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (double rate : {2.0, 0.2, 1.5, 0.0}) fluctuating.observe(rate * MiB, 1, true, 2);
    }
    const auto unstable = fluctuating.estimate();
    require(unstable.variability > 0.5 && unstable.sustainable_bytes_per_second < unstable.long_bytes_per_second,
        "fluctuating delivery produces a conservative sustainable rate and safety signal");
}

void sizesByConsumptionAndRisk()
{
    AdaptiveCacheConfig config;
    AdaptiveCacheBudgetController small({512 * MiB, 0, 512 * MiB, int(MiB)}, config);
    AdaptiveCacheBudgetController large({10LL * 1024 * MiB, 0, 10LL * 1024 * MiB, int(MiB)}, config);
    AdaptiveCacheInput input;
    input.consumption_bytes_per_second = MiB / 2;
    input.throughput = stable(5 * MiB);
    input.capacity_limit_slots = 256;
    const auto a = small.update(input, 0), b = large.update(input, 0);
    require(a.desired_slots == b.desired_slots, "video size is not a percentage multiplier");
    AdaptiveCacheBudgetController high({10LL * 1024 * MiB, 0, 10LL * 1024 * MiB, int(MiB)}, config);
    input.consumption_bytes_per_second = 2 * MiB;
    input.throughput = stable(MiB);
    input.verified_forward_seconds = 12;
    const auto deficit = high.update(input, 0);
    require(deficit.throughput_deficit, "insufficient throughput detected");
    require(deficit.depletion_seconds > 20 && deficit.depletion_seconds < 26, "depletion uses net consumption");
    require(deficit.desired_slots > a.desired_slots, "bitrate and deficit require more capacity");
    require(deficit.startup_seconds > a.startup_seconds, "deficit gets larger initial cushion");
    input.verified_forward_seconds = 0;
    require(high.update(input, 1).required_slots > deficit.required_slots,
        "depleted consecutive buffer increases the safety requirement under marginal delivery");
    input.verified_forward_seconds = 12;
    input.outside_protected_pieces = 5;
    const auto protected_target = high.update(input, 30);
    require(protected_target.required_slots >= deficit.required_slots + 5, "deduplicated external protection allowance");
    input.capacity_limit_slots = 8;
    const auto clipped = high.update(input, 31);
    require(clipped.desired_slots <= 8 && clipped.capacity_limited, "absolute/disk capacity clipping");
    require(clipped.resume_seconds < clipped.forward_seconds, "reachable separate watermarks");
}

void resizesWithHysteresis()
{
    AdaptiveCacheConfig config;
    AdaptiveCacheBudgetController controller({4LL * 1024 * MiB, 0, 4LL * 1024 * MiB, int(MiB)}, config);
    AdaptiveCacheInput input;
    input.capacity_limit_slots = 256;
    input.consumption_bytes_per_second = MiB / 2;
    input.throughput = stable(5 * MiB);
    const auto initial = controller.update(input, 0);
    input.throughput.variability = 1;
    input.throughput.sustainable_bytes_per_second = MiB / 4;
    require(controller.update(input, 1).desired_slots == initial.desired_slots, "brief fluctuation does not resize");
    const auto grown = controller.update(input, 20);
    require(grown.desired_slots > initial.desired_slots, "sustained risk requests growth");
    input.throughput = stable(5 * MiB);
    require(controller.update(input, 21).desired_slots == grown.desired_slots, "brief recovery does not shrink");
    require(controller.update(input, 90).desired_slots < grown.desired_slots, "stable recovery requests deferred shrink");
    AdaptiveCacheBudgetController late_spike({4LL * 1024 * MiB, 0, 4LL * 1024 * MiB, int(MiB)}, config);
    const auto stable_target = late_spike.update(input, 0);
    input.throughput.variability = 1;
    input.throughput.sustainable_bytes_per_second = MiB / 4;
    require(late_spike.update(input, 100).desired_slots == stable_target.desired_slots,
        "a spike after a long idle cooldown still needs confirmation");
    input.minimum_required_slots = 200;
    require(late_spike.update(input, 101).desired_slots >= 200,
        "essential protected pieces bypass speculative target cooldown");
}

void enforcesAlignedAbsoluteStorageLimits()
{
    CacheBudgetOptions budget{CacheBudgetMode::adaptive, 25};
    budget.initial_consumption_bytes_per_second = MiB / 2;
    const auto a = planCacheBudget(512 * MiB, MiB, 4096, 8192, budget);
    const auto b = planCacheBudget(10LL * 1024 * MiB, MiB, 4096, 8192, budget);
    require(a.effective_bytes == b.effective_bytes, "equal bitrate videos use equal initial disk allocation");
    budget.initial_consumption_bytes_per_second = 2 * MiB;
    require(planCacheBudget(512 * MiB, MiB, 4096, 8192, budget).effective_bytes > a.effective_bytes,
        "higher bitrate increases initial allocation");
    budget.adaptive.maximum_bytes = 8 * MiB;
    const auto aligned = planCacheBudget(512 * MiB, MiB, 6144, 8192, budget);
    require(aligned.slot_bytes % 6144 == 0 && aligned.effective_bytes <= 8 * MiB, "aligned slots respect absolute maximum");
    const auto small = planCacheBudget(MiB, MiB, 4096, 8192, budget);
    require((small.effective_bytes - 12288) / MiB >= 4, "small videos retain viable staging slots");
    bool rejected = false;
    try { planCacheBudget(512 * MiB, MiB, 4096, 8192, budget, budget.adaptive.free_space_reserve_bytes + MiB); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "available disk reserve limits allocation");
    budget.adaptive.maximum_bytes = MiB;
    rejected = false;
    try { planCacheBudget(512 * MiB, MiB, 4096, 8192, budget); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "impossible minimum fails instead of exceeding maximum");
}

void distinguishesBufferStatesAndPriorities()
{
    const PieceGeometry geometry{8 * MiB, 7000, 8 * MiB + 7000, int(MiB)};
    const auto scattered = [](int piece) { return piece == 0 || piece == 1 || piece == 3; };
    require(verifiedForwardBytes(geometry, 0, scattered) == 2 * MiB - 7000,
        "a scattered verified piece beyond the first hole is not playable forward buffer");
    require(verifiedForwardBytes(geometry, 2 * MiB, scattered) == 0,
        "a missing current piece gives zero forward data despite retained pieces");
    require(verifiedForwardBytes(geometry, geometry.video_size - 123, [](int) { return true; }) == 123,
        "forward data clips exactly to selected-file end");
    AdaptiveBufferController buffer;
    AdaptiveBufferTargets targets{3, 1.5, 0.5, 5};
    require(buffer.update(0, targets, false, false, 0) == BufferState::prebuffering, "initial prebuffer");
    require(buffer.update(3, targets, false, false, 1) == BufferState::playing, "startup readiness");
    require(buffer.update(1, targets, false, false, 2) == BufferState::buffer_low, "warning without forced pause");
    require(buffer.update(0, targets, true, false, 0) == BufferState::recovering, "emergency recovery");
    require(buffer.update(2, targets, false, false, 0) == BufferState::recovering, "resume hysteresis");
    require(buffer.update(5, targets, false, false, 0) == BufferState::playing, "resume threshold");
    require(buffer.update(0, targets, true, true, 0) == BufferState::seeking, "seek distinguished");
    require(buffer.update(0, targets, true, false, 6) == BufferState::stalled, "sustained outage distinguished");
    require(playbackPiecePriority(true, 0, false, 5, false) == 7, "blocking piece priority");
    require(playbackPiecePriority(false, 1, false, 5, false) == 7, "imminent piece priority");
    require(playbackPiecePriority(false, 6, false, 5, false) == 6, "near future priority");
    require(playbackPiecePriority(false, 20, false, 5, false) == 4, "distant prefetch priority");
    require(playbackPiecePriority(false, -1, false, 5, false) == 1, "backward retention priority");
}

void preservesRollingAlignmentWhenAdaptiveCapacityChanges()
{
    const PieceGeometry geometry{512 * MiB, 7000, 512 * MiB + 7000, int(MiB)};
    DynamicSegmentCache window(geometry, 64, MiB / 2);
    window.setAdaptiveForwardSeconds(12);
    window.reposition(64 * MiB);
    const auto normal = window.planWindow();
    const auto behind = normal.current_segment - normal.first_segment;
    require(std::abs(double(behind) / normal.segmentCount() - 1.0 / 3) < 0.1,
        "adaptive horizon keeps one-third backward positioning away from boundaries");
    window.updateCapacity(8);
    const auto constrained = window.planWindow({400});
    require(constrained.pieces.size() + 1 <= 8, "far metadata and constrained window share aligned slots");
    window.reposition(2 * MiB);
    require(window.planWindow().begin_byte <= 2 * MiB, "backward reposition includes requested playback byte");
}

int main()
{
    try { estimatesDemandInsteadOfIdleTime(); sizesByConsumptionAndRisk(); resizesWithHysteresis();
        enforcesAlignedAbsoluteStorageLimits(); distinguishesBufferStatesAndPriorities(); preservesRollingAlignmentWhenAdaptiveCapacityChanges(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
