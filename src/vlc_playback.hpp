#pragma once

#include "torrent_cache_session.hpp"
#include "vlc_player.hpp"

namespace intezam {

struct PlaybackOptions {
    CacheStreamOptions cache;
    VlcOptions vlc;
};

void runVlcPlayback(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video, const PlaybackOptions& options = {});

} // namespace intezam
