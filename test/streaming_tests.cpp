#include "torrent_cache_session.hpp"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/disabled_disk_io.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_flags.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace lt = libtorrent;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

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
    settings.set_int(lt::settings_pack::unchoke_interval, 1);
    return settings;
}

std::shared_ptr<lt::torrent_info> metadataFixture(bool corrupt = false)
{
    lt::entry torrent;
    auto& info = torrent["info"];
    info["name"] = "Fixture";
    info["piece length"] = 65536;
    for (const auto& entry : std::vector<std::pair<std::string, int>>{
             {"prefix.txt", 7000}, {"video.MKV", 16 * 1024 * 1024}, {"suffix.txt", 100000}}) {
        lt::entry file;
        file["length"] = entry.second;
        file["path"].list().emplace_back(entry.first);
        info["files"].list().push_back(std::move(file));
    }
    const int total = 7000 + 16 * 1024 * 1024 + 100000;
    std::string hashes;
    std::vector<char> zeroes(65536, corrupt ? 1 : 0);
    for (int offset = 0; offset < total; offset += 65536) {
        const auto hash = lt::hasher(zeroes.data(), std::min(65536, total - offset)).final();
        hashes.append(hash.data(), 20);
    }
    info["pieces"] = hashes;
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent);
    return std::make_shared<lt::torrent_info>(lt::span<const char>{encoded}, lt::from_span);
}

std::shared_ptr<lt::torrent_info> v2MetadataFixture(bool hybrid)
{
    constexpr int size = 16 * 1024 * 1024 + 123;
    lt::create_torrent creator(std::vector<lt::create_file_entry>{{"Fixture/video.MKV", size}},
                               65536, hybrid ? lt::create_flags_t{} : lt::create_torrent::v2_only);
    const std::vector<char> zeroes(16384, 0);
    for (int position = 0; position < size; position += 65536) {
        std::vector<lt::sha256_hash> hashes(4);
        for (int block = 0; block < 4 && position + block * 16384 < size; ++block) {
            hashes[block] = lt::hasher256(zeroes.data(), std::min(16384, size - position - block * 16384)).final();
        }
        while (hashes.size() > 1) {
            std::vector<lt::sha256_hash> parents;
            for (std::size_t index = 0; index < hashes.size(); index += 2) {
                lt::hasher256 hash;
                hash.update(hashes[index].data(), 32);
                hash.update(hashes[index + 1].data(), 32);
                parents.push_back(hash.final());
            }
            hashes = std::move(parents);
        }
        creator.set_hash2(lt::file_index_t{0}, lt::piece_index_t::diff_type{position / 65536}, hashes[0]);
    }
    if (hybrid) {
        const std::vector<char> piece(65536, 0);
        for (const auto index : creator.piece_range()) {
            creator.set_hash(index, lt::hasher(piece.data(), creator.piece_size(index)).final());
        }
    }
    auto encoded = creator.generate_buf();
    return std::make_shared<lt::torrent_info>(lt::span<const char>{encoded}, lt::from_span);
}

struct Requests {
    std::mutex mutex;
    std::map<int, int> pieces;
    int count(int piece)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return pieces[piece];
    }
};

struct PeerCounter : lt::peer_plugin {
    explicit PeerCounter(Requests& requests) : requests(requests) {}
    bool on_request(const lt::peer_request& request) override
    {
        std::lock_guard<std::mutex> lock(requests.mutex);
        ++requests.pieces[static_cast<int>(request.piece)];
        return false;
    }
    Requests& requests;
};

struct TorrentCounter : lt::torrent_plugin {
    explicit TorrentCounter(Requests& requests) : requests(requests) {}
    std::shared_ptr<lt::peer_plugin> new_connection(const lt::peer_connection_handle&) override
    { return std::make_shared<PeerCounter>(requests); }
    Requests& requests;
};

void waitForRange(intezam::TorrentCacheSession& cache, std::int64_t offset)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!cache.isRangeCached(offset, 16384)) {
        try { cache.pump(20ms); }
        catch (...) {
            const auto status = cache.diagnostics();
            std::cerr << "Failed at " << offset << ": verified=" << status.verified_pieces
                      << " pending=" << status.downloading_pieces << " queued=" << status.queued_pieces
                      << " deferred=" << status.deferred_pieces << " rebuilds=" << status.torrent_rebuilds << '\n';
            throw;
        }
        require(std::chrono::steady_clock::now() < deadline, "range did not become verified");
    }
}

void verifyMeasuredDisk(const intezam::TorrentCacheSession& cache)
{
    std::int64_t measured = 0;
    struct stat status{};
    require(lstat(cache.directory().parent_path().c_str(), &status) == 0, "stat cache root");
    measured += status.st_blocks * 512LL;
    require(lstat(cache.directory().c_str(), &status) == 0, "stat cache directory");
    measured += status.st_blocks * 512LL;
    for (const auto& entry : fs::directory_iterator(cache.directory())) {
        require(entry.path().filename() == "stream.cache" || entry.path().filename() == "session.lock",
                "no ordinary torrent payload files");
        require(lstat(entry.path().c_str(), &status) == 0, "stat cache file");
        measured += status.st_blocks * 512LL;
    }
    const auto diagnostics = cache.diagnostics();
    require(measured == diagnostics.allocated_bytes, "independently measured disk allocation matches");
    require(measured <= diagnostics.budget_bytes, "strict allocated-disk budget");
}

enum class TorrentKind { v1, v2, hybrid };

void runsSlidingCache(const fs::path& root, TorrentKind kind)
{
    Requests requests;
    lt::session_params seed_session_params(loopbackSettings());
    seed_session_params.disk_io_constructor = lt::disabled_disk_io_constructor;
    lt::session seed_session(std::move(seed_session_params));
    lt::add_torrent_params seed_params;
    seed_params.ti = kind == TorrentKind::v1 ? metadataFixture() : v2MetadataFixture(kind == TorrentKind::hybrid);
    seed_params.save_path = root.string();
    seed_params.flags |= lt::torrent_flags::seed_mode;
    seed_params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
    seed_params.verified_pieces.resize(seed_params.ti->num_pieces(), true);
    seed_params.extensions.push_back([&](const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<TorrentCounter>(requests);
    });
    const auto seed = seed_session.add_torrent(seed_params);
    const auto listen_deadline = std::chrono::steady_clock::now() + 3s;
    while (seed_session.listen_port() == 0 || !seed.status().is_seeding) {
        seed_session.wait_for_alert(10ms);
        require(std::chrono::steady_clock::now() < listen_deadline, "seeder listening");
    }
    const auto magnet = lt::make_magnet_uri(seed_params)
        + "&x.pe=127.0.0.1:" + std::to_string(seed_session.listen_port());
    intezam::MetadataOptions metadata_options;
    metadata_options.settings = loopbackSettings();
    metadata_options.timeout = 5s;
    const auto client_metadata = intezam::retrieveTorrentMetadata(magnet, metadata_options);
    require(requests.count(0) == 0, "metadata handoff requested no payload");
    const auto videos = intezam::detectVideoFiles(*client_metadata);
    require(videos.size() == 1, "selected fixture video");
    intezam::CacheStreamOptions options;
    options.root = root;
    options.settings = loopbackSettings();
    options.playback_bytes_per_second = 65536;
    options.stall_timeout = 10s;
    fs::path session_path;
    {
        intezam::TorrentCacheSession cache(magnet, client_metadata, videos[0], options);
        session_path = cache.directory();
        waitForRange(cache, 0);
        verifyMeasuredDisk(cache);
        auto reader = cache.read(0, 16384);
        require(reader.has_value(), "verified disk read lease");
        const auto bytes = reader->read();
        require(std::all_of(bytes.begin(), bytes.end(), [](char byte) { return byte == 0; }), "verified content matches seed");
        const int previous_requests = requests.count(0);
        require(cache.seek(100), "cached seek hit");
        cache.pump(10ms);
        require(requests.count(0) == previous_requests, "cached seek needs no piece redownload");

        require(!cache.seek(8 * 1024 * 1024), "uncached seek miss");
        require(!cache.isRangeCached(8 * 1024 * 1024, 16384), "desired window is not immediately readable");
        waitForRange(cache, 8 * 1024 * 1024);
        require(cache.isRangeCached(0, 16384), "pinned read survives distant seek");
        reader.reset();
        cache.setPlaybackRate(262144); // Fill available slots after the reader releases one.
        cache.pump(10ms);
        // Batch reclamation may retain the released piece until space is needed.
        // Force another cold window before testing eviction and re-download.
        cache.seek(10 * 1024 * 1024);
        waitForRange(cache, 10 * 1024 * 1024);
        verifyMeasuredDisk(cache);
        require(cache.diagnostics().torrent_rebuilds > 0, "eviction synchronized through public resume API");

        require(!cache.seek(0), "evicted backward seek is a miss");
        waitForRange(cache, 0);
        require(requests.count(0) > previous_requests, "libtorrent redownloads evicted data on demand");
        for (int position : {4, 12, 6}) {
            cache.seek(position * 1024 * 1024);
            waitForRange(cache, position * 1024 * 1024);
            verifyMeasuredDisk(cache);
        }
        cache.seek(videos[0].size_bytes - 16384);
        waitForRange(cache, videos[0].size_bytes - 16384);
        verifyMeasuredDisk(cache);
        const auto diagnostics = cache.diagnostics();
        const char* label = kind == TorrentKind::v1 ? "V1" : kind == TorrentKind::v2 ? "V2" : "Hybrid";
        std::cout << label << " streaming allocated disk: " << diagnostics.allocated_bytes
                  << " / " << diagnostics.budget_bytes << " bytes; received: " << diagnostics.received_bytes
                  << "; synchronized evictions: " << diagnostics.torrent_rebuilds << '\n';
        require(diagnostics.received_bytes < videos[0].size_bytes / 2, "window scheduling avoids a full download");
        {
            std::lock_guard<std::mutex> lock(requests.mutex);
            const int first = static_cast<int>(videos[0].torrent_offset / 65536);
            const int last = static_cast<int>((videos[0].torrent_offset + videos[0].size_bytes - 1) / 65536);
            for (const auto& requested : requests.pieces) {
                require(requested.first >= first && requested.first <= last,
                        "no payload requests for unrelated torrent files");
            }
        }
    }
    require(!fs::exists(session_path), "streaming cleanup removes session directory");
    require(fs::is_empty(root), "seeder and client created no ordinary payload files");

    options.stall_timeout = 100ms;
    bool starved = false;
    try {
        intezam::TorrentCacheSession disconnected(lt::make_magnet_uri(seed_params), client_metadata, videos[0], options);
        while (true) disconnected.pump(20ms);
    } catch (const std::runtime_error& error) {
        starved = std::string(error.what()).find("starved") != std::string::npos;
    }
    require(starved, "network starvation is reported");
    require(fs::is_empty(root), "failure cleanup removes cache");

    if (kind == TorrentKind::v1) {
        auto corrupt_params = seed_params;
        corrupt_params.ti = metadataFixture(true);
        seed_session.add_torrent(corrupt_params);
        const auto corrupt_magnet = lt::make_magnet_uri(corrupt_params)
            + "&x.pe=127.0.0.1:" + std::to_string(seed_session.listen_port());
        const auto corrupt_video = intezam::detectVideoFiles(*corrupt_params.ti).front();
        options.stall_timeout = 500ms;
        bool rejected = false;
        {
            intezam::TorrentCacheSession corrupt(corrupt_magnet, corrupt_params.ti, corrupt_video, options);
            try { while (true) corrupt.pump(20ms); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("starved") != std::string::npos;
            }
            require(!corrupt.isRangeCached(0, 16384), "corrupt payload is never readable");
            require(corrupt.diagnostics().received_bytes > 0, "corrupt fixture actually sent payload");
        }
        require(rejected, "hash-failing payload buffers then reports starvation");
        require(fs::is_empty(root), "corrupt payload failure cleans up disk");
    }
}

} // namespace

int main()
{
    auto pattern = (fs::current_path() / "intezam-stream-test-XXXXXX").string();
    const auto created = mkdtemp(pattern.data());
    if (!created) return 1;
    const fs::path root{created};
    try {
        runsSlidingCache(root, TorrentKind::v1);
        runsSlidingCache(root, TorrentKind::v2);
        runsSlidingCache(root, TorrentKind::hybrid);
        fs::remove_all(root);
        std::cout << "Loopback streaming, cached/uncached seeking and starvation tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        fs::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
