#pragma once

#include "video_files.hpp"
#include "cache_budget.hpp"

#include <iosfwd>
#include <optional>
#include <functional>
#include <string>

namespace libtorrent {
class torrent_info;
}

namespace intezam {

struct ExplicitVideoTarget {
    std::optional<int> torrent_file_index;
    std::string expected_info_hash;
    std::string expected_file_path;
    std::optional<std::int64_t> expected_file_size;
    bool select_single = false;
};

std::string infoHashHex(const libtorrent::torrent_info& metadata);
bool normalizedPathMatchesTarget(
    const std::string& torrent_name,
    const std::string& file_path,
    const std::string& expected_path);
VideoFile resolveExplicitVideoTarget(
    const libtorrent::torrent_info& metadata,
    const std::vector<VideoFile>& videos,
    const ExplicitVideoTarget& target,
    bool check_budget = false,
    CacheBudgetOptions budget = {});
std::string formatManifestJson(const libtorrent::torrent_info& metadata);

std::string formatVideoSize(std::int64_t size_bytes);
void displayVideos(const std::vector<VideoFile>& videos, std::ostream& output);
// Display numbers are 1-based; torrent_index remains the original 0-based index.
// EOF or q cancels without changing torrent state.
std::optional<VideoFile> selectVideo(
    const std::vector<VideoFile>& videos, std::istream& input, std::ostream& output);
std::optional<VideoFile> selectVideo(const std::vector<VideoFile>& videos,
    std::istream& input, std::ostream& output, const std::function<bool()>& cancelled);
// Adaptive mode retains manual choice; allocation is checked again on the real filesystem.
std::optional<VideoFile> selectStreamingVideo(const std::vector<VideoFile>& videos,
    int piece_length, std::istream& input, std::ostream& output, CacheBudgetOptions budget = {});
std::optional<VideoFile> selectStreamingVideo(const std::vector<VideoFile>& videos,
    int piece_length, std::istream& input, std::ostream& output, CacheBudgetOptions budget,
    const std::function<bool()>& cancelled);

} // namespace intezam
