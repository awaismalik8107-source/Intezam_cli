#pragma once

#include <cstddef>
#include <cstdint>

namespace intezam {

struct CachePerformance {
    std::uint64_t rebuild_microseconds = 0;
    std::uint64_t maximum_rebuild_microseconds = 0;
    std::uint64_t pieces_evicted = 0;
    std::uint64_t pieces_admitted = 0;
    std::uint64_t window_movements = 0;
    std::uint64_t priority_updates = 0;
    std::uint64_t deadline_updates = 0;
    std::uint64_t deadline_resets = 0;
    std::uint64_t proactive_refills = 0;
    std::uint64_t disk_jobs = 0;
    std::uint64_t disk_microseconds = 0;
    std::size_t total_slots = 0;
    std::size_t free_slots = 0;
    std::size_t reserved_slots = 0;
    std::size_t protected_slots = 0;
    std::size_t pinned_slots = 0;
};

struct ReadPerformance {
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t wait_microseconds = 0;
    std::uint64_t maximum_wait_microseconds = 0;
    std::uint64_t stalls = 0; // completed/cancelled waits lasting at least 50 ms
    std::uint64_t stall_microseconds = 0;
    std::uint64_t disk_read_microseconds = 0;
};

} // namespace intezam
