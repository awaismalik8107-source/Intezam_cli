#include "torrent_cache_session.hpp"
#include "stream_policy.hpp"

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
    VerifiedPiecePlugin(std::shared_ptr<PieceStore> store, std::function<void(int)> notification)
        : store_(std::move(store)), notification_(std::move(notification)) {}
    void on_piece_pass(lt::piece_index_t piece) override
    {
        store_->markVerified(static_cast<int>(piece));
        if (notification_) notification_(static_cast<int>(piece));
    }
    void on_piece_failed(lt::piece_index_t piece) override { store_->invalidate(static_cast<int>(piece)); }
private:
    std::shared_ptr<PieceStore> store_;
    std::function<void(int)> notification_;
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
      store_(PieceStore::create(geometry_, options.root, options.reserved_slots, options.budget)),
      controller_(geometry_, store_->slotCount(), options.playback_bytes_per_second, options.forward_buffer_target_seconds),
      disk_state_(std::make_shared<CacheDiskState>())
{
    if (options.stall_timeout <= 0ms) throw std::invalid_argument("Cache stall timeout must be positive.");
    updateReservation(options.playback_bytes_per_second);
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
    const auto initial = planAdmissions();
    store_->setProtectedPieces(initial.required);
    store_->admit(initial.required, initial.prefetch);
    addTorrent(std::move(params), initial);
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
    params.extensions.push_back([store = store_, notification = options_.on_piece_verified](const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<VerifiedPiecePlugin>(store, notification);
    });
    for (const auto& peer : known_peers_) {
        if (std::find(params.peers.begin(), params.peers.end(), peer) == params.peers.end()) params.peers.push_back(peer);
    }
    return params;
}

void TorrentCacheSession::addTorrent(lt::add_torrent_params params, const AdmissionPlan& plan)
{
    lt::error_code error;
    torrent_ = session_->add_torrent(safeParams(std::move(params)), error);
    if (error) throw std::runtime_error("Cannot start cache torrent: " + error.message());
    waitUntil([&] {
        const auto state = torrent_.status().state;
        return state != lt::torrent_status::checking_resume_data && state != lt::torrent_status::checking_files;
    });
    scheduled_.clear();
    scheduled_priorities_.clear();
    scheduled_deadlines_.clear();
    last_torrent_bytes_ = 0;
    // Storage was admitted by the constructor or quiescent rebuild. Publishing
    // priorities here must never recursively initiate another rebuild.
    applyPriorities(plan);
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

void TorrentCacheSession::rebuild(const AdmissionPlan& plan)
{
    const auto started = Clock::now();
    const auto resident_before = store_->residentPieces().size();
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
    const auto evicted = store_->reclaimOutside(plan.keep);
    store_->admit(plan.required, plan.prefetch);
    performance_.pieces_evicted += evicted.size();
    performance_.pieces_admitted += store_->residentPieces().size() - (resident_before - evicted.size());
    addTorrent(std::move(*resume), plan);
    ++rebuilds_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    performance_.rebuild_microseconds += elapsed;
    performance_.maximum_rebuild_microseconds = std::max<std::uint64_t>(performance_.maximum_rebuild_microseconds, elapsed);
}

void TorrentCacheSession::schedule()
{
    const auto plan = planAdmissions();
    const VideoRange bounds{plan.window.begin_byte, plan.window.end_byte - plan.window.begin_byte};
    if (last_window_ != bounds) { ++performance_.window_movements; last_window_ = bounds; }
    store_->setProtectedPieces(plan.required);
    const auto admitted = store_->admit(plan.required, plan.prefetch);
    if (shouldRebuild(plan, admitted)) {
        if (admitted.deferred_required.empty()) ++performance_.proactive_refills;
        rebuild(plan);
    }
    applyPriorities(plan);
}

TorrentCacheSession::AdmissionPlan TorrentCacheSession::planAdmissions()
{
    const auto urgent = urgentPieces();
    const auto window = controller_.planWindow(urgent);
    planned_window_ = window;
    auto desired = window.pieces;
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
    AdmissionPlan plan{window, urgent, {}, desired};
    const auto begin = window.current_segment * controller_.segmentSize();
    const auto current_pieces = geometry_.piecesForRange(begin, std::min(controller_.segmentSize(), geometry_.video_size - begin));
    for (int piece : current_pieces) {
        if (std::find(plan.required.begin(), plan.required.end(), piece) == plan.required.end()) plan.required.push_back(piece);
    }
    for (int piece : desired) {
        if (std::find(plan.required.begin(), plan.required.end(), piece) == plan.required.end()) plan.prefetch.push_back(piece);
    }
    return plan;
}

bool TorrentCacheSession::shouldRebuild(const AdmissionPlan& plan, const AdmissionResult& admission) const
{
    if (admission.deferred_required.empty() && admission.deferred_prefetch.empty()) return false;
    const auto eligible = store_->reclaimablePieces(plan.keep).size();
    if (eligible == 0) return false; // pins cannot justify an empty rebuild
    if (!admission.deferred_required.empty()) return true;
    if (eligible >= std::max<std::size_t>(2, store_->reservedSlots())) return true;
    // A small cache can expose only one cold slot at a time. Waiting for a
    // batch then makes the next forward piece urgent and strands paused reads.
    const auto position = controller_.playbackPosition();
    const bool missing_forward = std::any_of(admission.deferred_prefetch.begin(), admission.deferred_prefetch.end(),
        [&](int piece) { return std::int64_t{piece + 1} * geometry_.piece_length - geometry_.video_offset > position; });
    const double refill_lead = std::max(0.5, 2.0 * geometry_.piece_length / controller_.videoBitrate());
    return missing_forward && forwardBufferSeconds() <= refill_lead;
}

void TorrentCacheSession::applyPriorities(const AdmissionPlan& plan)
{
    std::vector<int> admitted;
    for (int piece : plan.required) if (store_->contains(piece)) admitted.push_back(piece);
    for (int piece : plan.prefetch) if (store_->contains(piece)) admitted.push_back(piece);
    const auto urgent = urgentPieces();
    const auto position = controller_.playbackPosition();
    const auto rate = controller_.videoBitrate();
    const bool completed_deadlines = std::any_of(scheduled_deadlines_.begin(), scheduled_deadlines_.end(),
        [&](const auto& entry) { return store_->isVerified(entry.first); });
    if (!retime_deadlines_ && !completed_deadlines && admitted == scheduled_ && urgent == scheduled_urgent_ && rate == scheduled_rate_
        && std::abs(position - scheduled_position_) < geometry_.piece_length / 2) return;
    const auto now = Clock::now();
    std::map<int, lt::download_priority_t> priorities;
    std::map<int, Clock::time_point> deadlines;
    for (int piece : admitted) {
        const auto offset = std::int64_t{piece} * geometry_.piece_length - geometry_.video_offset;
        const bool verified = store_->isVerified(piece);
        const bool needed = !verified && std::find(urgent.begin(), urgent.end(), piece) != urgent.end();
        const bool forward = offset + geometry_.piece_length > position;
        priorities[piece] = !verified && (needed || forward) ? lt::top_priority
            : forward ? lt::default_priority : lt::low_priority;
        if (!verified && (needed || forward)) {
            const auto milliseconds = needed ? 0 : pieceDeadlineMilliseconds(offset - position, rate);
            const auto proposed = now + std::chrono::milliseconds{milliseconds};
            deadlines[piece] = proposed;
        }
    }
    publishDeadlines(std::move(deadlines));
    publishPriorities(std::move(priorities));
    scheduled_ = std::move(admitted);
    scheduled_urgent_ = urgent;
    scheduled_rate_ = rate;
    scheduled_position_ = position;
    retime_deadlines_ = false;
}

void TorrentCacheSession::publishDeadlines(std::map<int, Clock::time_point> deadlines)
{
    for (const auto& [piece, deadline] : scheduled_deadlines_) {
        if (!deadlines.count(piece)) {
            torrent_.reset_piece_deadline(lt::piece_index_t{piece});
            ++performance_.deadline_resets;
        }
    }
    const auto now = Clock::now();
    for (auto& [piece, proposed] : deadlines) {
        const auto previous = scheduled_deadlines_.find(piece);
        // Keep existing requests unless this piece became meaningfully more urgent.
        if (!retime_deadlines_ && previous != scheduled_deadlines_.end() && proposed + 250ms >= previous->second) {
            proposed = previous->second;
            continue;
        }
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(proposed - now).count();
        torrent_.set_piece_deadline(lt::piece_index_t{piece}, static_cast<int>(std::clamp<std::int64_t>(milliseconds, 0, 60000)));
        ++performance_.deadline_updates;
    }
    scheduled_deadlines_ = std::move(deadlines);
}

void TorrentCacheSession::publishPriorities(std::map<int, lt::download_priority_t> priorities)
{
    std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> changes;
    for (const auto& [piece, priority] : priorities) {
        const auto previous = scheduled_priorities_.find(piece);
        if (previous == scheduled_priorities_.end() || previous->second != priority) {
            changes.emplace_back(lt::piece_index_t{piece}, priority);
        }
    }
    for (const auto& [piece, priority] : scheduled_priorities_) {
        if (!priorities.count(piece)) changes.emplace_back(lt::piece_index_t{piece}, lt::dont_download);
    }
    if (!changes.empty()) {
        torrent_.prioritize_pieces(changes);
        ++performance_.priority_updates;
    }
    scheduled_priorities_ = std::move(priorities);
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
        download_rate_ = (received_bytes_ - sampled_bytes_) / seconds;
        bool pending = false;
        for (int piece : scheduled_) if (!store_->isVerified(piece)) pending = true;
        if (pending || received_bytes_ != sampled_bytes_) {
            controller_.updateNetworkSpeed(download_rate_);
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
    controller_.reposition(byte_offset);
    retime_deadlines_ = true;
    last_progress_ = Clock::now();
    schedule();
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
    updateReservation(bytes_per_second);
    schedule();
}

void TorrentCacheSession::updateReservation(double bytes_per_second)
{
    auto reserved = options_.reserved_slots.value_or(PieceStore::automaticReservation(store_->slotCount()));
    if (!options_.reserved_slots) {
        const auto coverage = dataCapacity() / bytes_per_second;
        if (coverage < 1.0 || (store_->reservedSlots() == 0 && coverage < 1.25)) reserved = 0;
    }
    // Changing the target never discards a live slot. Any reservation debt is
    // repaid only when future admissions require the normal quiescent boundary.
    store_->setReservedSlots(reserved);
    controller_.updateCapacity(store_->slotCount() - reserved);
}

void TorrentCacheSession::updateReadRequest(std::int64_t playback_offset, std::optional<VideoRange> demand,
                                           PlaybackMovement movement)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    if (demand) geometry_.piecesForRange(demand->offset, demand->length);
    if (movement == PlaybackMovement::seek || playback_offset != controller_.playbackPosition()) {
        if (movement == PlaybackMovement::seek) {
            controller_.reposition(playback_offset);
            retime_deadlines_ = true;
        }
        else controller_.updatePlaybackPosition(playback_offset);
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
    const auto window = *planned_window_;
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
    auto performance = performance_;
    const auto inventory = store_->inventory();
    performance.total_slots = inventory.total;
    performance.free_slots = inventory.free;
    performance.reserved_slots = inventory.reserved_target;
    performance.protected_slots = inventory.protected_slots;
    performance.pinned_slots = inventory.pinned_slots;
    performance.disk_jobs = disk_state_->jobs.load();
    performance.disk_microseconds = disk_state_->work_microseconds.load();
    return {window, controller_.playbackPosition(), store_->budgetBytes(), store_->allocatedBytes(),
            received_bytes_, store_->residentPieces().size(), store_->verifiedPieces().size(), in_flight.size(),
            pending - std::min(pending, in_flight.size()),
            verified_segments, downloading_segments, deferred, rebuilds_, controller_.networkSpeed(),
            controller_.videoBitrate(), buffered, buffered > 0, urgent.size(), performance, download_rate_};
}

} // namespace intezam
