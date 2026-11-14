#pragma once
#include <cstdint>

namespace intezam {
enum class CacheBudgetMode { automatic, strict };
struct CacheBudgetOptions {
    CacheBudgetMode mode = CacheBudgetMode::automatic;
    int maximum_percent = 25;
};
struct CacheBudgetPlan {
    std::int64_t target_bytes;
    std::int64_t effective_bytes;
    std::int64_t slot_bytes;
};
// Automatic budgets are fixed for the session, and cover at least four pieces.
CacheBudgetPlan planCacheBudget(std::int64_t video_size, int piece_length,
    std::int64_t allocation_unit, std::int64_t overhead, CacheBudgetOptions options);
}
