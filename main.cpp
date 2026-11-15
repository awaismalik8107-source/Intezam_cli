#include "src/torrent_metadata.hpp"
#include "src/video_files.hpp"
#include "src/video_selection.hpp"
#include "src/cache_simulation.hpp"
#include "src/vlc_playback.hpp"
#include "src/playback_preparation.hpp"
#ifdef INTEZAM_QT_UI
#include "src/player_window.hpp"
#endif

#include <libtorrent/version.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
volatile std::sig_atomic_t selection_interrupted = 0;
void interruptSelection(int) { selection_interrupted = 1; }
class SelectionSignals {
public:
    SelectionSignals() : interrupt_(std::signal(SIGINT, interruptSelection)),
        terminate_(std::signal(SIGTERM, interruptSelection)) { selection_interrupted = 0; }
    ~SelectionSignals() { std::signal(SIGINT, interrupt_); std::signal(SIGTERM, terminate_); }
private:
    using Handler = void (*)(int);
    Handler interrupt_, terminate_;
};
#ifdef INTEZAM_QT_UI
constexpr const char* default_playback = "Default Qt playback window";
#else
constexpr const char* default_playback = "Default terminal playback controls (built without Qt)";
#endif

struct CliOptions {
    std::chrono::milliseconds timeout{120000};
    std::chrono::seconds duration{0};
    double bitrate = 1024 * 1024;
    bool metadata_only = false;
    bool manifest_json = false;
    bool help = false;
    bool simulate = false;
    bool headless = false;
    bool no_ui = false;
    bool bitrate_explicit = false;
    intezam::ExplicitVideoTarget target;
    intezam::CacheBudgetOptions budget{intezam::CacheBudgetMode::adaptive, 25};
    intezam::CliStatusOptions monitoring;
};

int parseNonNegativeInt(std::string_view argument, int maximum)
{
    int value = 0;
    const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size()
        || value < 0 || value > maximum) {
        throw std::invalid_argument("Value must be a whole number from 0 to " + std::to_string(maximum) + ".");
    }
    return value;
}

std::int64_t parseNonNegativeInt64(std::string_view argument)
{
    std::int64_t value = 0;
    const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || value < 0) {
        throw std::invalid_argument("File size must be a non-negative integer.");
    }
    return value;
}

std::string jsonEscapeError(std::string_view value)
{
    std::string out = "\"";
    for (unsigned char ch : value) {
        if (ch == '"') out += "\\\"";
        else if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch >= 0x20) out.push_back(static_cast<char>(ch));
    }
    out.push_back('"');
    return out;
}

int parsePositiveInteger(std::string_view argument, int maximum)
{
    int seconds = 0;
    const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), seconds);
    if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size()
        || seconds < 1 || seconds > maximum) {
        throw std::invalid_argument("Value must be a whole number from 1 to " + std::to_string(maximum) + ".");
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
        else if (argument == "--manifest-json") {
            options.metadata_only = true;
            options.manifest_json = true;
        }
        else if (argument == "--select-single") options.target.select_single = true;
        else if (argument == "--file-index") {
            if (++index >= argc) throw std::invalid_argument("Missing value for --file-index");
            options.target.torrent_file_index = parseNonNegativeInt(argv[index], 1000000);
        }
        else if (argument == "--expected-info-hash") {
            if (++index >= argc) throw std::invalid_argument("Missing value for --expected-info-hash");
            options.target.expected_info_hash = argv[index];
        }
        else if (argument == "--expected-file-path") {
            if (++index >= argc) throw std::invalid_argument("Missing value for --expected-file-path");
            options.target.expected_file_path = argv[index];
        }
        else if (argument == "--expected-file-size") {
            if (++index >= argc) throw std::invalid_argument("Missing value for --expected-file-size");
            options.target.expected_file_size = parseNonNegativeInt64(argv[index]);
        }
        else if (argument == "--simulate") options.simulate = true;
        else if (argument == "--headless") options.headless = true;
        else if (argument == "--no-ui") options.no_ui = true;
        else if (argument == "--monitor" || argument == "--monitor-log") {
            if (++index >= argc) throw std::invalid_argument("Missing value for " + std::string(argument));
            const std::string_view value{argv[index]};
            if (argument == "--monitor-log") options.monitoring.log_file = argv[index];
            else if (value == "compact") options.monitoring.verbosity = intezam::MonitorVerbosity::compact;
            else if (value == "detailed") options.monitoring.verbosity = intezam::MonitorVerbosity::detailed;
            else if (value == "debug") options.monitoring.verbosity = intezam::MonitorVerbosity::debug;
            else throw std::invalid_argument("Monitor level must be compact, detailed, or debug.");
        }
        else if (argument == "--strict-cache") options.budget.mode = intezam::CacheBudgetMode::strict;
        else if (argument == "--legacy-cache") options.budget.mode = intezam::CacheBudgetMode::automatic;
        else if (argument == "--cache-max-mib" || argument == "--cache-free-reserve-mib" || argument == "--buffer-seconds") {
            if (++index >= argc) throw std::invalid_argument("Missing value for " + std::string(argument));
            const auto value = parsePositiveInteger(argv[index], argument == "--buffer-seconds" ? 600 : 1048576);
            if (argument == "--cache-max-mib") options.budget.adaptive.maximum_bytes = std::int64_t{value} * 1024 * 1024;
            else if (argument == "--cache-free-reserve-mib") options.budget.adaptive.free_space_reserve_bytes = std::int64_t{value} * 1024 * 1024;
            else {
                if (value < 12) throw std::invalid_argument("Initial buffer horizon must be 12..600 seconds.");
                options.budget.adaptive.initial_forward_seconds = value;
                options.budget.adaptive.maximum_forward_seconds = std::max(90, value);
            }
        }
        else if (argument == "--cache-max-percent") {
            if (++index >= argc) throw std::invalid_argument("Missing value for --cache-max-percent");
            options.budget.maximum_percent = parsePositiveInteger(argv[index], 99);
            options.budget.mode = intezam::CacheBudgetMode::automatic;
            if (options.budget.maximum_percent < 3) throw std::invalid_argument("Cache ceiling must be from 3 to 99 percent.");
        }
        else if (argument == "--timeout" || argument == "--simulate-seconds" || argument == "--bitrate") {
            if (++index >= argc) throw std::invalid_argument("Missing value for " + std::string(argument));
            const std::string_view value{argv[index]};
            if (argument == "--timeout") options.timeout = std::chrono::seconds{parsePositiveInteger(value, 3600)};
            else if (argument == "--simulate-seconds") {
                options.duration = std::chrono::seconds{parsePositiveInteger(value, 86400)};
                options.simulate = true;
            }
            else {
                options.bitrate_explicit = true;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.bitrate);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                    || !std::isfinite(options.bitrate) || options.bitrate <= 0 || options.bitrate > 1e12) {
                    throw std::invalid_argument("Bitrate must be a positive number of bytes per second (at most 1 TB/s).");
                }
            }
        } else throw std::invalid_argument("Unknown option: " + std::string(argument) + ". Use --help.");
    }
    options.budget.initial_consumption_bytes_per_second = options.bitrate;
    return options;
}

} // namespace

int main(int argc, char* argv[])
{
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    bool manifest_json_mode = false;
    try {
        const auto cli = parseOptions(argc, argv);
        manifest_json_mode = cli.manifest_json;
        if (cli.help) {
            std::cout << "Usage: intezam [--timeout SECONDS] [--metadata-only] [--manifest-json]\n"
                      << "               [--file-index INDEX] [--expected-info-hash HEX] [--expected-file-path PATH]\n"
                      << "               [--expected-file-size BYTES] [--select-single]\n"
                      << "               [--monitor compact|detailed|debug] [--monitor-log PATH]\n"
                      << "               [--bitrate BYTES_PER_SECOND] [--headless | --no-ui]\n"
                      << "               [--cache-max-mib MIB] [--cache-free-reserve-mib MIB] [--buffer-seconds 12..600]\n"
                      << "               [--strict-cache | --legacy-cache | --cache-max-percent 3..99]\n"
                      << "               [--simulate | --simulate-seconds SECONDS]\n"
                      << "Retrieve metadata, select a video, then play verified cache bytes with LibVLC.\n"
                      << "--metadata-only preserves detection/selection without payload downloads.\n"
                      << "--manifest-json outputs metadata-only JSON file manifest and exits.\n"
                      << "--file-index selects an exact 0-based libtorrent file index without prompting.\n"
                      << "Normal playback prefetches bounded headers while you choose a video.\n"
                      << default_playback << "; adaptive time-based cache, absolute maximum 256 MiB.\n"
                      << "Free-space reserve 256 MiB; initial forward horizon 24 seconds; legacy modes retain percentage limits.\n"
                      << "Defaults: metadata timeout 120 seconds; header-derived bitrate, fallback 1048576 bytes/s.\n"
                      << "--bitrate overrides the header-derived startup estimate.\n"
                      << "--simulate retains the cache test harness; --headless uses memory video/dummy audio.\n";
            return EXIT_SUCCESS;
        }
        intezam::MetadataOptions options;
        options.timeout = cli.timeout;

        if (!cli.manifest_json) {
            std::cout << "========== INTEZAM CLI ==========\n\n"
                      << "Libtorrent version: " << LIBTORRENT_VERSION << '\n'
                      << "Enter your magnet link: " << std::flush;
        }
        std::string magnet;
        if (!std::getline(std::cin, magnet)) {
            if (cli.manifest_json) {
                std::cout << "{\"ok\":false,\"error\":\"A magnet link is required on stdin.\"}\n";
                return EXIT_FAILURE;
            }
            std::cout << "\nInput closed.\n";
            return EXIT_SUCCESS;
        }
        const auto first = magnet.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            throw std::invalid_argument("A magnet link is required.");
        }
        magnet = magnet.substr(first, magnet.find_last_not_of(" \t\r\n") - first + 1);

        if (!cli.manifest_json) {
            std::cout << "Retrieving metadata only (timeout: "
                      << options.timeout.count() / 1000 << " seconds)...\n" << std::flush;
        }
        const auto discovery = intezam::retrieveTorrentMetadataWithPeers(magnet, options);
        const auto& metadata = discovery.metadata;
        if (cli.manifest_json) {
            std::cout << intezam::formatManifestJson(*metadata) << '\n';
            return EXIT_SUCCESS;
        }
        const auto videos = intezam::detectVideoFiles(*metadata);
        std::cout << "\nTorrent: " << metadata->name()
                  << "\nTotal Files: " << metadata->num_files()
                  << "\nDetected Videos: " << videos.size() << "\n\n";
        if (videos.empty()) {
            if (cli.target.torrent_file_index.has_value() || cli.target.select_single) {
                throw std::runtime_error("No supported video files found in torrent.");
            }
            std::cout << "No supported video files found.\n";
            return EXIT_SUCCESS;
        }

        intezam::displayVideos(videos, std::cout);
        const bool explicit_target_mode =
            cli.target.torrent_file_index.has_value() || cli.target.select_single;
        SelectionSignals selection_signals;
        std::optional<intezam::PlaybackOptions> playback;
        if (!cli.metadata_only && !cli.simulate) {
            playback.emplace();
            playback->cache.bootstrap_peers = discovery.peers;
            playback->cache.playback_bytes_per_second = cli.bitrate;
            playback->cache.budget = cli.budget;
            playback->cache.estimate_bitrate_from_headers = !cli.bitrate_explicit;
            playback->monitoring = cli.monitoring;
            playback->vlc.headless = cli.headless;
            if (!explicit_target_mode) {
                if (const auto candidate = intezam::headerPrefetchCandidate(videos, metadata->piece_length(), cli.budget)) {
                    std::cout << "Preparing bounded headers in background: " << candidate->path
                        << "\nChoose any video; playback still requires your selection.\n" << std::flush;
                    playback->preparation = std::make_shared<intezam::PlaybackPreparation>(magnet, metadata, *candidate, playback->cache);
                }
            }
        }
        std::optional<intezam::VideoFile> selected_video;
        if (explicit_target_mode) {
            selected_video = intezam::resolveExplicitVideoTarget(
                *metadata, videos, cli.target, !cli.metadata_only && !cli.simulate, cli.budget);
        } else {
            selected_video = cli.metadata_only
                ? intezam::selectVideo(videos, std::cin, std::cout, [] { return selection_interrupted != 0; })
                : intezam::selectStreamingVideo(videos, metadata->piece_length(), std::cin, std::cout, cli.budget,
                    [] { return selection_interrupted != 0; });
        }
        if (!selected_video || selection_interrupted) {
            std::cout << "\nSelection cancelled.\n";
            return EXIT_SUCCESS;
        }
        if (playback && playback->preparation) {
            const auto prepared = playback->preparation->snapshot();
            if (!playback->preparation->matches(*selected_video) || !prepared.error.empty()
                || (prepared.source && !prepared.source->status().error.empty())) playback->preparation.reset();
        }
        if (selection_interrupted) { std::cout << "\nSelection cancelled.\n"; return EXIT_SUCCESS; }
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
            simulation.cache.budget = cli.budget;
            simulation.duration = cli.duration;
            intezam::runCacheSimulation(magnet, metadata, *selected_video, simulation);
        } else {
#ifdef INTEZAM_QT_UI
            if (!cli.headless && !cli.no_ui) intezam::runQtPlayback(magnet, metadata, *selected_video, *playback);
            else
#endif
                intezam::runVlcPlayback(magnet, metadata, *selected_video, *playback);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        if (manifest_json_mode) {
            std::cout << "{\"ok\":false,\"error\":" << jsonEscapeError(error.what()) << "}\n";
            return EXIT_FAILURE;
        }
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
