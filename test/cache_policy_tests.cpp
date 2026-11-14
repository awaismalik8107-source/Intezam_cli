#include "cache_budget.hpp"
#include "stream_policy.hpp"
#include "cache_file.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <unistd.h>

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

int main()
{
    try {
        using namespace intezam;
        const auto sample = planCacheBudget(60879760, 2097152, 4096, 8192, {});
        require(sample.target_bytes == 1521994, "keep relative 2.5% target");
        require(sample.effective_bytes == 8400896, "minimum four aligned pieces plus measured overhead and reserve");
        const auto movie = planCacheBudget(5869802096, 2097152, 4096, 8192, {});
        require(movie.effective_bytes == movie.target_bytes, "large video retains original budget");
        bool rejected = false;
        try { planCacheBudget(1024*1024, 2097152, 4096, 8192, {}); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "automatic ceiling rejects unreasonable expansion");
        rejected = false;
        try { planCacheBudget(60879760, 2097152, 4096, 8192, {CacheBudgetMode::strict, 25}); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "strict mode still rejects too-small target");
        rejected = false;
        try { planCacheBudget(60879760, 2097152, 4096, 8192, {CacheBudgetMode::automatic, 10}); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "user configured ceiling is enforced");
        rejected = false;
        try { planCacheBudget(std::numeric_limits<std::int64_t>::max(), 2097152,
                std::numeric_limits<std::int64_t>::max() / 8,
                std::numeric_limits<std::int64_t>::max() / 2, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "overflowing allocation dimensions are rejected");
        const auto root = CacheFile::defaultRoot() / ("adaptive-policy-test-" + std::to_string(getpid()));
        {
            const auto file = CacheFile::create(60879760, 2097152, root, {});
            require(file->slotCount() >= 4, "real filesystem has four usable slots");
            require(file->allocatedBytes() <= file->budgetBytes(), "real physical allocation respects effective budget");
            require(file->budgetBytes() <= 60879760 / 4, "real allocation respects ceiling");
        }
        require(std::filesystem::is_empty(root), "session cleaned up");
        std::filesystem::remove(root);
        const auto fast = bufferWatermarks(1e6, 3e6, 100e6);
        const auto slow = bufferWatermarks(1e6, 0.5e6, 100e6);
        require(fast.high_seconds == 6 && slow.high_seconds == 12, "approved smooth playback startup targets");
        require(bufferWatermarks(1e6, 1e6, 100e6).high_seconds == 8, "near-parity safety target");
        require(slow.high_seconds > fast.high_seconds, "slow swarm gets larger startup buffer");
        require(slow.low_seconds < slow.high_seconds, "rebuffer hysteresis");
        require(bufferWatermarks(1e6, 0, 100000).high_seconds <= 0.1, "tiny cache target is reachable");
        RebufferGate gate;
        require(!gate.update(true, false, true, 0.1, {0.5, 2.0}, 0), "brief miss does not pause");
        require(!gate.update(true, false, true, 0.1, {0.5, 2.0}, 200), "debounce transient stall");
        require(gate.update(true, false, true, 0.1, {0.5, 2.0}, 300), "sustained low buffer pauses");
        require(gate.update(false, false, false, 0.8, {0.5, 2.0}, 400), "one recovered read cannot bypass high watermark");
        require(gate.update(false, true, false, 3.0, {0.5, 2.0}, 500), "manual pause prevents automatic resume");
        require(!gate.update(false, false, false, 3.0, {0.5, 2.0}, 600), "resume only when high watermark is reached");
        require(pieceDeadlineMilliseconds(0, 1e6) == 0, "urgent piece deadline is immediate");
        require(pieceDeadlineMilliseconds(4e6, 1e6) > pieceDeadlineMilliseconds(2e6, 1e6), "farther pieces have later deadlines");
        require(pieceDeadlineMilliseconds(4e6, 2e6) < pieceDeadlineMilliseconds(4e6, 1e6), "higher bitrate tightens deadline");
        std::cout << "Adaptive budget and streaming policy tests passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
