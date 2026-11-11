#include "video_files.hpp"

#include <libtorrent/version.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

namespace intezam {
namespace {

std::string videoExtension(std::string_view path)
{
    auto extension = std::filesystem::path{path}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}

} // namespace

bool isVideoFile(std::string_view path)
{
    static constexpr std::array<std::string_view, 12> extensions{
        ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv",
        ".wmv", ".m4v", ".mpeg", ".mpg", ".ts", ".m2ts"
    };
    const auto extension = videoExtension(path);
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::vector<VideoFile> detectVideoFiles(const libtorrent::file_storage& files)
{
    std::vector<VideoFile> videos;
    for (int index = 0; index < files.num_files(); ++index) {
        const auto file_index = libtorrent::file_index_t{index};
        const auto flags = files.file_flags(file_index);
        if (flags & (libtorrent::file_storage::flag_pad_file | libtorrent::file_storage::flag_symlink)) {
            continue;
        }
        const auto path = files.file_path(file_index);
        if (isVideoFile(path)) {
            videos.push_back({file_index, path, videoExtension(path),
                              files.file_size(file_index), files.file_offset(file_index)});
        }
    }
    return videos;
}

std::vector<VideoFile> detectVideoFiles(const libtorrent::torrent_info& metadata)
{
#if LIBTORRENT_VERSION_NUM >= 20100
    return detectVideoFiles(metadata.layout());
#else
    return detectVideoFiles(metadata.files());
#endif
}

} // namespace intezam
