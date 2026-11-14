#pragma once

#include "cache_geometry.hpp"
#include "video_byte_source.hpp"

#include <string>

namespace intezam {

enum class ContainerType { unknown, mp4, matroska, webm, avi, mpeg_ts, mpeg_ps };

struct ContainerInfo {
    ContainerType type = ContainerType::unknown;
    std::string name = "Unknown (VLC probing)";
    std::vector<VideoRange> metadata_ranges;
    bool moov_at_end = false;
    bool isMetadata(std::int64_t offset) const;
};

// Signature/header probing only. VLC remains responsible for demuxing/decoding.
// ISO BMFF payload boxes are skipped with byte offsets, not read through.
ContainerInfo probeContainer(VideoByteSource& source);

} // namespace intezam
