#pragma once

#include "video_files.hpp"

#include <iosfwd>
#include <optional>

namespace intezam {

std::string formatVideoSize(std::int64_t size_bytes);
void displayVideos(const std::vector<VideoFile>& videos, std::ostream& output);
// Display numbers are 1-based; torrent_index remains the original 0-based index.
// EOF or q cancels without changing torrent state.
std::optional<VideoFile> selectVideo(
    const std::vector<VideoFile>& videos, std::istream& input, std::ostream& output);
// Reject budgets below one piece. If this leaves one of several candidates,
// select it automatically; retain original display numbers for manual choices.
// Passing this check does not guarantee sufficient filesystem/container capacity.
std::optional<VideoFile> selectStreamingVideo(const std::vector<VideoFile>& videos,
    int piece_length, std::istream& input, std::ostream& output);

} // namespace intezam
