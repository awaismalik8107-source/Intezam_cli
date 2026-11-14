#include "cache_budget.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace intezam {
CacheBudgetPlan planCacheBudget(std::int64_t video_size, int piece_length,
    std::int64_t allocation_unit, std::int64_t overhead, CacheBudgetOptions options)
{
    if (video_size <= 0 || piece_length <= 0 || allocation_unit <= 0 || overhead < 0
        || options.maximum_percent < 3 || options.maximum_percent > 99) {
        throw std::invalid_argument("Invalid cache dimensions or maximum percentage (expected 3..99).");
    }
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (allocation_unit > maximum / 8 || overhead > maximum / 2) {
        throw std::invalid_argument("Cache allocation dimensions overflow.");
    }
    const auto slot = ((piece_length - 1) / allocation_unit + 1) * allocation_unit;
    const auto target = video_size / 40;
    const auto minimum_slots = options.mode == CacheBudgetMode::automatic ? 4 : 1;
    if (slot > (maximum - overhead - allocation_unit) / minimum_slots) {
        throw std::invalid_argument("Cache allocation dimensions overflow.");
    }
    const auto minimum = minimum_slots * slot + overhead + allocation_unit;
    if (options.mode == CacheBudgetMode::strict) {
        if (target < minimum) throw std::runtime_error("Strict 2.5% cache budget cannot fit a whole piece plus filesystem overhead.");
        return {target, target, slot};
    }
    const auto ceiling = (video_size / 100) * options.maximum_percent
        + (video_size % 100) * options.maximum_percent / 100;
    const auto effective = std::max(target, minimum);
    if (effective > ceiling) {
        throw std::runtime_error("Four-piece cache minimum exceeds the " + std::to_string(options.maximum_percent)
            + "% automatic ceiling. Choose a larger video or raise --cache-max-percent (3..99).");
    }
    return {target, effective, slot};
}
}
