#include "torrent_video_source.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace intezam {

TorrentVideoSource::TorrentVideoSource(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata, const VideoFile& video, CacheStreamOptions options)
    : video_size_(video.size_bytes), torrent_offset_(video.torrent_offset),
      piece_length_(metadata ? metadata->piece_length() : 0), read_timeout_(options.stall_timeout),
      requested_rate_(options.playback_bytes_per_second)
{
    const auto external_cancel = options.cancelled;
    options.cancelled = [this, external_cancel] {
        return stopping_.load() || (external_cancel && external_cancel());
    };
    const auto external_notification = options.on_piece_verified;
    options.on_piece_verified = [signal = read_signal_, external_notification](int piece) {
        signal->notify();
        if (external_notification) external_notification(piece);
    };
    cache_ = std::make_unique<TorrentCacheSession>(magnet, std::move(metadata), video, options);
    directory_ = cache_->directory();
    capacity_ = cache_->dataCapacity();
    // Demuxers may read indexes and analyze packets before exposing tracks.
    // Permit bounded startup analysis, always less than the whole selected file.
    probe_limit_ = std::min<std::int64_t>(video_size_ - video_size_ / 10,
        std::max<std::int64_t>(8 * 1024 * 1024, std::min<std::int64_t>(16 * 1024 * 1024, capacity_ * 8)));
    status_.cache = cache_->diagnostics();
    worker_ = std::thread(&TorrentVideoSource::run, this);
}

TorrentVideoSource::~TorrentVideoSource() { stop(); }

void TorrentVideoSource::checkReadState(std::uint64_t epoch) const
{
    if (stopping_.load() || suspended_ || epoch != epoch_) throw ReadCancelled{};
    if (!status_.error.empty()) throw std::runtime_error(status_.error);
}

std::size_t TorrentVideoSource::read(std::int64_t offset, void* buffer, std::size_t length)
{ return readVerified(offset, buffer, length, ReadPurpose::playback); }

std::size_t TorrentVideoSource::readMetadata(std::int64_t offset, void* buffer, std::size_t length)
{ return readVerified(offset, buffer, length, ReadPurpose::metadata); }

std::size_t TorrentVideoSource::readVerified(std::int64_t offset, void* buffer, std::size_t length, ReadPurpose purpose)
{
    if (offset < 0 || offset > video_size_) throw std::out_of_range("Video byte offset");
    if (length == 0 || offset == video_size_) return 0;
    if (!buffer) throw std::invalid_argument("Video read buffer is null");
    const auto until_boundary = piece_length_ - (torrent_offset_ + offset) % piece_length_;
    length = std::min<std::uint64_t>({length, 16384, static_cast<std::uint64_t>(video_size_ - offset),
                                    static_cast<std::uint64_t>(until_boundary)});
    std::unique_lock<std::mutex> lock(read_signal_->mutex);
    const auto epoch = epoch_;
    checkReadState(epoch);
    if (probing_ && purpose != ReadPurpose::prebuffer) {
        // A tiny header read still requires a complete verified torrent piece.
        // Charge that selected-file footprint so sparse index scans cannot hide
        // full-file downloads behind small callback requests.
        const auto begin = std::max<std::int64_t>(0, offset - (torrent_offset_ + offset) % piece_length_);
        const auto end = std::min(video_size_, offset + until_boundary);
        accountProbeRange(begin, static_cast<std::size_t>(end - begin));
    }
    if (purpose == ReadPurpose::playback && !container_.isMetadata(offset)) playback_offset_ = offset;
    auto request = std::make_shared<PendingRead>(PendingRead{{offset, static_cast<std::int64_t>(length)}, epoch});
    pending_.push_back(request);
    read_signal_->changed.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + read_timeout_;
    std::optional<std::chrono::steady_clock::time_point> waiting_since;
    const auto finish_wait = [&] {
        if (!waiting_since) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - *waiting_since).count();
        auto& metrics = status_.performance;
        metrics.wait_microseconds += elapsed;
        metrics.maximum_wait_microseconds = std::max<std::uint64_t>(metrics.maximum_wait_microseconds, elapsed);
        if (elapsed >= 50000) { ++metrics.stalls; metrics.stall_microseconds += elapsed; }
        waiting_since.reset();
    };
    const auto remove_request = [&] {
        const auto found = std::find(pending_.begin(), pending_.end(), request);
        if (found != pending_.end()) pending_.erase(found);
        read_signal_->changed.notify_all();
    };
    try {
        for (;;) {
            checkReadState(epoch);
            if (!pending_.empty() && pending_.front() == request) {
                if (auto lease = cache_->read(offset, length)) {
                    if (!waiting_since) ++status_.performance.cache_hits;
                    finish_wait();
                    remove_request();
                    lock.unlock();
                    const auto reading_started = std::chrono::steady_clock::now();
                    const auto bytes = lease->read();
                    const auto reading_time = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - reading_started).count();
                    std::memcpy(buffer, bytes.data(), bytes.size());
                    lock.lock();
                    status_.delivered_bytes += bytes.size();
                    status_.performance.disk_read_microseconds += reading_time;
                    return bytes.size();
                }
            }
            if (!waiting_since) { waiting_since = std::chrono::steady_clock::now(); ++status_.performance.cache_misses; }
            if (read_signal_->changed.wait_until(lock, deadline) == std::cv_status::timeout) {
                throw std::runtime_error("Timed out waiting for verified video bytes at offset " + std::to_string(offset));
            }
        }
    } catch (...) {
        if (!lock.owns_lock()) lock.lock();
        finish_wait();
        remove_request();
        throw;
    }
}

void TorrentVideoSource::accountProbeRange(std::int64_t offset, std::size_t length)
{
    auto begin = offset;
    auto end = offset + static_cast<std::int64_t>(length);
    auto first = probe_ranges_.lower_bound(begin);
    if (first != probe_ranges_.begin() && std::prev(first)->second >= begin) --first;
    auto last = first;
    std::uint64_t previous = 0;
    while (last != probe_ranges_.end() && last->first <= end) {
        begin = std::min(begin, last->first);
        end = std::max(end, last->second);
        previous += last->second - last->first;
        ++last;
    }
    const auto added = static_cast<std::uint64_t>(end - begin) - previous;
    if (added > probe_limit_ - std::min(probe_limit_, probe_bytes_)) {
        throw std::runtime_error("VLC startup/index probing exceeded the bounded unique-range allowance at offset "
            + std::to_string(offset) + "; refusing a full-file scan (" + std::to_string(probe_bytes_) + " unique bytes).");
    }
    probe_ranges_.erase(first, last);
    probe_ranges_.emplace(begin, end);
    probe_bytes_ += added;
}

void TorrentVideoSource::run()
{
    std::optional<VideoRange> applied_demand;
    std::int64_t applied_offset = -1;
    double applied_rate = requested_rate_;
    try {
        while (!stopping_.load()) {
            std::optional<VideoRange> demand;
            std::int64_t offset;
            double rate;
            PlaybackMovement movement;
            {
                std::lock_guard<std::mutex> lock(read_signal_->mutex);
                if (!pending_.empty() && pending_.front()->epoch == epoch_) demand = pending_.front()->range;
                offset = playback_offset_;
                rate = requested_rate_;
                movement = seek_requested_ ? PlaybackMovement::seek : PlaybackMovement::sequential;
                seek_requested_ = false;
            }
            if (movement == PlaybackMovement::seek || applied_offset != offset || applied_demand != demand) {
                cache_->updateReadRequest(offset, demand, movement);
                applied_offset = offset;
                applied_demand = demand;
            }
            if (applied_rate != rate) { cache_->setPlaybackRate(rate); applied_rate = rate; }
            cache_->pump(std::chrono::milliseconds{0});
            auto diagnostics = cache_->diagnostics();
            {
                std::unique_lock<std::mutex> lock(read_signal_->mutex);
                status_.cache = std::move(diagnostics);
                read_signal_->changed.notify_all();
                // Coalesce cached sequential movement at 20 ms, but let a new
                // blocked read or explicit seek wake the controller immediately.
                read_signal_->changed.wait_for(lock, std::chrono::milliseconds{20}, [&] {
                    const bool new_demand = !pending_.empty() && pending_.front()->epoch == epoch_
                        && applied_demand != pending_.front()->range;
                    return stopping_.load() || seek_requested_ || requested_rate_ != applied_rate || new_demand;
                });
            }
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(read_signal_->mutex);
        if (!stopping_.load()) status_.error = error.what();
    }
    read_signal_->changed.notify_all();
}

void TorrentVideoSource::cancelPendingReads()
{
    {
        std::lock_guard<std::mutex> lock(read_signal_->mutex);
        ++epoch_;
        suspended_ = true;
        pending_.clear();
    }
    read_signal_->changed.notify_all();
}

void TorrentVideoSource::resumeReads()
{
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    if (stopping_.load()) throw ReadCancelled{};
    suspended_ = false;
    ++epoch_;
}

void TorrentVideoSource::seek(std::int64_t offset)
{ seekToByte(offset, PlaybackMovement::seek); }

void TorrentVideoSource::seekForDemux(std::int64_t offset)
{ seekToByte(offset, PlaybackMovement::sequential); }

void TorrentVideoSource::seekToByte(std::int64_t offset, PlaybackMovement movement)
{
    if (offset < 0 || offset > video_size_) throw std::out_of_range("Video seek offset");
    {
        std::lock_guard<std::mutex> lock(read_signal_->mutex);
        checkReadState(epoch_);
        ++epoch_;
        pending_.clear();
        if (offset < video_size_ && (movement == PlaybackMovement::seek || !container_.isMetadata(offset))) {
            playback_offset_ = offset;
            if (movement == PlaybackMovement::seek || reposition_next_demux_seek_) {
                seek_requested_ = true;
                reposition_next_demux_seek_ = false;
            }
        }
    }
    read_signal_->changed.notify_all();
}

void TorrentVideoSource::setContainer(ContainerInfo container)
{
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    container_ = std::move(container);
}

void TorrentVideoSource::beginPlaybackEpoch()
{
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    probing_ = true;
    reposition_next_demux_seek_ = true;
    probe_bytes_ = 0;
    probe_ranges_.clear();
}

void TorrentVideoSource::markPlaybackActive()
{
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    probing_ = false;
}

void TorrentVideoSource::setEstimatedRate(double bytes_per_second)
{
    if (!std::isfinite(bytes_per_second) || bytes_per_second <= 0 || bytes_per_second > 1e12) return;
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    requested_rate_ = bytes_per_second;
}

void TorrentVideoSource::prebuffer()
{
    const auto targets = bufferTargets();
    double rate;
    { std::lock_guard<std::mutex> lock(read_signal_->mutex); rate = requested_rate_; }
    const auto target = std::min<std::int64_t>({video_size_, capacity_ / 3,
        std::max<std::int64_t>(16384, static_cast<std::int64_t>(rate * targets.high_seconds))});
    std::array<unsigned char, 16384> bytes{};
    for (std::int64_t offset = 0; offset < target;) {
        offset += readVerified(offset, bytes.data(), std::min<std::int64_t>(bytes.size(), target - offset), ReadPurpose::prebuffer);
    }
}

BufferWatermarks TorrentVideoSource::bufferTargets() const
{
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    return bufferTargetsLocked();
}

BufferWatermarks TorrentVideoSource::bufferTargetsLocked() const
{
    const auto remaining = std::max<std::int64_t>(0, video_size_ - status_.cache.playback_position);
    const auto forward_capacity = std::min({remaining, capacity_ / 3,
        std::max<std::int64_t>(0, status_.cache.desired.end_byte - status_.cache.playback_position)});
    return bufferWatermarks(requested_rate_, status_.cache.network_bytes_per_second, forward_capacity);
}

VideoSourceStatus TorrentVideoSource::status() const
{
    std::lock_guard<std::mutex> lock(read_signal_->mutex);
    auto status = status_;
    status.waiting_reads = pending_.size();
    status.stopped = stopping_.load();
    status.buffer_targets = bufferTargetsLocked();
    return status;
}

void TorrentVideoSource::stop()
{
    std::lock_guard<std::mutex> stopping(stop_mutex_);
    stopping_.store(true);
    read_signal_->notify();
    if (worker_.joinable()) worker_.join();
    std::unique_ptr<TorrentCacheSession> closing;
    {
        std::lock_guard<std::mutex> lock(read_signal_->mutex);
        closing = std::move(cache_);
    }
    // The network thread can still publish verification notifications while it
    // shuts down. Do not hold the reader mutex across libtorrent's join.
    closing.reset();
}

} // namespace intezam
