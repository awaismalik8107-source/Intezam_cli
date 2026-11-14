#pragma once

#include "cache_file.hpp"
#include "cache_geometry.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace intezam {

struct SlotInventory {
    std::size_t total = 0;
    std::size_t occupied = 0;
    std::size_t free = 0;
    std::size_t reserved_target = 0;
    std::size_t reserved_free = 0;
    std::size_t prefetch_free = 0;
    std::size_t protected_slots = 0;
    std::size_t pinned_slots = 0;
};

struct AdmissionResult {
    std::vector<int> admitted;
    std::vector<int> deferred_required;
    std::vector<int> deferred_prefetch;
};

class PieceStore : public std::enable_shared_from_this<PieceStore> {
public:
    class ReadLease {
    public:
        ReadLease(ReadLease&& other) noexcept;
        ReadLease& operator=(ReadLease&& other) noexcept;
        ~ReadLease();
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        std::vector<char> read() const;
    private:
        friend class PieceStore;
        ReadLease(std::shared_ptr<PieceStore> store, std::int64_t offset, std::int64_t length,
                  std::vector<int> pieces);
        void release();
        std::shared_ptr<PieceStore> store_;
        std::int64_t offset_ = 0;
        std::int64_t length_ = 0;
        std::vector<int> pieces_;
    };

    static std::shared_ptr<PieceStore> create(const PieceGeometry& geometry,
        const std::filesystem::path& root = CacheFile::defaultRoot(),
        std::optional<std::size_t> reserved_slots = {});
    std::size_t slotCount() const { return file_->slotCount(); }
    static std::size_t automaticReservation(std::size_t slots);
    std::size_t reservedSlots() const;
    void setReservedSlots(std::size_t slots);
    SlotInventory inventory() const;
    void setProtectedPieces(const std::vector<int>& pieces);
    // Uses free slots only; required reads may borrow reserved capacity.
    AdmissionResult admit(const std::vector<int>& required, const std::vector<int>& prefetch);
    std::vector<int> reclaimablePieces(const std::vector<int>& keep) const;
    // Requires the same quiescent torrent boundary as reconcile(). Rechecks pins.
    std::vector<int> reclaimOutside(const std::vector<int>& keep);
    std::int64_t budgetBytes() const { return file_->budgetBytes(); }
    std::int64_t allocatedBytes() const;
    const std::filesystem::path& directory() const { return file_->directory(); }
    bool needsEviction(const std::vector<int>& desired) const;
    // Caller must quiesce libtorrent before calling reconcile() if needsEviction()
    // is true. It preserves overlap and pins; unadmitted target pieces stay missing.
    std::vector<int> reconcile(const std::vector<int>& desired);
    std::vector<int> residentPieces() const;
    std::vector<int> verifiedPieces() const;
    bool contains(int piece) const;
    bool isVerified(int piece) const;
    void markVerified(int piece);
    void invalidate(int piece);
    void write(int piece, int offset, const char* bytes, std::size_t length);
    std::vector<char> readRaw(int piece, int offset, int length) const;
    std::vector<bool> writtenBlocks(int piece) const;
    bool isRangeCached(std::int64_t offset, std::int64_t length) const;
    std::optional<ReadLease> acquire(std::int64_t offset, std::int64_t length);

private:
    struct Slot {
        std::size_t index;
        bool verified = false;
        std::size_t pins = 0;
        std::vector<std::pair<int, int>> written;
    };
    PieceStore(PieceGeometry geometry, std::unique_ptr<CacheFile> file, std::optional<std::size_t> reserved_slots);
    void validatePieces(const std::vector<int>& pieces) const;
    bool admitPieceLocked(int piece);
    bool reclaimableLocked(int piece, const Slot& slot, const std::set<int>& keep) const;
    bool rangeCachedLocked(const std::vector<int>& pieces) const;
    static void recordWrite(Slot& slot, int begin, int end);
    const PieceGeometry geometry_;
    std::unique_ptr<CacheFile> file_;
    mutable std::mutex mutex_;
    std::map<int, Slot> slots_;
    std::set<int> protected_pieces_;
    std::size_t reserved_slots_ = 0;
};

} // namespace intezam
