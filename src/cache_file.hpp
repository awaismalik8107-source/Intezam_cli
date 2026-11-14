#pragma once

#include "cache_budget.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <atomic>
#include <string>

namespace intezam {

// Owns one preallocated file. Session files, session and root directory blocks count
// toward budgetBytes(); mappings are kept in memory, not extra disk files.
class CacheFile {
public:
    static std::unique_ptr<CacheFile> create(std::int64_t video_size, int piece_length,
                                            const std::filesystem::path& root,
                                            CacheBudgetOptions options = {CacheBudgetMode::strict, 25});
    static std::filesystem::path defaultRoot();
    static void recoverStale(const std::filesystem::path& root);
    ~CacheFile();
    CacheFile(const CacheFile&) = delete;
    CacheFile& operator=(const CacheFile&) = delete;

    void write(std::int64_t offset, const char* bytes, std::size_t length);
    void read(std::int64_t offset, char* bytes, std::size_t length) const;
    std::int64_t allocatedBytes() const;
    std::int64_t budgetBytes() const { return budget_; }
    std::int64_t slotSize() const { return slot_size_; }
    std::size_t slotCount() const { return slots_.load(); }
    std::size_t growthLimitSlots() const;
    // Caller holds PieceStore's I/O lock. Only appends; offsets never change.
    bool growToSlots(std::size_t slots);
    const std::string& growthError() const { return growth_error_; }
    const std::filesystem::path& directory() const { return directory_; }

private:
    CacheFile() = default;
    void initialize(std::int64_t video_size, int piece_length, const std::filesystem::path& root, CacheBudgetOptions options);
    std::filesystem::path directory_;
    std::filesystem::path root_;
    int fd_ = -1;
    int lock_fd_ = -1;
    std::int64_t budget_ = 0;
    std::int64_t slot_size_ = 0;
    std::atomic<std::size_t> slots_{0};
    std::int64_t free_space_reserve_ = 0;
    bool adaptive_ = false;
    std::string growth_error_;
};

} // namespace intezam
