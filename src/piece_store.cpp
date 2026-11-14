#include "piece_store.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace intezam {

PieceStore::PieceStore(PieceGeometry geometry, std::unique_ptr<CacheFile> file,
                       std::optional<std::size_t> reserved_slots)
    : geometry_(geometry), file_(std::move(file))
{
    const auto slots = slotCount();
    const auto maximum = slots > 2 ? slots - 2 : 0;
    reserved_slots_ = reserved_slots.value_or(automaticReservation(slots));
    if (reserved_slots_ > maximum) throw std::invalid_argument("Reserved slots must leave at least two working slots.");
}

std::size_t PieceStore::automaticReservation(std::size_t slots)
{ return slots < 4 ? 0 : std::min(slots - 2, (slots + 7) / 8); }

std::size_t PieceStore::reservedSlots() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reserved_slots_;
}

void PieceStore::setReservedSlots(std::size_t slots)
{
    if (slots > (slotCount() > 2 ? slotCount() - 2 : 0)) {
        throw std::invalid_argument("Reserved slots must leave at least two working slots.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_slots_ = slots;
}

std::shared_ptr<PieceStore> PieceStore::create(const PieceGeometry& geometry, const std::filesystem::path& root,
                                             std::optional<std::size_t> reserved_slots)
{
    geometry.validate();
    return std::shared_ptr<PieceStore>(new PieceStore(geometry,
        CacheFile::create(geometry.video_size, geometry.piece_length, root), reserved_slots));
}

void PieceStore::validatePieces(const std::vector<int>& pieces) const
{ for (int piece : pieces) geometry_.pieceSize(piece); }

SlotInventory PieceStore::inventory() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    SlotInventory result;
    result.total = slotCount();
    result.occupied = slots_.size();
    result.free = result.total - result.occupied;
    result.reserved_target = reserved_slots_;
    result.reserved_free = std::min(result.free, reserved_slots_);
    result.prefetch_free = result.free - result.reserved_free;
    for (const auto& entry : slots_) {
        if (entry.second.pins) ++result.pinned_slots;
        if (entry.second.pins || protected_pieces_.count(entry.first)) ++result.protected_slots;
    }
    return result;
}

void PieceStore::setProtectedPieces(const std::vector<int>& pieces)
{
    validatePieces(pieces);
    std::lock_guard<std::mutex> lock(mutex_);
    protected_pieces_ = {pieces.begin(), pieces.end()};
}

bool PieceStore::admitPieceLocked(int piece)
{
    if (slots_.count(piece) || slots_.size() == slotCount()) return false;
    std::vector<bool> used(slotCount(), false);
    for (const auto& entry : slots_) used[entry.second.index] = true;
    const auto index = static_cast<std::size_t>(std::find(used.begin(), used.end(), false) - used.begin());
    std::array<char, 65536> zeroes{};
    const auto base = static_cast<std::int64_t>(index) * file_->slotSize();
    for (std::int64_t offset = 0; offset < file_->slotSize(); offset += zeroes.size()) {
        file_->write(base + offset, zeroes.data(), std::min<std::int64_t>(zeroes.size(), file_->slotSize() - offset));
    }
    slots_.emplace(piece, Slot{index, false, 0, {}});
    return true;
}

AdmissionResult PieceStore::admit(const std::vector<int>& required, const std::vector<int>& prefetch)
{
    validatePieces(required);
    validatePieces(prefetch);
    std::lock_guard<std::mutex> lock(mutex_);
    AdmissionResult result;
    for (int piece : required) {
        if (slots_.count(piece)) continue;
        if (admitPieceLocked(piece)) result.admitted.push_back(piece);
        else result.deferred_required.push_back(piece);
    }
    for (int piece : prefetch) {
        if (slots_.count(piece)) continue;
        if (slotCount() - slots_.size() > reserved_slots_ && admitPieceLocked(piece)) result.admitted.push_back(piece);
        else result.deferred_prefetch.push_back(piece);
    }
    return result;
}

bool PieceStore::reclaimableLocked(int piece, const Slot& slot, const std::set<int>& keep) const
{ return !keep.count(piece) && !protected_pieces_.count(piece) && slot.pins == 0; }

std::vector<int> PieceStore::reclaimablePieces(const std::vector<int>& keep) const
{
    validatePieces(keep);
    const std::set<int> wanted(keep.begin(), keep.end());
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> pieces;
    for (const auto& entry : slots_) if (reclaimableLocked(entry.first, entry.second, wanted)) pieces.push_back(entry.first);
    return pieces;
}

std::vector<int> PieceStore::reclaimOutside(const std::vector<int>& keep)
{
    validatePieces(keep);
    const std::set<int> wanted(keep.begin(), keep.end());
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> removed;
    for (auto entry = slots_.begin(); entry != slots_.end();) {
        if (reclaimableLocked(entry->first, entry->second, wanted)) {
            removed.push_back(entry->first);
            entry = slots_.erase(entry);
        } else ++entry;
    }
    return removed;
}

std::int64_t PieceStore::allocatedBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return file_->allocatedBytes();
}

bool PieceStore::needsEviction(const std::vector<int>& desired) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::set<int> wanted(desired.begin(), desired.end());
    std::size_t missing = 0;
    for (int piece : wanted) if (slots_.count(piece) == 0) ++missing;
    if (missing <= slotCount() - slots_.size()) return false;
    for (const auto& entry : slots_) {
        if (reclaimableLocked(entry.first, entry.second, wanted)) return true;
    }
    return false;
}

std::vector<int> PieceStore::reconcile(const std::vector<int>& desired)
{
    validatePieces(desired);
    std::lock_guard<std::mutex> lock(mutex_);
    const std::set<int> wanted(desired.begin(), desired.end());
    std::vector<int> evicted;
    for (int piece : desired) {
        if (slots_.count(piece)) continue;
        if (slots_.size() == slotCount()) {
            auto victim = std::find_if(slots_.begin(), slots_.end(), [&](const auto& entry) {
                return reclaimableLocked(entry.first, entry.second, wanted);
            });
            if (victim == slots_.end()) continue;
            evicted.push_back(victim->first);
            slots_.erase(victim);
        }
        admitPieceLocked(piece);
    }
    return evicted;
}

std::vector<int> PieceStore::residentPieces() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> pieces;
    for (const auto& entry : slots_) pieces.push_back(entry.first);
    return pieces;
}

std::vector<int> PieceStore::verifiedPieces() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> pieces;
    for (const auto& entry : slots_) if (entry.second.verified) pieces.push_back(entry.first);
    return pieces;
}

bool PieceStore::contains(int piece) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.count(piece) != 0;
}

bool PieceStore::isVerified(int piece) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(piece);
    return found != slots_.end() && found->second.verified;
}

void PieceStore::markVerified(int piece)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = slots_.find(piece); found != slots_.end()) found->second.verified = true;
}

void PieceStore::invalidate(int piece)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = slots_.find(piece); found != slots_.end()) {
        found->second.verified = false;
        found->second.written.clear();
    }
}

void PieceStore::recordWrite(Slot& slot, int begin, int end)
{
    slot.written.emplace_back(begin, end);
    std::sort(slot.written.begin(), slot.written.end());
    std::vector<std::pair<int, int>> merged;
    for (const auto& range : slot.written) {
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else merged.push_back(range);
    }
    slot.written = std::move(merged);
}

void PieceStore::write(int piece, int offset, const char* bytes, std::size_t length)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = slots_.find(piece);
    if (found == slots_.end()) throw std::runtime_error("Unreserved piece write rejected.");
    auto& slot = found->second;
    if (slot.verified || slot.pins != 0) throw std::runtime_error("Verified or pinned cache data cannot be overwritten.");
    if (offset < 0 || offset > geometry_.pieceSize(piece)
        || length > static_cast<std::size_t>(geometry_.pieceSize(piece) - offset)) {
        throw std::out_of_range("Piece write bounds");
    }
    file_->write(static_cast<std::int64_t>(slot.index) * file_->slotSize() + offset, bytes, length);
    recordWrite(slot, offset, offset + static_cast<int>(length));
}

std::vector<char> PieceStore::readRaw(int piece, int offset, int length) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(piece);
    if (found == slots_.end()) throw std::runtime_error("Piece is not resident in the disk cache.");
    if (offset < 0 || length < 0 || offset > geometry_.pieceSize(piece)
        || length > geometry_.pieceSize(piece) - offset) throw std::out_of_range("Piece read bounds");
    std::vector<char> bytes(length);
    file_->read(static_cast<std::int64_t>(found->second.index) * file_->slotSize() + offset, bytes.data(), bytes.size());
    return bytes;
}

std::vector<bool> PieceStore::writtenBlocks(int piece) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(piece);
    if (found == slots_.end()) return {};
    const int size = geometry_.pieceSize(piece);
    std::vector<bool> blocks((size + 16383) / 16384, false);
    for (std::size_t block = 0; block < blocks.size(); ++block) {
        const int begin = static_cast<int>(block) * 16384;
        const int end = std::min(size, begin + 16384);
        for (const auto& range : found->second.written) {
            if (range.first <= begin && range.second >= end) blocks[block] = true;
        }
    }
    return blocks;
}

bool PieceStore::rangeCachedLocked(const std::vector<int>& pieces) const
{
    return std::all_of(pieces.begin(), pieces.end(), [&](int piece) {
        const auto found = slots_.find(piece);
        return found != slots_.end() && found->second.verified;
    });
}

bool PieceStore::isRangeCached(std::int64_t offset, std::int64_t length) const
{
    const auto pieces = geometry_.piecesForRange(offset, length);
    std::lock_guard<std::mutex> lock(mutex_);
    return rangeCachedLocked(pieces);
}

std::optional<PieceStore::ReadLease> PieceStore::acquire(std::int64_t offset, std::int64_t length)
{
    auto pieces = geometry_.piecesForRange(offset, length);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!rangeCachedLocked(pieces)) return std::nullopt;
    auto owner = shared_from_this();
    for (int piece : pieces) ++slots_.at(piece).pins;
    return ReadLease{std::move(owner), offset, length, std::move(pieces)};
}

PieceStore::ReadLease::ReadLease(std::shared_ptr<PieceStore> store, std::int64_t offset,
                                std::int64_t length, std::vector<int> pieces)
    : store_(std::move(store)), offset_(offset), length_(length), pieces_(std::move(pieces)) {}

PieceStore::ReadLease::ReadLease(ReadLease&& other) noexcept
    : store_(std::move(other.store_)), offset_(other.offset_), length_(other.length_), pieces_(std::move(other.pieces_)) {}

PieceStore::ReadLease& PieceStore::ReadLease::operator=(ReadLease&& other) noexcept
{
    if (this != &other) {
        release();
        store_ = std::move(other.store_);
        offset_ = other.offset_;
        length_ = other.length_;
        pieces_ = std::move(other.pieces_);
    }
    return *this;
}

void PieceStore::ReadLease::release()
{
    if (!store_) return;
    {
        std::lock_guard<std::mutex> lock(store_->mutex_);
        for (int piece : pieces_) --store_->slots_.at(piece).pins;
    }
    store_.reset();
}

PieceStore::ReadLease::~ReadLease() { release(); }

std::vector<char> PieceStore::ReadLease::read() const
{
    if (!store_) throw std::logic_error("Read from released cache lease");
    std::lock_guard<std::mutex> lock(store_->mutex_);
    std::vector<char> bytes(static_cast<std::size_t>(length_));
    std::int64_t global = store_->geometry_.video_offset + offset_;
    std::size_t copied = 0;
    for (int piece : pieces_) {
        const int within = static_cast<int>(global % store_->geometry_.piece_length);
        const auto length = std::min<std::int64_t>(length_ - copied, store_->geometry_.pieceSize(piece) - within);
        const auto& slot = store_->slots_.at(piece);
        store_->file_->read(static_cast<std::int64_t>(slot.index) * store_->file_->slotSize() + within,
                            bytes.data() + copied, length);
        copied += length;
        global += length;
    }
    return bytes;
}

} // namespace intezam
