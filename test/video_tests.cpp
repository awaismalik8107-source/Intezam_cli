#include "video_files.hpp"
#include "video_selection.hpp"

#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/torrent_info.hpp>

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

void requiresManualSelectionEvenWhenOnlyOneVideoFits()
{
    const intezam::VideoFile sample{lt::file_index_t{3}, "Sample/sample.mkv", ".mkv", 60879760, 0};
    const intezam::VideoFile movie{lt::file_index_t{6}, "main-film.mkv", ".mkv", 5869802096, 60879760};
    std::istringstream sample_choice("1\n");
    std::ostringstream adaptive_output;
    const auto adaptive = intezam::selectStreamingVideo({sample, movie}, 2097152, sample_choice, adaptive_output);
    require(adaptive && adaptive->torrent_index == sample.torrent_index, "adaptive budget restores manual sample choice");
    require(adaptive_output.str().find("Automatically") == std::string::npos, "adaptive mode does not force main video");
    std::istringstream main_choice("2\n");
    std::ostringstream output;
    const auto selected = intezam::selectStreamingVideo({sample, movie}, 2097152, main_choice, output, {intezam::CacheBudgetMode::strict, 25});
    require(selected && selected->torrent_index == movie.torrent_index && selected->torrent_offset == movie.torrent_offset,
            "manual main-video selection preserves its torrent identity");
    require(output.str().find("Automatically") == std::string::npos, "prefetch never substitutes for manual selection");
    require(output.str().find("[1] unavailable") != std::string::npos, "explain sample cache limitation");

    std::istringstream reversed_input("1\n");
    std::ostringstream reversed_output;
    const auto first = intezam::selectStreamingVideo({movie, sample}, 2097152, reversed_input, reversed_output, {intezam::CacheBudgetMode::strict, 25});
    require(first && first->torrent_index == movie.torrent_index, "do not hard-code option two for other torrents");
    std::istringstream no_choice;
    require(!intezam::selectStreamingVideo({sample, movie}, 2097152, no_choice, output, {intezam::CacheBudgetMode::strict, 25}),
        "EOF cancels even when one eligible candidate was prefetched");

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
    std::istringstream interrupted("3\n");
    require(!intezam::selectStreamingVideo(videos, 2097152, interrupted, output, {}, [] { return true; }),
        "selection cancellation does not accept or auto-play a pending choice");
    bool rejected = false;
    std::istringstream unused;
    try { intezam::selectStreamingVideo({videos.front()}, 2097152, unused, output); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "report when no video budget can fit a piece");
}

void explicitTargetSelectionAndManifestJson()
{
    lt::entry torrent;
    auto& info = torrent["info"];
    info["name"] = "ShowPack";
    const std::int64_t piece_length = 2LL * 1024 * 1024;
    info["piece length"] = piece_length;

    struct Spec {
        const char* dir;
        const char* name;
        std::int64_t size;
    };
    const std::vector<Spec> specs{
        {"", "cover.jpg", 4096},
        {"Season 1", "Episode 01.mkv", 256LL * 1024 * 1024},
        {"Season 1", "Episode 07.mkv", 450LL * 1024 * 1024},
        {"Season 2", "Episode 07.mkv", 480LL * 1024 * 1024},
    };
    std::int64_t total_size = 0;
    for (const auto& s : specs) {
        lt::entry f;
        f["length"] = s.size;
        if (s.dir[0] != '\0') f["path"].list().emplace_back(s.dir);
        f["path"].list().emplace_back(s.name);
        info["files"].list().push_back(std::move(f));
        total_size += s.size;
    }
    const std::int64_t num_pieces = (total_size + piece_length - 1) / piece_length;
    info["pieces"] = std::string(static_cast<std::size_t>(num_pieces) * 20, 'x');

    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent);
    lt::torrent_info metadata(encoded.data(), static_cast<int>(encoded.size()));
    const auto videos = intezam::detectVideoFiles(metadata);
    const std::string actual_hash = intezam::infoHashHex(metadata);
    require(actual_hash.size() == 40, "expected 40-char info hash hex");
    require(videos.size() == 3, "expected 3 detected video files");

    // 1. Exact selection of Season 1/Episode 07 at raw index 2
    intezam::ExplicitVideoTarget ep7_target;
    ep7_target.torrent_file_index = 2;
    ep7_target.expected_info_hash = actual_hash;
    ep7_target.expected_file_path = "Season 1/Episode 07.mkv";
    ep7_target.expected_file_size = 450LL * 1024 * 1024;
    const auto chosen = intezam::resolveExplicitVideoTarget(metadata, videos, ep7_target, true);
    require(static_cast<int>(chosen.torrent_index) == 2, "must pick Season 1/Episode 07 index 2, never index 1");
    require(chosen.size_bytes == 450LL * 1024 * 1024, "exact file size preserved");

    // 2. Reject wrong file path instead of falling back to first video
    bool path_rejected = false;
    try {
        auto bad = ep7_target;
        bad.expected_file_path = "Season 2/Episode 07.mkv";
        intezam::resolveExplicitVideoTarget(metadata, videos, bad);
    } catch (const std::runtime_error&) {
        path_rejected = true;
    }
    require(path_rejected, "wrong file path must be rejected");

    // 3. Reject wrong info-hash
    bool hash_rejected = false;
    try {
        auto bad = ep7_target;
        bad.expected_info_hash = std::string(40, 'f');
        intezam::resolveExplicitVideoTarget(metadata, videos, bad);
    } catch (const std::runtime_error&) {
        hash_rejected = true;
    }
    require(hash_rejected, "wrong info-hash must be rejected");

    // 4. Reject non-video file index (0 = cover.jpg)
    bool non_video_rejected = false;
    try {
        intezam::ExplicitVideoTarget cover_target;
        cover_target.torrent_file_index = 0;
        intezam::resolveExplicitVideoTarget(metadata, videos, cover_target);
    } catch (const std::runtime_error&) {
        non_video_rejected = true;
    }
    require(non_video_rejected, "non-video index must be rejected");

    // 5. Reject --select-single on multi-video torrent
    bool multi_rejected = false;
    try {
        intezam::ExplicitVideoTarget single_target;
        single_target.select_single = true;
        intezam::resolveExplicitVideoTarget(metadata, videos, single_target);
    } catch (const std::runtime_error&) {
        multi_rejected = true;
    }
    require(multi_rejected, "select-single must refuse multi-video batch without file index");

    // 6. Verify manifest JSON serialization preserves exact 0-based index & stripped relative path
    const std::string json = intezam::formatManifestJson(metadata);
    require(json.find("\"ok\":true") != std::string::npos, "manifest JSON ok flag");
    require(json.find("\"torrent_file_index\":2,\"relative_path\":\"Season 1/Episode 07.mkv\"") != std::string::npos,
        "manifest JSON preserves 0-based index and relative path");
}

} // namespace

int main()
{
    try {
        detectsOnlySupportedFinalExtensions();
        const auto videos = detectsVideosWithOriginalIndexes();
        validatesSelectionAndKeepsOriginalIdentity(videos);
        displaysPathsFormatsSizesAndIndexes(videos);
        requiresManualSelectionEvenWhenOnlyOneVideoFits();
        keepsManualChoiceWhenMultipleVideosHaveEnoughBudget();
        explicitTargetSelectionAndManifestJson();
        std::cout << "Video detection and selection tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
