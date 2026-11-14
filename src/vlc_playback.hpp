#pragma once

#include "torrent_cache_session.hpp"
#include "vlc_player.hpp"
#include "cli_status_renderer.hpp"

namespace intezam {
class PlaybackPreparation;

struct PlaybackOptions {
    CacheStreamOptions cache;
    VlcOptions vlc;
    CliStatusOptions monitoring{};
    std::shared_ptr<PlaybackPreparation> preparation;
};

void runVlcPlayback(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata,
    const VideoFile& video, const PlaybackOptions& options = {});

} // namespace intezam
