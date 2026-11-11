#include "video_files.hpp"
#include "video_selection.hpp"

#include <libtorrent/file_storage.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lt = libtorrent;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void detectsOnlySupportedFinalExtensions()
{
    const std::vector<std::string> extensions{
        "mp4", "mkv", "avi", "mov", "webm", "flv",
        "wmv", "m4v", "mpeg", "mpg", "ts", "m2ts"
    };
    for (const auto& extension : extensions) {
        require(intezam::isVideoFile("Collection/video." + extension), extension);
    }
    require(intezam::isVideoFile("Collection/Episode.MkV"), "mixed-case extension");
    require(intezam::isVideoFile("Collection/clip.M2TS"), "uppercase extension");
    for (const auto& path : {"clip.mkv.zip", "clip.mp4.srt", "cover.jpg", "readme.txt",
                             "archive.rar", "movie", "folder.mkv/readme", "movie."}) {
        require(!intezam::isVideoFile(path), std::string("non-video: ") + path);
    }
}

std::vector<intezam::VideoFile> detectsVideosWithOriginalIndexes()
{
    lt::file_storage files;
    files.add_file("Collection/cover.jpg", 10);
    files.add_file("Collection/Season 1/Episode.MkV", 1024 * 1024);
    files.add_file("Collection/subtitles.srt", 20);
    files.add_file("Collection/clip.MP4", 2LL * 1024 * 1024 * 1024);
    files.add_file("Collection/padding.mp4", 16, lt::file_storage::flag_pad_file);
    files.add_file("Collection/link.mov", 0, lt::file_storage::flag_symlink,
                   0, "clip.MP4");

    auto videos = intezam::detectVideoFiles(files);
    require(videos.size() == 2, "ignore non-videos, padding and symlinks");
    require(videos[0].torrent_index == lt::file_index_t{1}, "first original index");
    require(videos[1].torrent_index == lt::file_index_t{3}, "second original index");
    require(videos[0].path == "Collection/Season 1/Episode.MkV", "preserve full path");
    require(videos[0].extension == ".mkv", "normalize extension");
    require(videos[1].size_bytes == 2147483648LL, "64-bit size");
    require(videos[0].torrent_offset == 10, "offset includes earlier non-video");
    require(videos[1].torrent_offset == 1048606, "offset includes all earlier files");
    require(intezam::detectVideoFiles(lt::file_storage{}).empty(), "empty file list");
    return videos;
}

void validatesSelectionAndKeepsOriginalIdentity(const std::vector<intezam::VideoFile>& videos)
{
    std::istringstream input("\nabc\n0\n-1\n3\n1.5\n1xyz\n9999999999999999999999999\n 2 \r\n");
    std::ostringstream output;
    const auto selected = intezam::selectVideo(videos, input, output);
    require(selected.has_value(), "valid selection after invalid input");
    require(selected->torrent_index == lt::file_index_t{3}, "display number != torrent index");
    require(selected->path == videos[1].path, "selected path");
    require(selected->size_bytes == videos[1].size_bytes, "selected size");
    require(output.str().find("Invalid selection") != std::string::npos, "feedback for errors");

    std::istringstream cancelled("q\n");
    require(!intezam::selectVideo(videos, cancelled, output), "explicit cancellation");
    std::istringstream eof;
    require(!intezam::selectVideo(videos, eof, output), "EOF cancellation");
    std::istringstream unused("1\n");
    require(!intezam::selectVideo({}, unused, output), "empty list");
    require(unused.peek() == '1', "empty list does not prompt");
}

void displaysPathsFormatsSizesAndIndexes(const std::vector<intezam::VideoFile>& videos)
{
    std::ostringstream output;
    intezam::displayVideos(videos, output);
    const auto text = output.str();
    require(text.find("[1] Collection/Season 1/Episode.MkV") != std::string::npos, "full path");
    require(text.find("Format: MKV") != std::string::npos, "format");
    require(text.find("1.00 MiB") != std::string::npos, "MiB size");
    require(text.find("2.00 GiB") != std::string::npos, "GiB size");
    require(text.find("Torrent file index: 3") != std::string::npos, "original index");
    require(intezam::formatVideoSize(0) == "0.00 MiB", "zero-byte size");
}

} // namespace

int main()
{
    try {
        detectsOnlySupportedFinalExtensions();
        const auto videos = detectsVideosWithOriginalIndexes();
        validatesSelectionAndKeepsOriginalIdentity(videos);
        displaysPathsFormatsSizesAndIndexes(videos);
        std::cout << "Video detection and selection tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
