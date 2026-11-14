#pragma once

#include "torrent_video_source.hpp"

#include <libtorrent/create_torrent.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_flags.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unistd.h>

namespace test_support {
namespace lt = libtorrent;
namespace fs = std::filesystem;

inline void require(bool condition, const std::string& message)
{ if (!condition) throw std::runtime_error(message); }

inline lt::settings_pack loopbackSettings()
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
    settings.set_int(lt::settings_pack::unchoke_interval, 1);
    return settings;
}

struct RequestCounts {
    std::mutex mutex;
    std::map<int, int> pieces;
    int count(int piece) { std::lock_guard<std::mutex> lock(mutex); return pieces[piece]; }
};

struct PeerRequests final : lt::peer_plugin {
    explicit PeerRequests(RequestCounts& counts) : counts(counts) {}
    bool on_request(const lt::peer_request& request) override
    {
        std::lock_guard<std::mutex> lock(counts.mutex);
        ++counts.pieces[static_cast<int>(request.piece)];
        return false;
    }
    RequestCounts& counts;
};

struct TorrentRequests final : lt::torrent_plugin {
    explicit TorrentRequests(RequestCounts& counts) : counts(counts) {}
    std::shared_ptr<lt::peer_plugin> new_connection(const lt::peer_connection_handle&) override
    { return std::make_shared<PeerRequests>(counts); }
    RequestCounts& counts;
};

class LocalVideoTorrent {
public:
    explicit LocalVideoTorrent(const fs::path& media_file)
    {
        auto pattern = (fs::current_path() / "intezam-vlc-test-XXXXXX").string();
        const char* directory = mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("Cannot create playback fixture directory");
        root_ = directory;
        try {
            const auto source = root_ / "source";
            // Concurrent CLI tests use normal discovery settings. Give each
            // fixture its own info hash so clients cannot discover one another
            // and displace the real seeder under per-IP connection limits.
            const auto torrent_name = "Fixture-" + root_.filename().string();
            const auto collection = source / torrent_name;
            fs::create_directories(collection);
            std::ofstream prefix(collection / "prefix.txt", std::ios::binary);
            prefix << std::string(7000, 'p');
            prefix.close();
            const auto name = std::string{"video"} + media_file.extension().string();
            source_file_ = collection / name;
            std::error_code error;
            fs::create_hard_link(media_file, source_file_, error);
            if (error) fs::copy_file(media_file, source_file_);
            lt::create_torrent creator(std::vector<lt::create_file_entry>{
                {torrent_name + "/prefix.txt", 7000}, {torrent_name + "/" + name, static_cast<std::int64_t>(fs::file_size(media_file))}},
                16384, lt::create_torrent::v1_only);
            lt::set_piece_hashes(creator, source.string());
            auto encoded = creator.generate_buf();
            metadata_ = std::make_shared<lt::torrent_info>(lt::span<const char>{encoded}, lt::from_span);
            video_ = intezam::detectVideoFiles(*metadata_).front();
            session_ = std::make_unique<lt::session>(loopbackSettings());
            lt::add_torrent_params params;
            params.ti = metadata_;
            params.save_path = source.string();
            params.flags |= lt::torrent_flags::seed_mode;
            params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
            params.verified_pieces.resize(metadata_->num_pieces(), true);
            params.extensions.push_back([this](const lt::torrent_handle&, lt::client_data_t) {
                return std::make_shared<TorrentRequests>(counts);
            });
            seed = session_->add_torrent(params);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (session_->listen_port() == 0 || !seed.status().is_seeding) {
                session_->wait_for_alert(lt::milliseconds{10});
                std::vector<lt::alert*> alerts;
                session_->pop_alerts(&alerts);
                require(std::chrono::steady_clock::now() < deadline, "local media seeder startup");
            }
            magnet_ = lt::make_magnet_uri(params) + "&x.pe=127.0.0.1:" + std::to_string(session_->listen_port());
        } catch (...) { session_.reset(); fs::remove_all(root_); throw; }
    }
    ~LocalVideoTorrent()
    {
        session_.reset();
        std::error_code error;
        fs::remove_all(root_, error);
    }
    intezam::CacheStreamOptions options() const
    {
        intezam::CacheStreamOptions options;
        options.root = root_ / "client";
        options.settings = loopbackSettings();
        options.stall_timeout = std::chrono::seconds{15};
        return options;
    }
    std::shared_ptr<intezam::TorrentVideoSource> source() const
    { return source(options()); }
    std::shared_ptr<intezam::TorrentVideoSource> source(const intezam::CacheStreamOptions& configuration) const
    { return std::make_shared<intezam::TorrentVideoSource>(magnet_, metadata_, video_, configuration); }
    std::vector<unsigned char> expected(std::int64_t offset, std::size_t length) const
    {
        std::ifstream file(source_file_, std::ios::binary);
        file.seekg(offset);
        std::vector<unsigned char> bytes(length);
        file.read(reinterpret_cast<char*>(bytes.data()), length);
        require(file.gcount() == static_cast<std::streamsize>(length), "fixture reference read");
        return bytes;
    }
    const intezam::VideoFile& video() const { return video_; }
    std::shared_ptr<const lt::torrent_info> metadata() const { return metadata_; }
    const std::string& magnet() const { return magnet_; }
    RequestCounts counts;
    lt::torrent_handle seed;
private:
    fs::path root_;
    fs::path source_file_;
    std::string magnet_;
    std::shared_ptr<lt::torrent_info> metadata_;
    intezam::VideoFile video_{};
    std::unique_ptr<lt::session> session_;
};

} // namespace test_support
