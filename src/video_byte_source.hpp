#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace intezam {

class ReadCancelled final : public std::runtime_error {
public:
    ReadCancelled() : std::runtime_error("Video read cancelled") {}
};

// Reads can be short. Zero means EOF only; missing data must wait or throw.
class VideoByteSource {
public:
    virtual ~VideoByteSource() = default;
    virtual std::int64_t size() const = 0;
    virtual std::size_t read(std::int64_t offset, void* buffer, std::size_t length) = 0;
    virtual std::size_t readMetadata(std::int64_t offset, void* buffer, std::size_t length)
    { return read(offset, buffer, length); }
    virtual void seek(std::int64_t offset) = 0;
    // Container-internal repositioning need not force a new rolling window.
    virtual void seekForDemux(std::int64_t offset) { seek(offset); }
    virtual void cancelPendingReads() = 0;
    virtual void resumeReads() {}
    virtual void stop() = 0;
};

} // namespace intezam
