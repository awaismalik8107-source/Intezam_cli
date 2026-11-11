#include "video_selection.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <istream>
#include <locale>
#include <ostream>
#include <sstream>
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

} // namespace intezam
