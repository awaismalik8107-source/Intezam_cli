#include "torrent_cache_session.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_status.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace lt = libtorrent;
using namespace std::chrono_literals;

namespace intezam {
namespace {

PieceGeometry geometryFor(const std::shared_ptr<const lt::torrent_info>& metadata, const VideoFile& video)
{
    if (!metadata || !metadata->is_valid()) throw std::invalid_argument("Cache requires valid torrent metadata.");
    const auto& files = metadata->layout();
    if (static_cast<int>(video.torrent_index) < 0 || static_cast<int>(video.torrent_index) >= files.num_files()
        || video.size_bytes != files.file_size(video.torrent_index)
        || video.torrent_offset != files.file_offset(video.torrent_index)) {
        throw std::invalid_argument("Selected video does not match torrent metadata.");
    }
    return {video.size_bytes, video.torrent_offset, files.total_size(), files.piece_length()};
}

class VerifiedPiecePlugin final : public lt::torrent_plugin {
public:
    explicit VerifiedPiecePlugin(std::shared_ptr<PieceStore> store) : store_(std::move(store)) {}
    void on_piece_pass(lt::piece_index_t piece) override { store_->markVerified(static_cast<int>(piece)); }
    void on_piece_failed(lt::piece_index_t piece) override { store_->invalidate(static_cast<int>(piece)); }
private:
    std::shared_ptr<PieceStore> store_;
};

} // namespace

lt::settings_pack streamingSessionSettings()
{
    auto settings = metadataSessionSettings();
    settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    return settings;
}

TorrentCacheSession::TorrentCacheSession(const std::string& magnet,
    std::shared_ptr<const lt::torrent_info> metadata, const VideoFile& video, const CacheStreamOptions& options)
    : metadata_(std::move(metadata)), geometry_(geometryFor(metadata_, video)), options_(options),
      store_(PieceStore::create(geometry_, options.root)),
      controller_(geometry_, store_->slotCount(), options.playback_bytes_per_second),
      disk_state_(std::make_shared<CacheDiskState>())
{
    if (options.stall_timeout <= 0ms) throw std::invalid_argument("Cache stall timeout must be positive.");
    lt::error_code error;
    auto params = lt::parse_magnet_uri(magnet, error);
    if (error) throw std::invalid_argument("Invalid cache magnet: " + error.message());
    if (params.info_hashes != metadata_->info_hashes()) {
        // A hybrid magnet may specify only one of the two valid hashes.
        const auto expected = metadata_->info_hashes();
        if ((!params.info_hashes.has_v1() || params.info_hashes.v1 != expected.v1)
            && (!params.info_hashes.has_v2() || params.info_hashes.v2 != expected.v2)) {
            throw std::invalid_argument("Magnet and selected torrent metadata do not match.");
        }
    }
    known_peers_ = params.peers;
    params.ti = std::make_shared<lt::torrent_info>(*metadata_);
    lt::session_params session_params(options.settings);
    session_params.settings.set_int(lt::settings_pack::alert_mask,
        static_cast<int>(static_cast<std::uint32_t>(lt::alert_category::status
            | lt::alert_category::error | lt::alert_category::storage | lt::alert_category::connect)));
    session_params.settings.set_int(lt::settings_pack::stop_tracker_timeout, 1);
    // A streaming read must recover promptly when a temporarily unavailable peer
    // returns, rather than inheriting the default minute-long reconnect backoff.
    session_params.settings.set_int(lt::settings_pack::min_reconnect_time, 1);
    session_params.disk_io_constructor = cacheDiskConstructor(store_, geometry_, disk_state_);
    session_ = std::make_unique<lt::session>(std::move(session_params));
    store_->reconcile(controller_.window().pieces);
    addTorrent(std::move(params));
    schedule();
}

TorrentCacheSession::~TorrentCacheSession()
{
    // Join libtorrent before releasing the slot store. Outstanding read leases
    // retain the store/file until their readers finish, including during teardown.
    session_.reset();
}

lt::add_torrent_params TorrentCacheSession::safeParams(lt::add_torrent_params params)
{
    params.flags |= lt::torrent_flags::upload_mode | lt::torrent_flags::paused | lt::torrent_flags::default_dont_download
                  | lt::torrent_flags::no_verify_files;
    params.flags &= ~(lt::torrent_flags::auto_managed
                   | lt::torrent_flags::seed_mode | lt::torrent_flags::share_mode
                   | lt::torrent_flags::stop_when_ready | lt::torrent_flags::sequential_download);
    params.save_path = store_->directory().string();
    params.url_seeds.clear();
    params.file_priorities.assign(metadata_->num_files(), lt::dont_download);
    params.piece_priorities.assign(metadata_->num_pieces(), lt::dont_download);
    params.have_pieces.resize(metadata_->num_pieces(), false);
    params.have_pieces.clear_all();
    params.verified_pieces.clear();
    for (int piece : store_->verifiedPieces()) params.have_pieces.set_bit(lt::piece_index_t{piece});
    params.unfinished_pieces.clear();
    for (int piece : store_->residentPieces()) {
        if (store_->isVerified(piece)) continue;
        const auto written = store_->writtenBlocks(piece);
        lt::bitfield blocks(static_cast<int>(written.size()), false);
        for (std::size_t block = 0; block < written.size(); ++block) {
            if (written[block]) blocks.set_bit(static_cast<int>(block));
        }
        if (!blocks.none_set()) params.unfinished_pieces.emplace(lt::piece_index_t{piece}, std::move(blocks));
    }
    params.extensions.clear();
    params.extensions.push_back([store = store_](const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<VerifiedPiecePlugin>(store);
    });
    for (const auto& peer : known_peers_) {
        if (std::find(params.peers.begin(), params.peers.end(), peer) == params.peers.end()) params.peers.push_back(peer);
    }
    return params;
}

void TorrentCacheSession::addTorrent(lt::add_torrent_params params)
{
    lt::error_code error;
    torrent_ = session_->add_torrent(safeParams(std::move(params)), error);
    if (error) throw std::runtime_error("Cannot start cache torrent: " + error.message());
    waitUntil([&] {
        const auto state = torrent_.status().state;
        return state != lt::torrent_status::checking_resume_data && state != lt::torrent_status::checking_files;
    });
    scheduled_.clear();
    last_torrent_bytes_ = 0;
    schedule(); // publish reserved piece priorities before leaving upload-only mode
    torrent_.unset_flags(lt::torrent_flags::upload_mode);
    torrent_.resume();
    for (const auto& peer : known_peers_) torrent_.connect_peer(peer);
}

void TorrentCacheSession::drain(std::chrono::milliseconds wait,
                               const std::function<void(lt::alert*)>& inspect)
{
    checkCancelled();
    if (wait > 0ms) session_->wait_for_alert(wait);
    std::vector<lt::alert*> alerts;
    session_->pop_alerts(&alerts);
    for (auto* alert : alerts) {
        if (inspect) inspect(alert);
        if (const auto* failed = lt::alert_cast<lt::file_error_alert>(alert)) {
            throw std::runtime_error("Cache disk error: " + failed->error.message() + ". " + disk_state_->error());
        }
        if (const auto* failed = lt::alert_cast<lt::torrent_error_alert>(alert)) {
            throw std::runtime_error("Cache torrent error: " + failed->error.message());
        }
        if (const auto* failed = lt::alert_cast<lt::session_error_alert>(alert)) {
            throw std::runtime_error("Cache session error: " + failed->error.message());
        }
        if (const auto* connected = lt::alert_cast<lt::peer_connect_alert>(alert)) {
            if (std::find(known_peers_.begin(), known_peers_.end(), connected->endpoint) == known_peers_.end()) {
                known_peers_.push_back(connected->endpoint);
            }
        }
        if (const auto* disconnected = lt::alert_cast<lt::peer_disconnected_alert>(alert)) {
            last_peer_error_ = disconnected->message();
        }
    }
    if (const auto error = disk_state_->error(); !error.empty()) throw std::runtime_error("Cache storage failure: " + error);
}

void TorrentCacheSession::waitUntil(const std::function<bool()>& complete)
{
    const auto deadline = Clock::now() + 10s;
    while (!complete()) {
        if (Clock::now() >= deadline) throw std::runtime_error("Timed out synchronizing cache torrent state.");
        drain(20ms);
    }
}

void TorrentCacheSession::rebuild(const std::vector<int>& desired)
{
    torrent_.pause();
    bool paused = false;
    const auto deadline = Clock::now() + 10s;
    while (!paused) {
        drain(20ms, [&](lt::alert* alert) {
            if (const auto* event = lt::alert_cast<lt::torrent_paused_alert>(alert);
                event && event->handle == torrent_) paused = true;
        });
        if (Clock::now() >= deadline) throw std::runtime_error("Timed out pausing cache torrent for eviction.");
    }
    observeThroughput();
    torrent_.save_resume_data(lt::torrent_handle::save_info_dict);
    std::optional<lt::add_torrent_params> resume;
    while (!resume) {
        drain(20ms, [&](lt::alert* alert) {
            if (auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert); saved && saved->handle == torrent_) {
                resume = std::move(saved->params);
            }
            if (const auto* failed = lt::alert_cast<lt::save_resume_data_failed_alert>(alert);
                failed && failed->handle == torrent_) throw std::runtime_error("Cannot snapshot cache torrent: " + failed->error.message());
        });
        if (Clock::now() >= deadline) throw std::runtime_error("Timed out snapshotting cache torrent.");
    }
    session_->remove_torrent(torrent_);
    waitUntil([&] { return disk_state_->active_storages.load() == 0; });
    store_->reconcile(desired);
    addTorrent(std::move(*resume));
    ++rebuilds_;
}

void TorrentCacheSession::schedule()
{
    const auto urgent = urgentPieces();
    auto desired = controller_.window(urgent).pieces;
    for (int piece : urgent) {
        if (std::find(desired.begin(), desired.end(), piece) == desired.end()) desired.push_back(piece);
    }
    const int current = static_cast<int>((geometry_.video_offset + controller_.playbackPosition()) / geometry_.piece_length);
    // Admit playback first, then forward prefetch, then backward retention.
    std::stable_sort(desired.begin(), desired.end(), [current, &urgent](int left, int right) {
        const auto rank = [current, &urgent](int piece) {
            if (std::find(urgent.begin(), urgent.end(), piece) != urgent.end()) return std::int64_t{-1};
            return piece >= current ? std::int64_t{piece - current} : std::int64_t{current - piece} + 1000000000;
        };
        return rank(left) < rank(right);
    });
    if (store_->needsEviction(desired)) rebuild(desired);
    else store_->reconcile(desired);

    std::vector<int> admitted;
    for (int piece : desired) if (store_->contains(piece)) admitted.push_back(piece);
    if (admitted == scheduled_) return;
    torrent_.clear_piece_deadlines();
    std::vector<lt::download_priority_t> priorities(metadata_->num_pieces(), lt::dont_download);
    for (int piece : admitted) priorities[piece] = lt::top_priority;
    torrent_.prioritize_pieces(priorities);
    int deadline_ms = 0;
    for (int piece : admitted) {
        if (!store_->isVerified(piece)) torrent_.set_piece_deadline(lt::piece_index_t{piece}, deadline_ms);
        deadline_ms = std::min(60000, deadline_ms + 100);
    }
    scheduled_ = std::move(admitted);
}

std::vector<int> TorrentCacheSession::urgentPieces() const
{
    return read_demand_ ? geometry_.piecesForRange(read_demand_->offset, read_demand_->length) : std::vector<int>{};
}

void TorrentCacheSession::checkCancelled() const
{
    if (options_.cancelled && options_.cancelled()) throw std::runtime_error("Cache operation cancelled.");
}

void TorrentCacheSession::observeThroughput()
{
    const auto bytes = torrent_.status().total_payload_download;
    received_bytes_ += std::max<std::int64_t>(0, bytes - last_torrent_bytes_);
    last_torrent_bytes_ = bytes;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - sample_time_).count();
    if (seconds >= 0.5) {
        bool pending = false;
        for (int piece : scheduled_) if (!store_->isVerified(piece)) pending = true;
        if (pending || received_bytes_ != sampled_bytes_) {
            controller_.updateNetworkSpeed((received_bytes_ - sampled_bytes_) / seconds);
        }
        sampled_bytes_ = received_bytes_;
        sample_time_ = now;
    }
}

double TorrentCacheSession::forwardBufferSeconds() const
{
    auto global = geometry_.video_offset + controller_.playbackPosition();
    const auto begin = global;
    const auto end = geometry_.video_offset + geometry_.video_size;
    while (global < end) {
        const int piece = static_cast<int>(global / geometry_.piece_length);
        if (!store_->isVerified(piece)) break;
        global = std::min(end, (std::int64_t{piece} + 1) * geometry_.piece_length);
    }
    return (global - begin) / controller_.videoBitrate();
}

void TorrentCacheSession::pump(std::chrono::milliseconds wait)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    drain(wait);
    observeThroughput();
    controller_.updateBufferHealth(forwardBufferSeconds());
    schedule();
    const auto verified = store_->verifiedPieces();
    if (verified != last_verified_) {
        last_progress_ = Clock::now();
        last_verified_ = verified;
    }
    if (forwardBufferSeconds() == 0 && Clock::now() - last_progress_ > options_.stall_timeout) {
        const auto status = torrent_.status();
        throw std::runtime_error("Playback cache starved: no verified playback data before stall timeout. Peers: "
            + std::to_string(status.num_peers) + ". " + last_peer_error_);
    }
}

bool TorrentCacheSession::seek(std::int64_t byte_offset)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    const bool hit = store_->isRangeCached(byte_offset, 1);
    updatePosition(byte_offset);
    return hit;
}

void TorrentCacheSession::setPlaybackPosition(std::int64_t byte_offset)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    updatePosition(byte_offset);
}

void TorrentCacheSession::updatePosition(std::int64_t byte_offset)
{
    controller_.updatePlaybackPosition(byte_offset);
    last_progress_ = Clock::now();
    schedule();
}

void TorrentCacheSession::setPlaybackRate(double bytes_per_second)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    controller_.updateVideoBitrate(bytes_per_second);
    schedule();
}

void TorrentCacheSession::updateReadRequest(std::int64_t playback_offset, std::optional<VideoRange> demand)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    if (demand) geometry_.piecesForRange(demand->offset, demand->length);
    if (playback_offset != controller_.playbackPosition()) {
        controller_.updatePlaybackPosition(playback_offset);
        last_progress_ = Clock::now();
    }
    read_demand_ = demand;
    schedule();
}

bool TorrentCacheSession::isRangeCached(std::int64_t offset, std::int64_t length) const
{ return store_->isRangeCached(offset, length); }

std::optional<PieceStore::ReadLease> TorrentCacheSession::read(std::int64_t offset, std::int64_t length)
{ return store_->acquire(offset, length); }

CacheDiagnostics TorrentCacheSession::diagnostics() const
{
    std::lock_guard<std::mutex> control(control_mutex_);
    const auto urgent = urgentPieces();
    const auto window = controller_.window(urgent);
    auto required = window.pieces;
    for (int piece : urgent) if (std::find(required.begin(), required.end(), piece) == required.end()) required.push_back(piece);
    std::set<int> in_flight;
    for (const auto& partial : torrent_.get_download_queue()) {
        const int piece = static_cast<int>(partial.piece_index);
        if (!store_->isVerified(piece)
            && std::find(required.begin(), required.end(), piece) != required.end()) {
            in_flight.insert(piece);
        }
    }
    std::size_t pending = 0;
    std::size_t deferred = 0;
    for (int piece : required) {
        if (!store_->contains(piece)) ++deferred;
        else if (!store_->isVerified(piece)) ++pending;
    }
    std::size_t verified_segments = 0;
    std::size_t downloading_segments = 0;
    for (auto segment = window.first_segment; segment <= window.last_segment; ++segment) {
        const auto offset = segment * controller_.segmentSize();
        const auto length = std::min(controller_.segmentSize(), geometry_.video_size - offset);
        if (store_->isRangeCached(offset, length)) ++verified_segments;
        else {
            const auto pieces = geometry_.piecesForRange(offset, length);
            if (std::any_of(pieces.begin(), pieces.end(), [&](int piece) {
                return in_flight.count(piece) != 0;
            })) ++downloading_segments;
        }
    }
    const double buffered = forwardBufferSeconds();
    return {window, controller_.playbackPosition(), store_->budgetBytes(), store_->allocatedBytes(),
            received_bytes_, store_->residentPieces().size(), store_->verifiedPieces().size(), in_flight.size(),
            pending - std::min(pending, in_flight.size()),
            verified_segments, downloading_segments, deferred, rebuilds_, controller_.networkSpeed(),
            controller_.videoBitrate(), buffered, buffered > 0, urgent.size()};
}

} // namespace intezam
