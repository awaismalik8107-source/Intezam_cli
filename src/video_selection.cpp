#include "video_selection.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <istream>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace intezam {

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
{
    if (videos.empty()) {
        return std::nullopt;
    }
    for (;;) {
        output << "Select a video (1-" << videos.size()
               << ", q to cancel): " << std::flush;
        std::string line;
        if (!std::getline(input, line)) {
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
    int piece_length, std::istream& input, std::ostream& output)
{
    if (piece_length <= 0) throw std::invalid_argument("Torrent piece length must be positive.");
    if (videos.empty()) return std::nullopt;
    const auto fits_piece = [piece_length](const VideoFile& video) {
        return video.size_bytes / 40 >= piece_length;
    };
    const auto eligible = std::count_if(videos.begin(), videos.end(), fits_piece);
    for (std::size_t index = 0; index < videos.size(); ++index) {
        if (!fits_piece(videos[index])) {
            output << "Video [" << index + 1 << "] unavailable: its 2.5% cache budget is "
                   << formatVideoSize(videos[index].size_bytes / 40) << ", below the "
                   << formatVideoSize(piece_length) << " torrent piece size.\n";
        }
    }
    if (eligible == 0) throw std::runtime_error("No video's 2.5% cache budget can fit one whole torrent piece.");
    if (eligible == 1 && videos.size() > 1) {
        const auto selected = std::find_if(videos.begin(), videos.end(), fits_piece);
        output << "Automatically selecting [" << std::distance(videos.begin(), selected) + 1 << "]: "
               << selected->path << " (the only candidate with enough piece budget).\n" << std::flush;
        return *selected;
    }
    for (;;) {
        auto selected = selectVideo(videos, input, output);
        if (!selected || fits_piece(*selected)) return selected;
        output << "That video cannot fit a torrent piece in its cache budget. Choose another video.\n";
    }
}

} // namespace intezam
