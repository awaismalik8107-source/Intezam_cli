#pragma once

#include "torrent_cache_session.hpp"

#include <chrono>

namespace intezam {

struct CacheSimulationOptions {
    CacheStreamOptions cache;
    std::chrono::seconds duration{0}; // zero: interactive until q/EOF/signal
};

void runCacheSimulation(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video, const CacheSimulationOptions& options);

} // namespace intezam
