#include "piece_store.hpp"

#include <algorithm>
#include <future>
#include <iostream>
#include <numeric>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

class StoreFixture {
public:
    explicit StoreFixture(std::size_t slots, std::optional<std::size_t> reservation = {})
    {
        auto pattern = (fs::current_path() / "intezam-reserve-test-XXXXXX").string();
        require(mkdtemp(pattern.data()) != nullptr, "reserve fixture directory");
        root_ = pattern;
        const auto size = static_cast<std::int64_t>((slots + 1) * 16384 * 40 - 40);
        try { store = intezam::PieceStore::create({size, 0, size, 16384}, root_, reservation); }
        catch (...) { fs::remove_all(root_); throw; }
        require(store->slotCount() == slots, "fixture whole-piece slot count");
    }
    ~StoreFixture() { store.reset(); std::error_code error; fs::remove_all(root_, error); }
    std::shared_ptr<intezam::PieceStore> store;
private:
    fs::path root_;
};

void checksSmallCapacities()
{
    for (std::size_t slots : {1, 2, 3, 4, 8, 16}) {
        StoreFixture fixture(slots);
        auto& store = *fixture.store;
        const auto reserve = slots < 4 ? 0 : std::min(slots - 2, (slots + 7) / 8);
        require(store.inventory().reserved_target == reserve, "adaptive reserve leaves two working slots");
        std::vector<int> speculative(slots);
        std::iota(speculative.begin(), speculative.end(), 0);
        const auto admitted = store.admit({}, speculative);
        const auto inventory = store.inventory();
        require(inventory.occupied == slots - reserve && inventory.free == reserve,
                "prefetch cannot consume reserved capacity");
        require(admitted.deferred_prefetch.size() == reserve, "excess prefetch is queued");
        require(inventory.reserved_free == reserve && inventory.prefetch_free == 0, "coherent reservation accounting");
        const auto urgent = store.admit({static_cast<int>(slots + 5)}, {});
        require(urgent.deferred_required.empty() == (reserve > 0), "urgent reads consume reserve when available");
        require(store.allocatedBytes() <= store.budgetBytes(), "reservations do not grow physical cache");
    }
    StoreFixture no_reserve(8, 0);
    require(no_reserve.store->inventory().reserved_target == 0, "explicit zero reserve");
    no_reserve.store->admit({}, {0, 1, 2, 3, 4, 5, 6, 7});
    no_reserve.store->setReservedSlots(2);
    require(no_reserve.store->inventory().occupied == 8 && no_reserve.store->inventory().reserved_free == 0,
            "raising reservation does not evict live pieces");
    no_reserve.store->setReservedSlots(0);
    require(no_reserve.store->inventory().reserved_target == 0, "reserve can be released under buffer pressure");
    bool rejected = false;
    try { StoreFixture invalid(4, 3); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid reserve cannot starve the working set");
}

void protectsOverlapPinsAndVerification()
{
    StoreFixture fixture(8, 2);
    auto& store = *fixture.store;
    store.admit({0, 1}, {2, 3, 4, 5, 6, 7});
    require(store.inventory().occupied == 6, "two slots reserved");
    const std::vector<char> bytes(16384, 'v');
    for (int piece : {0, 1}) {
        store.write(piece, 0, bytes.data(), bytes.size());
        require(!store.acquire(piece * 16384, 1), "unverified bytes cannot be exposed");
        store.markVerified(piece);
    }
    auto lease = store.acquire(16384, 16384);
    require(lease.has_value(), "verified piece is pinned");
    store.setProtectedPieces({0});
    const auto inventory = store.inventory();
    require(inventory.protected_slots == 2 && inventory.pinned_slots == 1, "pins and explicit protection are counted once");
    const auto victims = store.reclaimablePieces({2});
    require(victims == std::vector<int>({3, 4, 5}), "only expired unprotected pieces are eligible");
    auto reading = std::async(std::launch::async, [&] {
        for (int iteration = 0; iteration < 50; ++iteration) require(lease->read() == bytes, "pinned bytes survive reclamation");
    });
    const auto evicted = store.reclaimOutside({2}); // no live torrent; safe quiescent boundary
    reading.get();
    require(evicted == victims && store.contains(0) && store.contains(1) && store.contains(2),
            "batch preserves overlap and protected readers");
    store.admit({20, 21}, {22, 23, 24, 25});
    require(lease->read() == bytes, "urgent reserve use cannot overwrite a reader");
    lease.reset();
    store.setProtectedPieces({});
    store.reclaimOutside({2, 20, 21});
    require(!store.contains(0) && !store.isVerified(1), "released expired pieces lose availability");
    require(store.inventory().free + store.inventory().occupied == store.slotCount(), "accounting after batch reuse");
    store.invalidate(20);
    require(!store.isVerified(20), "failed verification remains unreadable");
    const auto occupied = store.inventory().occupied;
    bool rejected = false;
    try { store.admit({30, 1000000}, {}); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected && store.inventory().occupied == occupied, "invalid admission fails before mutating slots");
    require(store.allocatedBytes() <= store.budgetBytes(), "disk limit after repeated reuse");
}
} // namespace

int main()
{
    try {
        checksSmallCapacities();
        protectsOverlapPinsAndVerification();
        std::cout << "Reserved admission, small capacities, batch protection and verification passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
