#include "dynamic_segment_cache.hpp"
#include "piece_store.hpp"

#include <filesystem>
#include <cerrno>
#include <csignal>
#include <future>
#include <iostream>
#include <linux/magic.h>
#include <stdexcept>
#include <vector>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/vfs.h>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try {
        auto pattern = (fs::current_path() / "intezam-cache-test-XXXXXX").string();
        const auto path = mkdtemp(pattern.data());
        require(path != nullptr, "temporary directory");
        const fs::path root{path};
        try {
            const intezam::PieceGeometry geometry{128LL * 1024 * 1024, 7000,
                128LL * 1024 * 1024 + 20000, 65536};
            auto store = intezam::PieceStore::create(geometry, root);
            const auto directory = store->directory();
            require(store->budgetBytes() == geometry.video_size / 40, "2.5 percent budget");
            require(store->allocatedBytes() <= store->budgetBytes(), "allocated blocks within budget");
            require(store->slotCount() > 4, "capacity is not fixed at four slots");

            intezam::DynamicSegmentCache cache(geometry, store->slotCount(), 262144);
            cache.updatePlaybackPosition(geometry.video_size / 2);
            cache.updateNetworkSpeed(4 * 1024 * 1024);
            const auto fast = cache.window();
            require(fast.current_segment - fast.first_segment == fast.segmentCount() / 3,
                    "one-third behind at mid-file");
            require(fast.pieces.size() <= store->slotCount(), "whole pieces fit");
            cache.updateBufferHealth(10);
            require(cache.window().segmentCount() < fast.segmentCount(), "healthy buffer reduces startup cushion");
            cache.updateVideoBitrate(131072);
            require(cache.window().segmentCount() < fast.segmentCount(), "consumption rate affects window sizing");
            cache.updateVideoBitrate(262144);
            cache.updateBufferHealth(0);
            for (int sample = 0; sample < 20; ++sample) cache.updateNetworkSpeed(16384);
            const auto slow = cache.window();
            require(slow.segmentCount() != fast.segmentCount(), "adapt to observed throughput");
            cache.updatePlaybackPosition(0);
            require(cache.window().first_segment == 0, "begin boundary");
            cache.updatePlaybackPosition(geometry.video_size - 1);
            require(cache.window().end_byte == geometry.video_size, "end boundary");

            const auto pieces = geometry.piecesForRange(0, 100000);
            require(pieces.front() == 0 && pieces.back() == 1, "torrent-global unaligned mapping");
            store->reconcile(pieces);
            for (int piece : pieces) {
                std::vector<char> bytes(geometry.pieceSize(piece), static_cast<char>(piece + 1));
                store->write(piece, 0, bytes.data(), bytes.size());
                require(!store->isRangeCached(0, 100000), "incomplete verification is not a hit");
            }
            for (int piece : pieces) store->markVerified(piece);
            require(store->isRangeCached(0, 100000), "verified range cached");
            auto lease = store->acquire(0, 100000);
            require(lease.has_value(), "pin verified range");
            auto bytes = lease->read();
            require(bytes.front() == 1 && bytes.back() == 2, "read crosses correct piece offsets");

            std::vector<int> distant;
            for (std::size_t n = 0; n < store->slotCount(); ++n) distant.push_back(100 + n);
            auto reading = std::async(std::launch::async, [&] {
                for (int iteration = 0; iteration < 20; ++iteration) {
                    const auto read = lease->read();
                    require(read.front() == 1 && read.back() == 2, "concurrent pinned read is stable");
                }
            });
            for (int iteration = 0; iteration < 20; ++iteration) store->reconcile(distant);
            reading.get();
            require(store->isRangeCached(0, 100000), "active reader preserved across seek");
            require(store->residentPieces().size() <= store->slotCount(), "pinned reads cannot grow cache");
            lease.reset();
            store->reconcile(distant);
            require(!store->isRangeCached(0, 100000), "unpinned old pieces evicted");
            require(!store->acquire(0, 100000), "uncached seek returns miss");
            require(store->allocatedBytes() <= store->budgetBytes(), "disk bound after repeated reuse");
            std::cout << "Disk allocation: " << store->allocatedBytes()
                      << " bytes; budget: " << store->budgetBytes() << " bytes\n";
            intezam::CacheFile::recoverStale(root);
            require(fs::exists(directory), "stale cleanup preserves a live locked session");
            store.reset();
            require(!fs::exists(directory), "normal cleanup removes session");

            bool rejected = false;
            try { intezam::PieceStore::create({100000, 0, 100000, 65536}, root); }
            catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "reject budget smaller than one complete piece");
            require(fs::is_empty(root), "failed allocation leaves no session artifacts");

            const auto child = fork();
            require(child >= 0, "fork crash simulation");
            if (child == 0) {
                try {
                    auto abandoned = intezam::PieceStore::create(geometry, root);
                    _exit(0); // simulate abrupt exit without RAII cleanup
                } catch (...) { _exit(1); }
            }
            int status = 0;
            require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "crash fixture created");
            require(!fs::is_empty(root), "crash left temporary session data");
            intezam::CacheFile::recoverStale(root);
            require(fs::is_empty(root), "stale session recovered using unlocked session lock");

            const auto limited = fork();
            require(limited >= 0, "fork allocation-failure simulation");
            if (limited == 0) {
                std::signal(SIGXFSZ, SIG_IGN);
                const rlimit limit{32768, 32768};
                if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(1);
                try {
                    auto oversized = intezam::PieceStore::create(geometry, root);
                } catch (const std::system_error& error) {
                    _exit(error.code().value() == EFBIG && fs::is_empty(root) ? 0 : 1);
                } catch (...) { _exit(1); }
                _exit(1);
            }
            require(waitpid(limited, &status, 0) == limited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "allocation failure is reported and leaves no artifacts");

            const auto temporary = fs::temp_directory_path();
            struct statfs filesystem{};
            if (statfs(temporary.c_str(), &filesystem) == 0
                && (filesystem.f_type == TMPFS_MAGIC || filesystem.f_type == RAMFS_MAGIC)) {
                auto memory_pattern = (temporary / "intezam-ram-test-XXXXXX").string();
                const auto memory_directory = mkdtemp(memory_pattern.data());
                require(memory_directory != nullptr, "RAM filesystem fixture");
                const fs::path memory_root{memory_directory};
                bool refused = false;
                try { intezam::PieceStore::create(geometry, memory_root); }
                catch (const std::runtime_error& error) {
                    refused = std::string(error.what()).find("tmpfs/ramfs") != std::string::npos;
                }
                const bool empty = fs::is_empty(memory_root);
                fs::remove_all(memory_root);
                require(refused && empty, "RAM-backed storage rejected without creating cache data");
            }
        } catch (...) {
            fs::remove_all(root);
            throw;
        }
        fs::remove_all(root);
        std::cout << "Cache allocation, adaptation, seeking and cleanup tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
