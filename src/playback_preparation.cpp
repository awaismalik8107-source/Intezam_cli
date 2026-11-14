#include "playback_preparation.hpp"
#include <algorithm>
#include <set>

namespace intezam {
namespace {
struct HeaderPrefetchLimit {};

class BoundedHeaderSource final : public VideoByteSource {
public:
    BoundedHeaderSource(TorrentVideoSource& source, const VideoFile& video, int piece_length)
        : source_(source), video_offset_(video.torrent_offset), piece_length_(piece_length),
          limit_(std::max(1, std::min(4, 8 * 1024 * 1024 / piece_length))) {}
    std::int64_t size() const override { return source_.size(); }
    std::size_t read(std::int64_t offset, void* buffer, std::size_t length) override
    { return readMetadata(offset, buffer, length); }
    std::size_t readMetadata(std::int64_t offset, void* buffer, std::size_t length) override
    {
        if (offset == size() || length == 0) return 0;
        if (offset < 0 || offset > size()) throw std::out_of_range("Header prefetch offset");
        const int piece = static_cast<int>((video_offset_ + offset) / piece_length_);
        if (!pieces_.count(piece) && pieces_.size() >= limit_) throw HeaderPrefetchLimit{};
        pieces_.insert(piece);
        // Charge each whole piece before requesting any part of it, including
        // an unaligned first/last piece. Transport duplicates do not expand this set.
        const auto boundary = piece_length_ - (video_offset_ + offset) % piece_length_;
        return source_.readMetadata(offset, buffer, std::min<std::uint64_t>(length, boundary));
    }
    void seek(std::int64_t offset) override { source_.seek(offset); }
    void cancelPendingReads() override { source_.cancelPendingReads(); }
    void stop() override { source_.stop(); }
private:
    TorrentVideoSource& source_;
    std::int64_t video_offset_;
    int piece_length_;
    std::size_t limit_;
    std::set<int> pieces_;
};
}

PlaybackPreparation::PlaybackPreparation(std::string magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata, VideoFile video, CacheStreamOptions options)
    : video_(std::move(video))
{
    worker_ = std::thread([this, magnet = std::move(magnet), metadata = std::move(metadata), options = std::move(options)] {
        run(magnet, metadata, options);
    });
}

PlaybackPreparation::~PlaybackPreparation()
{
    cancel();
    worker_.join();
    if (auto source = snapshot().source) source->stop();
}

bool PlaybackPreparation::matches(const VideoFile& video) const
{
    return video.torrent_index == video_.torrent_index && video.torrent_offset == video_.torrent_offset
        && video.size_bytes == video_.size_bytes && video.path == video_.path;
}

PreparationSnapshot PlaybackPreparation::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void PlaybackPreparation::cancel()
{
    cancelled_->store(true);
    if (auto source = snapshot().source) source->cancelPendingReads();
    changed_.notify_all();
}

std::shared_ptr<TorrentVideoSource> PlaybackPreparation::waitForSource()
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return cancelled_->load() || snapshot_.source || snapshot_.finished; });
    if (cancelled_->load()) throw ReadCancelled{};
    if (!snapshot_.error.empty()) throw std::runtime_error(snapshot_.error);
    return snapshot_.source;
}

std::optional<ContainerInfo> PlaybackPreparation::waitForHeaders()
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return cancelled_->load() || snapshot_.finished; });
    if (cancelled_->load()) throw ReadCancelled{};
    if (!snapshot_.error.empty()) throw std::runtime_error(snapshot_.error);
    return snapshot_.container;
}

void PlaybackPreparation::run(const std::string& magnet,
    std::shared_ptr<const libtorrent::torrent_info> metadata, const CacheStreamOptions& options)
{
    try {
        auto configuration = options;
        configuration.header_read_mode = true;
        configuration.cancelled = [cancelled = cancelled_, external = options.cancelled] {
            return cancelled->load() || (external && external());
        };
        auto source = std::make_shared<TorrentVideoSource>(magnet, metadata, video_, configuration);
        { std::lock_guard<std::mutex> lock(mutex_); snapshot_.source = source; }
        changed_.notify_all();
        if (cancelled_->load()) { source->cancelPendingReads(); throw ReadCancelled{}; }
        BoundedHeaderSource headers(*source, video_, metadata->piece_length());
        auto container = probeContainer(headers);
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.container = std::move(container);
    } catch (const HeaderPrefetchLimit&) {
        // Selection can finish probing against the same cached source. A quota
        // boundary is normal and must not become a playback error.
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled_->load()) snapshot_.error = error.what();
    }
    { std::lock_guard<std::mutex> lock(mutex_); snapshot_.finished = true; }
    changed_.notify_all();
}

std::optional<VideoFile> headerPrefetchCandidate(const std::vector<VideoFile>& videos,
    int piece_length, CacheBudgetOptions budget)
{
    std::optional<VideoFile> candidate;
    for (const auto& video : videos) {
        try { planCacheBudget(video.size_bytes, piece_length, 4096, 8192, budget); }
        catch (const std::runtime_error&) { continue; }
        if (!candidate || video.size_bytes > candidate->size_bytes) candidate = video;
    }
    return candidate;
}
} // namespace intezam
