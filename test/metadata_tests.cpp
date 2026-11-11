#include "torrent_metadata.hpp"
#include "video_files.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/disabled_disk_io.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_status.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace lt = libtorrent;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        auto pattern = (fs::temp_directory_path() / "intezam-test-XXXXXX").string();
        const auto directory = mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("Cannot create test directory");
        }
        path_ = directory;
        previous_ = fs::current_path();
        fs::current_path(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::current_path(previous_, error);
        fs::remove_all(path_, error);
    }

private:
    fs::path path_;
    fs::path previous_;
};

lt::settings_pack loopbackSettings()
{
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_str(lt::settings_pack::outgoing_interfaces, "127.0.0.1");
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
    settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    return settings;
}

std::shared_ptr<lt::torrent_info> fixtureMetadata()
{
    // Build metadata in memory. No video payload or .torrent file is written.
    lt::entry torrent;
    auto& info = torrent["info"];
    info["name"] = "Collection";
    info["piece length"] = 16384;
    info["pieces"] = std::string(40, 'x');
    for (const auto& name : {"cover.jpg", "Episode.MKV", "subtitles.srt"}) {
        lt::entry file;
        file["length"] = 8192;
        file["path"].list().emplace_back(name);
        info["files"].list().push_back(std::move(file));
    }
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent);
    return std::make_shared<lt::torrent_info>(
        lt::span<char const>{encoded}, lt::from_span);
}

struct RequestCounter : lt::peer_plugin {
    explicit RequestCounter(std::atomic<int>& requests) : requests_(requests) {}
    bool on_request(const lt::peer_request&) override
    {
        ++requests_;
        return false;
    }
    std::atomic<int>& requests_;
};

struct TorrentRequestCounter : lt::torrent_plugin {
    explicit TorrentRequestCounter(std::atomic<int>& requests) : requests_(requests) {}
    std::shared_ptr<lt::peer_plugin> new_connection(const lt::peer_connection_handle&) override
    {
        return std::make_shared<RequestCounter>(requests_);
    }
    std::atomic<int>& requests_;
};

void rejectsInvalidMagnetsAndTimeouts()
{
    for (const auto& magnet : {"", "not a magnet", "https://example.com/file.torrent",
                               "magnet:?dn=NoHash", "magnet:?xt=urn:btih:invalid"}) {
        try {
            intezam::retrieveTorrentMetadata(magnet);
            throw std::logic_error("Invalid magnet accepted");
        } catch (const std::invalid_argument&) {
        }
    }

    intezam::MetadataOptions options;
    options.settings = loopbackSettings();
    options.timeout = 100ms;
    lt::add_torrent_params params;
    params.ti = fixtureMetadata();
    const auto magnet = lt::make_magnet_uri(params);
    const auto started = std::chrono::steady_clock::now();
    try {
        intezam::retrieveTorrentMetadata(magnet, options);
        throw std::logic_error("Unreachable torrent did not time out");
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find("Timed out") != std::string::npos,
                "timeout has useful error");
    }
    require(std::chrono::steady_clock::now() - started < 3s, "bounded timeout");

    options.timeout = 0ms;
    try {
        intezam::retrieveTorrentMetadata(magnet, options);
        throw std::logic_error("Non-positive timeout accepted");
    } catch (const std::invalid_argument&) {
    }
}

void retrievesMetadataWithoutPayloadOrFilesystemWrites()
{
    std::atomic<int> payload_requests{0};
    lt::session_params seed_session_params(loopbackSettings());
    seed_session_params.disk_io_constructor = lt::disabled_disk_io_constructor;
    lt::session seed_session(std::move(seed_session_params));
    lt::add_torrent_params seed_params;
    seed_params.ti = fixtureMetadata();
    seed_params.save_path = fs::current_path().string();
    seed_params.flags |= lt::torrent_flags::seed_mode;
    seed_params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
    seed_params.extensions.push_back([&payload_requests](const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<TorrentRequestCounter>(payload_requests);
    });
    const auto seed = seed_session.add_torrent(seed_params);

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (seed_session.listen_port() == 0 && std::chrono::steady_clock::now() < deadline) {
        seed_session.wait_for_alert(50ms);
        std::vector<lt::alert*> alerts;
        seed_session.pop_alerts(&alerts);
    }
    require(seed_session.listen_port() != 0, "local seeder listening");

    intezam::MetadataOptions options;
    options.settings = loopbackSettings();
    options.timeout = 10s;
    // A magnet can request file priorities and web seeds. Neither may enable payload.
    const auto magnet = lt::make_magnet_uri(seed_params)
        + "&x.pe=127.0.0.1:" + std::to_string(seed_session.listen_port())
        + "&so=1&ws=http%3A%2F%2F127.0.0.1%3A1%2Fvideo";
    const auto metadata = intezam::retrieveTorrentMetadata(magnet, options);
    require(metadata != nullptr, "metadata received");
    require(metadata->info_hashes() == seed_params.ti->info_hashes(), "correct torrent");
    const auto videos = intezam::detectVideoFiles(*metadata);
    require(videos.size() == 1, "detect local fixture video");
    require(videos[0].torrent_index == lt::file_index_t{1}, "keep fixture original index");
    require(payload_requests.load() == 0, "no BitTorrent payload requests");
    require(seed.status().total_payload_upload == 0, "zero payload bytes uploaded");
    require(fs::is_empty(fs::current_path()), "no files or subdirectories created");
}

} // namespace

int main()
{
    try {
        TemporaryDirectory directory;
        rejectsInvalidMagnetsAndTimeouts();
        retrievesMetadataWithoutPayloadOrFilesystemWrites();
        std::cout << "Metadata-only local peer tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
