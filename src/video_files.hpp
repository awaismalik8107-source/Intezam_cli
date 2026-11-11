#pragma once

#include <libtorrent/file_storage.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/units.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace intezam {

struct VideoFile {
    libtorrent::file_index_t torrent_index;
    std::string path;
    std::string extension;
    std::int64_t size_bytes;
    std::int64_t torrent_offset;
};

bool isVideoFile(std::string_view path);
std::vector<VideoFile> detectVideoFiles(const libtorrent::file_storage& files);
std::vector<VideoFile> detectVideoFiles(const libtorrent::torrent_info& metadata);

} // namespace intezam
