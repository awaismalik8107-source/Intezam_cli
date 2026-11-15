#include "video_selection.hpp"

#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <istream>
#include <iostream>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace intezam {
namespace {
std::string normalizeForwardPath(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    bool prev_slash = false;
    for (char ch : raw) {
        const char mapped = (ch == '\\') ? '/' : ch;
        if (mapped == '/') {
            if (!out.empty() && !prev_slash) {
                out.push_back('/');
            }
            prev_slash = true;
        } else {
            out.push_back(mapped);
            prev_slash = false;
        }
    }
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

std::string stripTorrentRootPrefix(const std::string& torrent_name, const std::string& path)
{
    const std::string norm_path = normalizeForwardPath(path);
    const std::string norm_root = normalizeForwardPath(torrent_name);
    if (!norm_root.empty() && norm_path.size() > norm_root.size() + 1
        && norm_path.compare(0, norm_root.size(), norm_root) == 0
        && norm_path[norm_root.size()] == '/') {
        return norm_path.substr(norm_root.size() + 1);
    }
    return norm_path;
}

std::string lowercaseAscii(std::string_view input)
{
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

void appendJsonEscaped(std::ostringstream& out, std::string_view value)
{
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[(ch >> 4) & 0xF] << hex[ch & 0xF];
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    out << '"';
}
bool readSelectionLine(std::istream& input, std::string& line, const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) return false;
    if (!cancelled || &input != &std::cin) return static_cast<bool>(std::getline(input, line));
    line.clear();
    while (!cancelled()) {
        if (input.rdbuf()->in_avail() <= 0) {
            pollfd descriptor{STDIN_FILENO, POLLIN, 0};
            const int ready = poll(&descriptor, 1, 100);
            if (ready < 0 && errno != EINTR) throw std::runtime_error("Cannot poll video selection input.");
            if (ready <= 0) continue;
        }
        const int character = input.get();
        if (character == std::char_traits<char>::eof()) return !line.empty();
        if (character == '\n') return true;
        line.push_back(static_cast<char>(character));
    }
    return false;
}
}

std::string formatVideoSize(std::int64_t size_bytes)
{
    constexpr double mib = 1024.0 * 1024.0;
    constexpr double gib = 1024.0 * mib;
    const bool use_gib = size_bytes >= static_cast<std::int64_t>(gib);
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(2)
           << static_cast<double>(size_bytes) / (use_gib ? gib : mib)
           << (use_gib ? " GiB" : " MiB");
    return output.str();
}

void displayVideos(const std::vector<VideoFile>& videos, std::ostream& output)
{
    for (std::size_t index = 0; index < videos.size(); ++index) {
        const auto& video = videos[index];
        auto format = video.extension.substr(1);
        std::transform(format.begin(), format.end(), format.begin(),
            [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
        output << '[' << index + 1 << "] " << video.path
               << "\n    Format: " << format
               << "\n    Size: " << formatVideoSize(video.size_bytes)
               << "\n    Torrent file index: " << static_cast<int>(video.torrent_index) << "\n\n";
    }
}

std::optional<VideoFile> selectVideo(
    const std::vector<VideoFile>& videos, std::istream& input, std::ostream& output)
{ return selectVideo(videos, input, output, {}); }

std::optional<VideoFile> selectVideo(const std::vector<VideoFile>& videos,
    std::istream& input, std::ostream& output, const std::function<bool()>& cancelled)
{
    if (videos.empty()) {
        return std::nullopt;
    }
    for (;;) {
        output << "Select a video (1-" << videos.size()
               << ", q to cancel): " << std::flush;
        std::string line;
        if (!readSelectionLine(input, line, cancelled)) {
            return std::nullopt;
        }
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const auto choice = std::string_view{line}.substr(
                first, line.find_last_not_of(" \t\r\n") - first + 1);
            if (choice == "q" || choice == "Q") {
                return std::nullopt;
            }
            std::size_t number = 0;
            const auto result = std::from_chars(choice.data(), choice.data() + choice.size(), number);
            if (result.ec == std::errc{} && result.ptr == choice.data() + choice.size()
                && number >= 1 && number <= videos.size()) {
                return videos[number - 1];
            }
        }
        output << "Invalid selection. Enter a whole number from 1 to " << videos.size() << ".\n";
    }
}

std::optional<VideoFile> selectStreamingVideo(const std::vector<VideoFile>& videos,
    int piece_length, std::istream& input, std::ostream& output, CacheBudgetOptions budget)
{ return selectStreamingVideo(videos, piece_length, input, output, budget, {}); }

std::optional<VideoFile> selectStreamingVideo(const std::vector<VideoFile>& videos,
    int piece_length, std::istream& input, std::ostream& output, CacheBudgetOptions budget,
    const std::function<bool()>& cancelled)
{
    if (piece_length <= 0) throw std::invalid_argument("Torrent piece length must be positive.");
    if (cancelled && cancelled()) return std::nullopt;
    if (videos.empty()) return std::nullopt;
    const auto fits_piece = [piece_length, budget](const VideoFile& video) {
        if (budget.mode == CacheBudgetMode::strict) return video.size_bytes / 40 >= piece_length;
        try { planCacheBudget(video.size_bytes, piece_length, 4096, 8192, budget); return true; }
        catch (const std::runtime_error&) { return false; }
    };
    const auto eligible = std::count_if(videos.begin(), videos.end(), fits_piece);
    for (std::size_t index = 0; index < videos.size(); ++index) {
        if (!fits_piece(videos[index])) {
            output << "Video [" << index + 1 << "] unavailable under the selected cache budget policy.\n";
        }
    }
    if (eligible == 0) throw std::runtime_error("No video fits the selected cache budget policy.");
    for (;;) {
        auto selected = selectVideo(videos, input, output, cancelled);
        if (!selected || fits_piece(*selected)) return selected;
        output << "That video does not fit the selected cache budget policy. Choose another video.\n";
    }
}

std::string infoHashHex(const libtorrent::torrent_info& metadata)
{
    constexpr char hex[] = "0123456789abcdef";
    const auto hashes = metadata.info_hashes();
    if (hashes.has_v1()) {
        std::string out;
        out.reserve(40);
        for (const auto byte : hashes.v1) {
            const unsigned char v = static_cast<unsigned char>(byte);
            out.push_back(hex[(v >> 4) & 0xF]);
            out.push_back(hex[v & 0xF]);
        }
        return out;
    }
    if (hashes.has_v2()) {
        std::string out;
        out.reserve(40);
        for (std::size_t i = 0; i < 20 && i < static_cast<std::size_t>(hashes.v2.size()); ++i) {
            const unsigned char v = static_cast<unsigned char>(hashes.v2[i]);
            out.push_back(hex[(v >> 4) & 0xF]);
            out.push_back(hex[v & 0xF]);
        }
        return out;
    }
    return {};
}

bool normalizedPathMatchesTarget(
    const std::string& torrent_name,
    const std::string& file_path,
    const std::string& expected_path)
{
    const std::string norm_actual = normalizeForwardPath(file_path);
    const std::string norm_expected = normalizeForwardPath(expected_path);
    if (norm_expected.empty()) return true;
    if (norm_actual == norm_expected) return true;
    const std::string rel_actual = stripTorrentRootPrefix(torrent_name, norm_actual);
    const std::string rel_expected = stripTorrentRootPrefix(torrent_name, norm_expected);
    return rel_actual == norm_expected || rel_actual == rel_expected;
}

VideoFile resolveExplicitVideoTarget(
    const libtorrent::torrent_info& metadata,
    const std::vector<VideoFile>& videos,
    const ExplicitVideoTarget& target,
    bool check_budget,
    CacheBudgetOptions budget)
{
    if (!target.expected_info_hash.empty()) {
        const std::string actual_hash = infoHashHex(metadata);
        const std::string expected_hash = lowercaseAscii(target.expected_info_hash);
        if (actual_hash.empty() || actual_hash != expected_hash) {
            throw std::runtime_error(
                "Torrent info hash mismatch: expected " + expected_hash + ", got " + actual_hash + ".");
        }
    }

    if (videos.empty()) {
        throw std::runtime_error("No supported video files found in torrent.");
    }

    const VideoFile* chosen = nullptr;
    if (target.torrent_file_index.has_value()) {
        const int wanted = *target.torrent_file_index;
        if (wanted < 0) {
            throw std::invalid_argument("Torrent file index must be non-negative.");
        }
        for (const auto& video : videos) {
            if (static_cast<int>(video.torrent_index) == wanted) {
                chosen = &video;
                break;
            }
        }
        if (chosen == nullptr) {
            throw std::runtime_error(
                "Requested --file-index " + std::to_string(wanted) +
                " does not refer to a valid playable video file in this torrent.");
        }
    } else if (target.select_single) {
        if (videos.size() != 1) {
            throw std::runtime_error(
                "Torrent contains " + std::to_string(videos.size()) +
                " video files; explicit --file-index is required to prevent wrong-episode playback.");
        }
        chosen = &videos.front();
    } else {
        throw std::invalid_argument("Explicit target requires --file-index or --select-single.");
    }

    if (!target.expected_file_path.empty()
        && !normalizedPathMatchesTarget(metadata.name(), chosen->path, target.expected_file_path)) {
        throw std::runtime_error(
            "Target file path mismatch at index " +
            std::to_string(static_cast<int>(chosen->torrent_index)) +
            ": expected \"" + target.expected_file_path + "\", got \"" + chosen->path + "\".");
    }

    if (target.expected_file_size.has_value() && *target.expected_file_size > 0
        && chosen->size_bytes != *target.expected_file_size) {
        throw std::runtime_error(
            "Target file size mismatch at index " +
            std::to_string(static_cast<int>(chosen->torrent_index)) +
            ": expected " + std::to_string(*target.expected_file_size) +
            " bytes, got " + std::to_string(chosen->size_bytes) + " bytes.");
    }

    if (check_budget && metadata.piece_length() > 0) {
        const int piece_length = metadata.piece_length();
        bool fits = true;
        if (budget.mode == CacheBudgetMode::strict) {
            fits = (chosen->size_bytes / 40 >= piece_length);
        } else {
            try {
                planCacheBudget(chosen->size_bytes, piece_length, 4096, 8192, budget);
            } catch (const std::runtime_error&) {
                fits = false;
            }
        }
        if (!fits) {
            throw std::runtime_error("Selected video does not fit the selected cache budget policy.");
        }
    }

    return *chosen;
}

std::string formatManifestJson(const libtorrent::torrent_info& metadata)
{
#if LIBTORRENT_VERSION_NUM >= 20100
    const auto& files = metadata.layout();
#else
    const auto& files = metadata.files();
#endif
    const std::string torrent_name = metadata.name();
    const int num_files = files.num_files();
    std::int64_t total_size = 0;

    std::ostringstream files_json;
    files_json.imbue(std::locale::classic());
    files_json << '[';
    bool first = true;

    for (int index = 0; index < num_files; ++index) {
        const auto idx = lt::file_index_t{index};
        const auto flags = files.file_flags(idx);
        if ((flags & lt::file_storage::flag_pad_file)
            || (flags & lt::file_storage::flag_symlink)) {
            continue;
        }
        const std::string raw_path = files.file_path(idx);
        const std::string rel_path = num_files > 1
            ? stripTorrentRootPrefix(torrent_name, raw_path)
            : normalizeForwardPath(raw_path);
        const auto slash = rel_path.rfind('/');
        const std::string filename = (slash == std::string::npos) ? rel_path : rel_path.substr(slash + 1);
        const std::int64_t sz = files.file_size(idx);
        total_size += sz;

        if (!first) {
            files_json << ',';
        }
        first = false;
        files_json << "{\"torrent_file_index\":" << static_cast<int>(idx)
                   << ",\"relative_path\":";
        appendJsonEscaped(files_json, rel_path);
        files_json << ",\"filename\":";
        appendJsonEscaped(files_json, filename);
        files_json << ",\"size_bytes\":" << sz
                   << ",\"is_video\":" << (isVideoFile(filename) ? "true" : "false")
                   << '}';
    }
    files_json << ']';

    std::ostringstream root;
    root.imbue(std::locale::classic());
    root << "{\"ok\":true,\"info_hash\":";
    appendJsonEscaped(root, infoHashHex(metadata));
    root << ",\"torrent_name\":";
    appendJsonEscaped(root, torrent_name);
    root << ",\"total_files\":" << num_files
         << ",\"total_size_bytes\":" << total_size
         << ",\"files\":" << files_json.str() << '}';
    return root.str();
}

} // namespace intezam
