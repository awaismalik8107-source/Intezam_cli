#pragma once

#include "piece_store.hpp"

#include <libtorrent/session_params.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace intezam {

struct CacheDiskState {
    std::atomic<int> active_storages{0};
    std::atomic<std::uint64_t> jobs{0};
    std::atomic<std::uint64_t> work_microseconds{0};
    void fail(const std::string& message);
    std::string error() const;
private:
    mutable std::mutex mutex_;
    std::string error_;
};

libtorrent::disk_io_constructor_type cacheDiskConstructor(
    std::shared_ptr<PieceStore> store, PieceGeometry geometry, std::shared_ptr<CacheDiskState> state);

} // namespace intezam
