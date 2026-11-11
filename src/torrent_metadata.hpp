#pragma once

#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

#include <chrono>
#include <memory>
#include <string>

#if LIBTORRENT_VERSION_NUM < 20100
#error "Intezam requires libtorrent-rasterbar 2.1.0 or newer for its cache disk interface."
#endif

namespace intezam {

libtorrent::settings_pack metadataSessionSettings();

struct MetadataOptions {
    std::chrono::milliseconds timeout{120000};
    libtorrent::settings_pack settings = metadataSessionSettings();
};

// Returns owned, in-memory metadata. The session is closed before returning.
// Throws invalid_argument for invalid input and runtime_error for retrieval failures.
std::shared_ptr<const libtorrent::torrent_info> retrieveTorrentMetadata(
    const std::string& magnet, const MetadataOptions& options = {});

} // namespace intezam
