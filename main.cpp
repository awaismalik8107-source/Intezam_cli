#include "src/torrent_metadata.hpp"
#include "src/video_files.hpp"
#include "src/video_selection.hpp"
#include "src/cache_simulation.hpp"
#include "src/vlc_playback.hpp"

#include <libtorrent/version.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct CliOptions {
    std::chrono::milliseconds timeout{120000};
    std::chrono::seconds duration{0};
    double bitrate = 1024 * 1024;
    bool metadata_only = false;
    bool help = false;
    bool simulate = false;
    bool headless = false;
};

int parseSeconds(std::string_view argument, int maximum)
{
    int seconds = 0;
    const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), seconds);
    if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size()
        || seconds < 1 || seconds > maximum) {
        throw std::invalid_argument("Seconds must be a whole number from 1 to " + std::to_string(maximum) + ".");
    }
    return seconds;
}

CliOptions parseOptions(int argc, char* argv[])
{
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") options.help = true;
        else if (argument == "--metadata-only") options.metadata_only = true;
        else if (argument == "--simulate") options.simulate = true;
        else if (argument == "--headless") options.headless = true;
        else if (argument == "--timeout" || argument == "--simulate-seconds" || argument == "--bitrate") {
            if (++index >= argc) throw std::invalid_argument("Missing value for " + std::string(argument));
            const std::string_view value{argv[index]};
            if (argument == "--timeout") options.timeout = std::chrono::seconds{parseSeconds(value, 3600)};
            else if (argument == "--simulate-seconds") {
                options.duration = std::chrono::seconds{parseSeconds(value, 86400)};
                options.simulate = true;
            }
            else {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.bitrate);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                    || !std::isfinite(options.bitrate) || options.bitrate <= 0 || options.bitrate > 1e12) {
                    throw std::invalid_argument("Bitrate must be a positive number of bytes per second (at most 1 TB/s).");
                }
            }
        } else throw std::invalid_argument("Unknown option: " + std::string(argument) + ". Use --help.");
    }
    return options;
}

} // namespace

int main(int argc, char* argv[])
{
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        const auto cli = parseOptions(argc, argv);
        if (cli.help) {
            std::cout << "Usage: intezam [--timeout SECONDS] [--metadata-only]\n"
                      << "               [--bitrate BYTES_PER_SECOND] [--headless]\n"
                      << "               [--simulate | --simulate-seconds SECONDS]\n"
                      << "Retrieve metadata, select a video, then play verified cache bytes with LibVLC.\n"
                      << "--metadata-only preserves detection/selection without payload downloads.\n"
                      << "Defaults: metadata timeout 120 seconds, initial bitrate estimate 1048576 bytes/s.\n"
                      << "--simulate retains the cache test harness; --headless uses memory video/dummy audio.\n";
            return EXIT_SUCCESS;
        }
        intezam::MetadataOptions options;
        options.timeout = cli.timeout;

        std::cout << "========== INTEZAM CLI ==========\n\n"
                  << "Libtorrent version: " << LIBTORRENT_VERSION << '\n'
                  << "Enter your magnet link: " << std::flush;
        std::string magnet;
        if (!std::getline(std::cin, magnet)) {
            std::cout << "\nInput closed.\n";
            return EXIT_SUCCESS;
        }
        const auto first = magnet.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            throw std::invalid_argument("A magnet link is required.");
        }
        magnet = magnet.substr(first, magnet.find_last_not_of(" \t\r\n") - first + 1);

        std::cout << "Retrieving metadata only (timeout: "
                  << options.timeout.count() / 1000 << " seconds)...\n" << std::flush;
        const auto metadata = intezam::retrieveTorrentMetadata(magnet, options);
        const auto videos = intezam::detectVideoFiles(*metadata);
        std::cout << "\nTorrent: " << metadata->name()
                  << "\nTotal Files: " << metadata->num_files()
                  << "\nDetected Videos: " << videos.size() << "\n\n";
        if (videos.empty()) {
            std::cout << "No supported video files found.\n";
            return EXIT_SUCCESS;
        }

        intezam::displayVideos(videos, std::cout);
        const auto selected_video = intezam::selectVideo(videos, std::cin, std::cout);
        if (!selected_video) {
            std::cout << "\nSelection cancelled.\n";
            return EXIT_SUCCESS;
        }
        std::cout << "\nSelected: " << selected_video->path
                  << "\nTorrent file index: " << static_cast<int>(selected_video->torrent_index)
                  << "\nSize: " << intezam::formatVideoSize(selected_video->size_bytes)
                  << " (" << selected_video->size_bytes << " bytes)\n";
        if (cli.metadata_only) {
            std::cout << "Metadata-only selection complete. No video data downloaded.\n";
            return EXIT_SUCCESS;
        }
        if (cli.simulate) {
            intezam::CacheSimulationOptions simulation;
            simulation.cache.playback_bytes_per_second = cli.bitrate;
            simulation.duration = cli.duration;
            intezam::runCacheSimulation(magnet, metadata, *selected_video, simulation);
        } else {
            intezam::PlaybackOptions playback;
            playback.cache.playback_bytes_per_second = cli.bitrate;
            playback.vlc.headless = cli.headless;
            intezam::runVlcPlayback(magnet, metadata, *selected_video, playback);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
