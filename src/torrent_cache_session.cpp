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
    VerifiedPiecePlugin(std::shared_ptr<PieceStore> store, std::function<void(int)> notification,
        PieceGeometry geometry, std::shared_ptr<CacheDiskState> state)
        : store_(std::move(store)), notification_(std::move(notification)), geometry_(geometry), state_(std::move(state)) {}
    void on_piece_pass(lt::piece_index_t piece) override
    {
        const int index = static_cast<int>(piece);
        const bool already_verified = store_->isVerified(index);
        store_->markVerified(index);
        if (!already_verified) {
            const auto begin = std::max(geometry_.video_offset, std::int64_t{index} * geometry_.piece_length);
            const auto end = std::min(geometry_.video_offset + geometry_.video_size,
                std::int64_t{index + 1} * geometry_.piece_length);
            state_->verified_video_bytes += std::max<std::int64_t>(0, end - begin);
        }
        if (notification_) notification_(static_cast<int>(piece));
    }
    void on_piece_failed(lt::piece_index_t piece) override { store_->invalidate(static_cast<int>(piece)); }
private:
    std::shared_ptr<PieceStore> store_;
    std::function<void(int)> notification_;
    PieceGeometry geometry_;
    std::shared_ptr<CacheDiskState> state_;
};

CacheBudgetOptions initialBudget(const CacheStreamOptions& options)
{
    auto budget = options.budget;
    budget.initial_consumption_bytes_per_second = options.playback_bytes_per_second;
    return budget;
}

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
      store_(PieceStore::create(geometry_, options.root, options.reserved_slots, initialBudget(options))),
      controller_(geometry_, store_->slotCount(), options.playback_bytes_per_second, options.forward_buffer_target_seconds),
      disk_state_(std::make_shared<CacheDiskState>())
{
    header_read_mode_ = options.header_read_mode;
    if (options.stall_timeout <= 0ms) throw std::invalid_argument("Cache stall timeout must be positive.");
    if (options.budget.mode == CacheBudgetMode::adaptive) {
        adaptive_budget_.emplace(geometry_, options.budget.adaptive);
        adaptCache();
    }
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
    for (const auto& peer : options.bootstrap_peers) {
        if (peer.port() != 0 && !peer.address().is_unspecified()
            && std::find(known_peers_.begin(), known_peers_.end(), peer) == known_peers_.end()) known_peers_.push_back(peer);
    }
    params.ti = std::make_shared<lt::torrent_info>(*metadata_);
    lt::session_params session_params(options.settings);
    session_params.settings.set_int(lt::settings_pack::alert_mask,
        static_cast<int>(static_cast<std::uint32_t>(lt::alert_category::status
            | lt::alert_category::error | lt::alert_category::storage | lt::alert_category::connect)));
    session_params.settings.set_int(lt::settings_pack::stop_tracker_timeout, 1);
    // A streaming read must recover promptly when a temporarily unavailable peer
    // returns, rather than inheriting the default minute-long reconnect backoff.
    session_params.settings.set_int(lt::settings_pack::min_reconnect_time, 1);
    if (adaptive_budget_ || header_read_mode_) {
        // Finishing the current rolling window is temporary. Libtorrent's
        // default sends upload-only and disconnects seeds at that point, even
        // though later playback will need more pieces from those same peers.
        session_params.settings.set_bool(lt::settings_pack::close_redundant_connections, false);
    }
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
    params.extensions.push_back([store = store_, notification = options_.on_piece_verified, geometry = geometry_, state = disk_state_]
        (const lt::torrent_handle&, lt::client_data_t) {
        return std::make_shared<VerifiedPiecePlugin>(store, notification, geometry, state);
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
    planned_startup_indexes_ = header_read_mode_ ? std::vector<int>{} : startupIndexPieces(urgent);
    auto constraints = urgent;
    constraints.insert(constraints.end(), planned_startup_indexes_.begin(), planned_startup_indexes_.end());
    const auto window = controller_.planWindow(constraints);
    planned_window_ = window;
    if (header_read_mode_) {
        auto headers = window;
        headers.pieces = urgent;
        planned_window_ = headers;
        return {headers, urgent, {}, urgent};
    }
    auto desired = window.pieces;
    for (int piece : constraints) {
        if (std::find(desired.begin(), desired.end(), piece) == desired.end()) desired.push_back(piece);
    }
    const int current = static_cast<int>((geometry_.video_offset + controller_.playbackPosition()) / geometry_.piece_length);
    // Admit playback first, then forward prefetch, then backward retention.
    std::stable_sort(desired.begin(), desired.end(), [this, current, &urgent](int left, int right) {
        const auto rank = [this, current, &urgent](int piece) -> std::pair<int, std::int64_t> {
            if (std::find(urgent.begin(), urgent.end(), piece) != urgent.end()) return {0, piece};
            const auto offset = std::int64_t{piece} * geometry_.piece_length - geometry_.video_offset;
            if (startup_prefix_bytes_ > 0 && offset < startup_prefix_bytes_) return {1, piece};
            if (std::find(planned_startup_indexes_.begin(), planned_startup_indexes_.end(), piece)
                != planned_startup_indexes_.end()) return {2, piece};
            return piece >= current ? std::pair<int, std::int64_t>{3, piece - current}
                : std::pair<int, std::int64_t>{4, current - piece};
        };
        return rank(left) < rank(right);
    });
    AdmissionPlan plan{window, urgent, {}, desired};
    plan.required.insert(plan.required.end(), planned_startup_indexes_.begin(), planned_startup_indexes_.end());
    for (int piece : currentPieces()) {
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
    const bool buffer_low = adaptive_budget_ && forwardBufferSeconds() < adaptive_target_.low_seconds;
    const bool completed_deadlines = std::any_of(scheduled_deadlines_.begin(), scheduled_deadlines_.end(),
        [&](const auto& entry) { return store_->isVerified(entry.first); });
    if (!retime_deadlines_ && !completed_deadlines && buffer_low == scheduled_buffer_low_
        && admitted == scheduled_ && urgent == scheduled_urgent_ && rate == scheduled_rate_
        && startup_prefix_bytes_ == scheduled_startup_prefix_ && planned_startup_indexes_ == scheduled_startup_indexes_
        && std::abs(position - scheduled_position_) < geometry_.piece_length / 2) return;
    const auto now = Clock::now();
    std::map<int, lt::download_priority_t> priorities;
    std::map<int, Clock::time_point> deadlines;
    for (int piece : admitted) {
        const auto offset = std::int64_t{piece} * geometry_.piece_length - geometry_.video_offset;
        const bool verified = store_->isVerified(piece);
        const bool needed = !verified && std::find(urgent.begin(), urgent.end(), piece) != urgent.end();
        const bool forward = offset + geometry_.piece_length > position;
        const double seconds_ahead = forward ? std::max(0.0, double(offset - position) / rate) : -1;
        const bool startup_prefix = startup_prefix_bytes_ > 0 && offset < startup_prefix_bytes_;
        const auto index = std::find(planned_startup_indexes_.begin(), planned_startup_indexes_.end(), piece);
        const bool startup_index = index != planned_startup_indexes_.end();
        const int priority = !verified && (startup_prefix || startup_index) ? 7
            : playbackPiecePriority(needed, seconds_ahead, verified,
                adaptive_budget_ ? adaptive_target_.low_seconds : 2, buffer_low);
        priorities[piece] = lt::download_priority_t{static_cast<std::uint8_t>(priority)};
        // Deadlines cover imminent playback and the bounded startup work.
        // Other lookahead retains its ordinary priority.
        if (!verified && priority == 7) {
            auto milliseconds = needed ? 0 : pieceDeadlineMilliseconds(offset - position, rate);
            if (!needed && (startup_prefix || startup_index)) {
                const auto first = geometry_.video_offset / geometry_.piece_length;
                const auto prefix_pieces = (geometry_.video_offset % geometry_.piece_length + startup_prefix_bytes_ - 1)
                    / geometry_.piece_length + 1;
                const auto order = startup_index && !startup_prefix
                    ? prefix_pieces + std::distance(planned_startup_indexes_.begin(), index) : piece - first;
                // Startup has no advancing playback clock: the whole runway is
                // needed now. A short stagger keeps prefix holes ahead of indexes.
                milliseconds = static_cast<int>(std::min<std::int64_t>(order * 100, 60000));
            }
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
    scheduled_buffer_low_ = buffer_low;
    scheduled_startup_prefix_ = startup_prefix_bytes_;
    scheduled_startup_indexes_ = planned_startup_indexes_;
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
    auto pieces = header_read_mode_ ? std::vector<int>{} : metadata_pieces_;
    if (header_read_mode_ && !read_demand_) {
        const int first = static_cast<int>(geometry_.video_offset / geometry_.piece_length);
        // Start with one genuinely needed header piece, rather than briefly
        // advertising an empty/finished torrent before the reader can enqueue it.
        if (!store_->isVerified(first)) pieces.push_back(first);
    }
    if (read_demand_) {
        const auto demand = geometry_.piecesForRange(read_demand_->offset, read_demand_->length);
        pieces.insert(pieces.end(), demand.begin(), demand.end());
    }
    std::sort(pieces.begin(), pieces.end());
    pieces.erase(std::unique(pieces.begin(), pieces.end()), pieces.end());
    return pieces;
}

std::vector<int> TorrentCacheSession::currentPieces() const
{
    const auto begin = controller_.playbackPosition() / controller_.segmentSize() * controller_.segmentSize();
    return geometry_.piecesForRange(begin, std::min(controller_.segmentSize(), geometry_.video_size - begin));
}

std::vector<int> TorrentCacheSession::startupIndexPieces(const std::vector<int>& urgent) const
{
    if (startup_prefix_bytes_ == 0 || startup_index_candidates_.empty()) return {};
    auto required = geometry_.piecesForRange(0, startup_prefix_bytes_);
    const auto current = currentPieces();
    required.insert(required.end(), current.begin(), current.end());
    required.insert(required.end(), urgent.begin(), urgent.end());
    std::set<int> protected_pieces(required.begin(), required.end());
    const auto inventory = store_->inventory();
    const auto logical_slots = adaptive_budget_ ? std::min(inventory.total, adaptive_target_.desired_slots) : inventory.total;
    const auto window_slots = logical_slots > inventory.reserved_target ? logical_slots - inventory.reserved_target : 1;
    auto free_slots = inventory.free;
    for (const int piece : protected_pieces) {
        if (!store_->contains(piece)) {
            if (free_slots == 0) return {};
            --free_slots;
        }
    }
    std::vector<int> indexes;
    for (const int piece : startup_index_candidates_) {
        if (protected_pieces.count(piece)) continue;
        if (protected_pieces.size() >= window_slots) break;
        if (!store_->contains(piece)) {
            if (free_slots == 0) continue;
            --free_slots;
        }
        protected_pieces.insert(piece);
        indexes.push_back(piece);
    }
    return indexes;
}

void TorrentCacheSession::checkCancelled() const
{
    if (options_.cancelled && options_.cancelled()) throw std::runtime_error("Cache operation cancelled.");
}

void TorrentCacheSession::observeThroughput()
{
    const auto status = torrent_.status();
    const auto bytes = status.total_payload_download;
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
        const auto verified = disk_state_->verified_video_bytes.load();
        throughput_.observe(verified - sampled_verified_bytes_, seconds, pending, status.num_peers);
        sampled_verified_bytes_ = verified;
        sampled_bytes_ = received_bytes_;
        sample_time_ = now;
    }
}

void TorrentCacheSession::adaptCache()
{
    if (!adaptive_budget_) return;
    const auto now = Clock::now();
    const auto resident = store_->residentPieces();
    const auto protected_pieces = store_->protectedPieces();
    std::set<int> external(protected_pieces.begin(), protected_pieces.end());
    external.insert(metadata_pieces_.begin(), metadata_pieces_.end());
    // Preserve all unfinished resident pieces, conservatively including queued
    // work. Those inside the rolling range are already counted in its capacity.
    for (int piece : resident) if (!store_->isVerified(piece)) external.insert(piece);
    if (planned_window_) for (int piece : planned_window_->pieces) external.erase(piece);
    AdaptiveCacheInput input;
    input.consumption_bytes_per_second = controller_.videoBitrate();
    input.throughput = throughput_.estimate();
    input.verified_forward_seconds = forwardBufferSeconds();
    input.outside_protected_pieces = external.size();
    input.capacity_limit_slots = store_->growthLimitSlots();
    std::set<int> critical(protected_pieces.begin(), protected_pieces.end());
    const auto urgent = urgentPieces();
    critical.insert(urgent.begin(), urgent.end());
    const auto current = currentPieces();
    critical.insert(current.begin(), current.end());
    input.minimum_required_slots = critical.size() + (critical.size() + 6) / 7;
    adaptive_target_ = adaptive_budget_->update(input, std::chrono::duration<double>(now - created_).count());
    if (adaptive_target_.desired_slots > store_->slotCount()
        && (store_->slotCount() < critical.size()
            || now - last_growth_attempt_ >= std::chrono::duration<double>(options_.budget.adaptive.growth_interval_seconds))) {
        last_growth_attempt_ = now;
        if (!store_->growToSlots(adaptive_target_.desired_slots)) capacity_constraint_ = store_->growthError();
        else capacity_constraint_.clear();
    }
    updateReservation(controller_.videoBitrate());
    controller_.setAdaptiveForwardSeconds(std::max(0.001, adaptive_target_.forward_seconds));
    buffer_controller_.update(input.verified_forward_seconds,
        {adaptive_target_.startup_seconds, adaptive_target_.low_seconds,
            adaptive_target_.emergency_seconds, adaptive_target_.resume_seconds},
        read_demand_ && !store_->isRangeCached(read_demand_->offset, read_demand_->length), seeking_, input.throughput.stalled_seconds);
    seeking_ = false;
}

double TorrentCacheSession::forwardBufferSeconds() const
{
    return verifiedForwardBytes(geometry_, controller_.playbackPosition(),
        [this](int piece) { return store_->isVerified(piece); }) / controller_.videoBitrate();
}

void TorrentCacheSession::pump(std::chrono::milliseconds wait)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    drain(wait);
    observeThroughput();
    adaptCache();
    controller_.updateBufferHealth(forwardBufferSeconds());
    schedule();
    const auto verified = store_->verifiedPieces();
    if (verified != last_verified_) {
        last_progress_ = Clock::now();
        last_verified_ = verified;
    }
    if ((!header_read_mode_ || read_demand_) && forwardBufferSeconds() == 0
        && Clock::now() - last_progress_ > options_.stall_timeout) {
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
    seeking_ = true;
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
    adaptCache();
    updateReservation(bytes_per_second);
    schedule();
}

void TorrentCacheSession::setHeaderReadMode(bool enabled)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    if (header_read_mode_ == enabled) return;
    header_read_mode_ = enabled;
    last_progress_ = Clock::now();
    controller_.reposition(controller_.playbackPosition());
    schedule();
    if (!enabled) {
        torrent_.unset_flags(lt::torrent_flags::upload_mode);
        for (const auto& peer : known_peers_) torrent_.connect_peer(peer);
    }
}

void TorrentCacheSession::setProtectedMetadata(const std::vector<VideoRange>& ranges)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    if (!adaptive_budget_) return; // explicit legacy modes keep their old footprint
    std::set<int> pieces;
    for (const auto& range : ranges) {
        if (range.length == 0) continue;
        const auto covering = geometry_.piecesForRange(range.offset, range.length);
        pieces.insert(covering.begin(), covering.end());
    }
    auto required = pieces;
    const auto current = currentPieces();
    required.insert(current.begin(), current.end());
    if (required.size() > store_->growthLimitSlots()) {
        throw std::runtime_error("Container metadata and playback cannot fit the configured adaptive cache maximum.");
    }
    metadata_pieces_.assign(pieces.begin(), pieces.end());
    adaptCache();
    // Protection is authoritative even before cooldown permits capacity growth.
    // Required metadata borrows staging slots; fail clearly if still impossible.
    schedule();
}

void TorrentCacheSession::setStartupReadAhead(std::int64_t prefix_bytes, const std::vector<VideoRange>& index_ranges)
{
    if (prefix_bytes < 0 || prefix_bytes > geometry_.video_size) throw std::out_of_range("Startup prefix range");
    const auto limit = static_cast<std::size_t>(std::max(1, std::min(4, 8 * 1024 * 1024 / geometry_.piece_length)));
    std::vector<int> candidates;
    if (prefix_bytes > 0) {
        for (const auto& range : index_ranges) {
            if (range.offset < 0 || range.offset >= geometry_.video_size || range.length <= 0) continue;
            const auto length = std::min(range.length, geometry_.video_size - range.offset);
            const int first = static_cast<int>((geometry_.video_offset + range.offset) / geometry_.piece_length);
            const int last = static_cast<int>((geometry_.video_offset + range.offset + length - 1) / geometry_.piece_length);
            for (int piece = first; piece <= last && candidates.size() < limit; ++piece) {
                if (std::find(candidates.begin(), candidates.end(), piece) == candidates.end()) candidates.push_back(piece);
            }
            if (candidates.size() == limit) break;
        }
    }
    std::lock_guard<std::mutex> control(control_mutex_);
    if (prefix_bytes == startup_prefix_bytes_ && candidates == startup_index_candidates_) return;
    startup_prefix_bytes_ = prefix_bytes;
    startup_index_candidates_ = std::move(candidates);
    schedule();
}

void TorrentCacheSession::updateReservation(double bytes_per_second)
{
    const auto logical_slots = adaptive_budget_ ? std::min(store_->slotCount(), adaptive_target_.desired_slots) : store_->slotCount();
    auto reserved = options_.reserved_slots.value_or(PieceStore::automaticReservation(logical_slots));
    if (!options_.reserved_slots) {
        const auto coverage = dataCapacity() / bytes_per_second;
        if (coverage < 1.0 || (store_->reservedSlots() == 0 && coverage < 1.25)) reserved = 0;
    }
    // Changing the target never discards a live slot. Any reservation debt is
    // repaid only when future admissions require the normal quiescent boundary.
    reserved = std::min(reserved, logical_slots > 2 ? logical_slots - 2 : 0);
    if (adaptive_budget_) {
        auto required = urgentPieces();
        const auto current = currentPieces();
        required.insert(required.end(), current.begin(), current.end());
        std::sort(required.begin(), required.end());
        required.erase(std::unique(required.begin(), required.end()), required.end());
        reserved = std::min(reserved, logical_slots > required.size() ? logical_slots - required.size() : 0);
    }
    store_->setReservedSlots(reserved);
    controller_.updateCapacity(std::max<std::size_t>(1, logical_slots - reserved));
}

void TorrentCacheSession::updateReadRequest(std::int64_t playback_offset, std::optional<VideoRange> demand,
                                           PlaybackMovement movement)
{
    std::lock_guard<std::mutex> control(control_mutex_);
    if (demand) geometry_.piecesForRange(demand->offset, demand->length);
    if (movement == PlaybackMovement::seek || playback_offset != controller_.playbackPosition()) {
        if (movement == PlaybackMovement::seek) {
            controller_.reposition(playback_offset);
            seeking_ = true;
            retime_deadlines_ = true;
        }
        else controller_.updatePlaybackPosition(playback_offset);
        last_progress_ = Clock::now();
    }
    if (demand && demand != read_demand_) last_progress_ = Clock::now();
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
    CacheDiagnostics result{window, controller_.playbackPosition(), store_->budgetBytes(), store_->allocatedBytes(),
            received_bytes_, store_->residentPieces().size(), store_->verifiedPieces().size(), in_flight.size(),
            pending - std::min(pending, in_flight.size()),
            verified_segments, downloading_segments, deferred, rebuilds_, controller_.networkSpeed(),
            controller_.videoBitrate(), buffered, buffered > 0, urgent.size(), performance, download_rate_};
    result.useful_throughput = throughput_.estimate();
    result.adaptive_target = adaptive_target_;
    result.buffer_state = buffer_controller_.state();
    result.adaptive_budget = adaptive_budget_.has_value();
    result.desired_capacity_bytes = static_cast<std::int64_t>(adaptive_budget_ ? adaptive_target_.desired_slots : inventory.total) * geometry_.piece_length;
    result.physical_capacity_bytes = static_cast<std::int64_t>(inventory.total) * geometry_.piece_length;
    result.occupied_capacity_bytes = static_cast<std::int64_t>(inventory.occupied) * geometry_.piece_length;
    result.shrink_deferred = result.desired_capacity_bytes < result.physical_capacity_bytes;
    result.capacity_constraint = capacity_constraint_;
    const auto status = torrent_.status();
    result.connected_peers = status.num_peers;
    result.torrent_finished = status.is_finished;
    result.last_peer_error = last_peer_error_;
    result.header_read_mode = header_read_mode_;
    if (read_demand_) {
        const auto pieces = geometry_.piecesForRange(read_demand_->offset, read_demand_->length);
        if (!pieces.empty() && !store_->isVerified(pieces.front())) {
            result.actual_blocking_priority = static_cast<std::uint8_t>(torrent_.piece_priority(lt::piece_index_t{pieces.front()}));
            const auto found = scheduled_priorities_.find(pieces.front());
            if (found != scheduled_priorities_.end()) result.planned_blocking_priority = static_cast<std::uint8_t>(found->second);
        }
    }
    if (adaptive_target_.capacity_limited && result.capacity_constraint.empty()) {
        result.capacity_constraint = "Desired safety window exceeds the configured storage or free-space limit.";
    }
    return result;
}

} // namespace intezam
