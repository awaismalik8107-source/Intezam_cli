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

void selectsTheOnlyVideoWithEnoughPieceBudget()
{
    const intezam::VideoFile sample{lt::file_index_t{3}, "Sample/sample.mkv", ".mkv", 60879760, 0};
    const intezam::VideoFile movie{lt::file_index_t{6}, "main-film.mkv", ".mkv", 5869802096, 60879760};
    std::istringstream sample_choice("1\n");
    std::ostringstream adaptive_output;
    const auto adaptive = intezam::selectStreamingVideo({sample, movie}, 2097152, sample_choice, adaptive_output);
    require(adaptive && adaptive->torrent_index == sample.torrent_index, "adaptive budget restores manual sample choice");
    require(adaptive_output.str().find("Automatically") == std::string::npos, "adaptive mode does not force main video");
    std::istringstream no_choice;
    std::ostringstream output;
    const auto selected = intezam::selectStreamingVideo({sample, movie}, 2097152, no_choice, output, {intezam::CacheBudgetMode::strict, 25});
    require(selected && selected->torrent_index == movie.torrent_index && selected->torrent_offset == movie.torrent_offset,
            "auto-select second/main video without changing its torrent identity");
    require(output.str().find("Automatically selecting [2]") != std::string::npos, "explain second-option selection");
    require(output.str().find("[1] unavailable") != std::string::npos, "explain sample cache limitation");

    std::istringstream reversed_input;
    std::ostringstream reversed_output;
    const auto first = intezam::selectStreamingVideo({movie, sample}, 2097152, reversed_input, reversed_output, {intezam::CacheBudgetMode::strict, 25});
    require(first && first->torrent_index == movie.torrent_index, "do not hard-code option two for other torrents");
    require(reversed_output.str().find("Automatically selecting [1]") != std::string::npos, "preserve display numbering");

    std::istringstream metadata_choice("1\n");
    const auto metadata_only = intezam::selectVideo({sample, movie}, metadata_choice, output);
    require(metadata_only && metadata_only->torrent_index == sample.torrent_index, "metadata-only selection still permits small videos");
}

void keepsManualChoiceWhenMultipleVideosHaveEnoughBudget()
{
    const std::vector<intezam::VideoFile> videos{
        {lt::file_index_t{1}, "sample.mkv", ".mkv", 1024 * 1024, 0},
        {lt::file_index_t{4}, "episode-one.mkv", ".mkv", 512LL * 1024 * 1024, 1024 * 1024},
        {lt::file_index_t{7}, "episode-two.mkv", ".mkv", 512LL * 1024 * 1024, 513LL * 1024 * 1024}};
    std::istringstream input("1\n3\n");
    std::ostringstream output;
    const auto selected = intezam::selectStreamingVideo(videos, 2097152, input, output);
    require(selected && selected->torrent_index == lt::file_index_t{7}, "reject small sample then accept original option three");
    std::istringstream cancel("q\n");
    require(!intezam::selectStreamingVideo(videos, 2097152, cancel, output), "streaming selection can be cancelled");
    bool rejected = false;
    std::istringstream unused;
    try { intezam::selectStreamingVideo({videos.front()}, 2097152, unused, output); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "report when no video budget can fit a piece");
}

} // namespace

int main()
{
    try {
        detectsOnlySupportedFinalExtensions();
        const auto videos = detectsVideosWithOriginalIndexes();
        validatesSelectionAndKeepsOriginalIdentity(videos);
        displaysPathsFormatsSizesAndIndexes(videos);
        selectsTheOnlyVideoWithEnoughPieceBudget();
        keepsManualChoiceWhenMultipleVideosHaveEnoughBudget();
        std::cout << "Video detection and selection tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
