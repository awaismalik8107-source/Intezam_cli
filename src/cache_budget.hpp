#pragma once
#include <cstdint>
#include <cstddef>

namespace intezam {
enum class CacheBudgetMode { automatic, strict, adaptive };
struct AdaptiveCacheConfig {
    std::int64_t maximum_bytes = 256LL * 1024 * 1024;
    std::int64_t free_space_reserve_bytes = 256LL * 1024 * 1024;
    std::size_t minimum_slots = 4;
    double initial_forward_seconds = 24;
    double minimum_forward_seconds = 12;
    double maximum_forward_seconds = 90;
    double growth_interval_seconds = 15;
    double shrink_interval_seconds = 60;
};
struct CacheBudgetOptions {
    CacheBudgetMode mode = CacheBudgetMode::automatic;
    int maximum_percent = 25;
    AdaptiveCacheConfig adaptive{};
    double initial_consumption_bytes_per_second = 1024 * 1024;
};
struct CacheBudgetPlan {
    std::int64_t target_bytes;
    std::int64_t effective_bytes;
    std::int64_t slot_bytes;
};
// Automatic budgets are fixed for the session, and cover at least four pieces.
CacheBudgetPlan planCacheBudget(std::int64_t video_size, int piece_length,
    std::int64_t allocation_unit, std::int64_t overhead, CacheBudgetOptions options,
    std::int64_t available_bytes = INT64_MAX);
}
