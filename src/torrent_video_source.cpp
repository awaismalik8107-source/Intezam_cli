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
{ return readVerified(offset, buffer, length, false); }

std::size_t TorrentVideoSource::readMetadata(std::int64_t offset, void* buffer, std::size_t length)
{ return readVerified(offset, buffer, length, true); }

std::size_t TorrentVideoSource::readVerified(std::int64_t offset, void* buffer, std::size_t length, bool metadata)
{
    if (offset < 0 || offset > video_size_) throw std::out_of_range("Video byte offset");
    if (length == 0 || offset == video_size_) return 0;
    if (!buffer) throw std::invalid_argument("Video read buffer is null");
    const auto until_boundary = piece_length_ - (torrent_offset_ + offset) % piece_length_;
    length = std::min<std::uint64_t>({length, 16384, static_cast<std::uint64_t>(video_size_ - offset),
                                    static_cast<std::uint64_t>(until_boundary)});
    std::unique_lock<std::mutex> lock(mutex_);
    const auto epoch = epoch_;
    checkReadState(epoch);
    if (probing_) {
        // A tiny header read still requires a complete verified torrent piece.
        // Charge that selected-file footprint so sparse index scans cannot hide
        // full-file downloads behind small callback requests.
        const auto begin = std::max<std::int64_t>(0, offset - (torrent_offset_ + offset) % piece_length_);
        const auto end = std::min(video_size_, offset + until_boundary);
        accountProbeRange(begin, static_cast<std::size_t>(end - begin));
    }
    if (!metadata && !container_.isMetadata(offset)) playback_offset_ = offset;
    auto request = std::make_shared<PendingRead>(PendingRead{{offset, static_cast<std::int64_t>(length)}, epoch});
    pending_.push_back(request);
    changed_.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + read_timeout_;
    const auto remove_request = [&] {
        const auto found = std::find(pending_.begin(), pending_.end(), request);
        if (found != pending_.end()) pending_.erase(found);
        changed_.notify_all();
    };
    try {
        for (;;) {
            checkReadState(epoch);
            if (!pending_.empty() && pending_.front() == request) {
                if (auto lease = cache_->read(offset, length)) {
                    remove_request();
                    lock.unlock();
                    const auto bytes = lease->read();
                    std::memcpy(buffer, bytes.data(), bytes.size());
                    lock.lock();
                    status_.delivered_bytes += bytes.size();
                    return bytes.size();
                }
            }
            if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
                throw std::runtime_error("Timed out waiting for verified video bytes at offset " + std::to_string(offset));
            }
        }
    } catch (...) {
        if (!lock.owns_lock()) lock.lock();
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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pending_.empty() && pending_.front()->epoch == epoch_) demand = pending_.front()->range;
                offset = playback_offset_;
                rate = requested_rate_;
            }
            if (applied_offset != offset || applied_demand != demand) {
                cache_->updateReadRequest(offset, demand);
                applied_offset = offset;
                applied_demand = demand;
            }
            if (applied_rate != rate) { cache_->setPlaybackRate(rate); applied_rate = rate; }
            cache_->pump(std::chrono::milliseconds{20});
            auto diagnostics = cache_->diagnostics();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.cache = std::move(diagnostics);
            }
            changed_.notify_all();
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_.load()) status_.error = error.what();
    }
    changed_.notify_all();
}

void TorrentVideoSource::cancelPendingReads()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++epoch_;
        suspended_ = true;
        pending_.clear();
    }
    changed_.notify_all();
}

void TorrentVideoSource::resumeReads()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load()) throw ReadCancelled{};
    suspended_ = false;
    ++epoch_;
}

void TorrentVideoSource::seek(std::int64_t offset)
{
    if (offset < 0 || offset > video_size_) throw std::out_of_range("Video seek offset");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        checkReadState(epoch_);
        ++epoch_;
        pending_.clear();
        if (offset < video_size_ && !container_.isMetadata(offset)) playback_offset_ = offset;
    }
    changed_.notify_all();
}

void TorrentVideoSource::setContainer(ContainerInfo container)
{
    std::lock_guard<std::mutex> lock(mutex_);
    container_ = std::move(container);
}

void TorrentVideoSource::beginPlaybackEpoch()
{
    std::lock_guard<std::mutex> lock(mutex_);
    probing_ = true;
    probe_bytes_ = 0;
    probe_ranges_.clear();
}

void TorrentVideoSource::markPlaybackActive()
{
    std::lock_guard<std::mutex> lock(mutex_);
    probing_ = false;
}

void TorrentVideoSource::setEstimatedRate(double bytes_per_second)
{
    if (!std::isfinite(bytes_per_second) || bytes_per_second <= 0 || bytes_per_second > 1e12) return;
    std::lock_guard<std::mutex> lock(mutex_);
    requested_rate_ = bytes_per_second;
}

void TorrentVideoSource::prebuffer()
{
    double rate;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rate = requested_rate_;
    }
    const auto target = std::min<std::int64_t>({video_size_, capacity_ / 3,
        std::max<std::int64_t>(16384, static_cast<std::int64_t>(rate * 0.2))});
    std::array<unsigned char, 16384> bytes{};
    for (std::int64_t offset = 0; offset < target;) {
        offset += readMetadata(offset, bytes.data(), std::min<std::int64_t>(bytes.size(), target - offset));
    }
}

VideoSourceStatus TorrentVideoSource::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = status_;
    status.waiting_reads = pending_.size();
    status.stopped = stopping_.load();
    return status;
}

void TorrentVideoSource::stop()
{
    std::lock_guard<std::mutex> stopping(stop_mutex_);
    stopping_.store(true);
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.reset();
}

} // namespace intezam
