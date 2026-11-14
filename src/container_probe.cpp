#include "container_probe.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace intezam {
namespace {

std::vector<unsigned char> readHeader(VideoByteSource& source, std::int64_t offset, std::size_t length)
{
    length = std::min<std::uint64_t>(length, source.size() - offset);
    std::vector<unsigned char> bytes(length);
    std::size_t read = 0;
    while (read < length) {
        const auto count = source.readMetadata(offset + read, bytes.data() + read, length - read);
        if (count == 0) throw std::runtime_error("Unexpected EOF in container header.");
        read += count;
    }
    return bytes;
}

std::uint64_t bigEndian(const unsigned char* bytes, std::size_t count)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < count; ++index) value = (value << 8) | bytes[index];
    return value;
}

std::optional<double> mp4Duration(VideoByteSource& source, std::int64_t begin, std::int64_t end)
{
    const auto prefix_end = begin + std::min<std::int64_t>(65536, end - begin);
    for (int children = 0; begin <= end - 8 && begin < prefix_end && children < 128; ++children) {
        const auto header = readHeader(source, begin, 8);
        if (header.size() != 8) return {};
        auto length = bigEndian(header.data(), 4);
        std::int64_t header_length = 8;
        if (length == 1) {
            if (end - begin < 16) return {};
            const auto extended = readHeader(source, begin + 8, 8);
            if (extended.size() != 8) return {};
            length = bigEndian(extended.data(), 8);
            header_length = 16;
        }
        if (length < static_cast<std::uint64_t>(header_length) || length > static_cast<std::uint64_t>(end - begin)) return {};
        if (std::memcmp(header.data() + 4, "mvhd", 4) == 0) {
            const auto body = readHeader(source, begin + header_length, std::min<std::uint64_t>(32, length - header_length));
            if (body.empty() || body[0] > 1) return {};
            const bool version_one = body[0] == 1;
            if (body.size() < (version_one ? 32u : 20u)) return {};
            const auto scale = bigEndian(body.data() + (version_one ? 20 : 12), 4);
            const auto ticks = bigEndian(body.data() + (version_one ? 24 : 16), version_one ? 8 : 4);
            if (scale == 0 || ticks == 0 || ticks == (version_one ? UINT64_MAX : UINT32_MAX)) return {};
            return double(ticks) / scale;
        }
        begin += static_cast<std::int64_t>(length);
    }
    return {};
}

ContainerInfo probeMp4(VideoByteSource& source)
{
    ContainerInfo info;
    info.type = ContainerType::mp4;
    info.name = "ISO BMFF (MP4/MOV/M4V)";
    std::int64_t position = 0;
    bool seen_payload = false;
    for (int boxes = 0; position < source.size(); ++boxes) {
        if (boxes == 4096) throw std::runtime_error("Container atom probe exceeded its bounded header limit.");
        auto header = readHeader(source, position, 8);
        if (header.size() != 8) throw std::runtime_error("Truncated ISO BMFF atom header.");
        std::uint64_t size = bigEndian(header.data(), 4);
        std::uint64_t header_size = 8;
        if (size == 1) {
            const auto extended = readHeader(source, position + 8, 8);
            if (extended.size() != 8) throw std::runtime_error("Truncated extended atom length.");
            size = bigEndian(extended.data(), 8);
            header_size = 16;
        } else if (size == 0) size = source.size() - position;
        if (size < header_size || size > static_cast<std::uint64_t>(source.size() - position)) {
            throw std::runtime_error("ISO BMFF atom length exceeds container bounds.");
        }
        const std::string type(reinterpret_cast<const char*>(header.data() + 4), 4);
        if (type == "moov") {
            info.moov_at_end = seen_payload;
            info.metadata_ranges.push_back({position, static_cast<std::int64_t>(size)});
            info.duration_seconds = mp4Duration(source, position + header_size, position + size);
            // Once startup metadata is located, leave media fragments to VLC.
            // Walking every moof/mdat pair could fetch the whole torrent merely
            // to read headers, despite each individual request being small.
            return info;
        } else if (type == "ftyp" || type == "sidx" || type == "mfra") {
            info.metadata_ranges.push_back({position, static_cast<std::int64_t>(size)});
        } else if (type == "mdat") {
            seen_payload = true;
        }
        position += static_cast<std::int64_t>(size);
    }
    return info;
}

std::pair<std::uint64_t, std::size_t> ebmlInteger(
    const std::vector<unsigned char>& bytes, std::size_t offset, bool strip_marker)
{
    if (offset >= bytes.size() || bytes[offset] == 0) return {0, 0};
    unsigned char marker = 0x80;
    std::size_t width = 1;
    while (!(bytes[offset] & marker)) { marker >>= 1; ++width; }
    if (width > 8 || width > bytes.size() - offset) return {0, 0};
    auto value = bigEndian(bytes.data() + offset, width);
    if (strip_marker) value &= ~(std::uint64_t{marker} << (8 * (width - 1)));
    return {value, width};
}

std::optional<double> ebmlInfoDuration(const std::vector<unsigned char>& bytes)
{
    double scale = 1000000; // Matroska's default nanoseconds per timecode tick
    double ticks = 0;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto id = ebmlInteger(bytes, offset, false);
        if (!id.second) return {};
        const auto size = ebmlInteger(bytes, offset + id.second, true);
        if (!size.second) return {};
        offset += id.second + size.second;
        if (size.first > bytes.size() - offset) return {};
        if (id.first == 0x2ad7b1 && size.first > 0 && size.first <= 8)
            scale = static_cast<double>(bigEndian(bytes.data() + offset, size.first));
        if (id.first == 0x4489 && (size.first == 4 || size.first == 8)) {
            if (size.first == 4) {
                const auto bits = static_cast<std::uint32_t>(bigEndian(bytes.data() + offset, 4));
                float value;
                std::memcpy(&value, &bits, 4);
                ticks = value;
            } else {
                const auto bits = bigEndian(bytes.data() + offset, 8);
                std::memcpy(&ticks, &bits, 8);
            }
        }
        offset += static_cast<std::size_t>(size.first);
    }
    const auto seconds = ticks * scale / 1e9;
    if (scale <= 0 || !std::isfinite(seconds) || seconds <= 0) return {};
    return seconds;
}

void probeEbmlInfo(VideoByteSource& source, std::int64_t offset, ContainerInfo& info)
{
    if (offset >= source.size()) return;
    auto header = readHeader(source, offset, 12);
    const auto segment = ebmlInteger(header, 0, false);
    const auto segment_size = ebmlInteger(header, segment.second, true);
    if (segment.first != 0x18538067 || !segment_size.second) return;
    offset += segment.second + segment_size.second;
    const auto unknown = (std::uint64_t{1} << (7 * segment_size.second)) - 1;
    if (segment_size.first != unknown && segment_size.first > static_cast<std::uint64_t>(source.size() - offset)) return;
    const auto end = segment_size.first == unknown ? source.size() : offset + static_cast<std::int64_t>(segment_size.first);
    const auto prefix_end = offset + std::min<std::int64_t>(65536, end - offset);
    for (int elements = 0; offset < end && offset < prefix_end && elements < 64; ++elements) {
        header = readHeader(source, offset, static_cast<std::size_t>(std::min<std::int64_t>(12, end - offset)));
        const auto id = ebmlInteger(header, 0, false);
        const auto size = ebmlInteger(header, id.second, true);
        if (!id.second || !size.second || id.first == 0x1f43b675) return; // never scan media clusters
        const auto body = offset + static_cast<std::int64_t>(id.second + size.second);
        if (body > end || size.first > static_cast<std::uint64_t>(end - body)) return;
        if (id.first == 0x1549a966) {
            if (size.first > 65536) return;
            info.duration_seconds = ebmlInfoDuration(readHeader(source, body, size.first));
            info.metadata_ranges.push_back({offset, static_cast<std::int64_t>(size.first + id.second + size.second)});
            return;
        }
        offset = body + static_cast<std::int64_t>(size.first);
    }
}

ContainerInfo probeEbml(VideoByteSource& source, const std::vector<unsigned char>& header)
{
    ContainerInfo info;
    info.type = ContainerType::matroska;
    info.name = "Matroska/EBML";
    const auto root = ebmlInteger(header, 4, true);
    if (root.second == 0) throw std::runtime_error("Malformed EBML header size.");
    std::size_t offset = 4 + root.second;
    const auto end = std::min<std::uint64_t>(header.size(), root.first + offset);
    info.metadata_ranges.push_back({0, static_cast<std::int64_t>(end)});
    while (offset < end) {
        const auto id = ebmlInteger(header, offset, false);
        if (id.second == 0 || id.second > 4) break;
        offset += id.second;
        const auto size = ebmlInteger(header, offset, true);
        if (size.second == 0) break;
        offset += size.second;
        if (offset > end || size.first > end - offset) break;
        if (id.first == 0x4282) {
            const std::string doc(reinterpret_cast<const char*>(header.data() + offset), size.first);
            if (doc == "webm") { info.type = ContainerType::webm; info.name = "WebM"; }
            else if (doc == "matroska") info.name = "Matroska";
            break;
        }
        offset += size.first;
    }
    if (root.first + 4 + root.second <= header.size()) probeEbmlInfo(source, end, info);
    return info;
}

} // namespace

bool ContainerInfo::isMetadata(std::int64_t offset) const
{
    return std::any_of(metadata_ranges.begin(), metadata_ranges.end(), [offset](const VideoRange& range) {
        return offset >= range.offset && offset - range.offset < range.length;
    });
}

ContainerInfo probeContainer(VideoByteSource& source)
{
    const auto header = readHeader(source, 0, 4096);
    if (header.size() >= 8) {
        const std::string atom(reinterpret_cast<const char*>(header.data() + 4), 4);
        if (atom == "ftyp" || atom == "moov" || atom == "wide" || atom == "mdat" || atom == "free") return probeMp4(source);
    }
    if (header.size() >= 5 && bigEndian(header.data(), 4) == 0x1a45dfa3) return probeEbml(source, header);
    ContainerInfo info;
    if (header.size() >= 12 && std::memcmp(header.data(), "RIFF", 4) == 0
        && std::memcmp(header.data() + 8, "AVI ", 4) == 0) {
        info.type = ContainerType::avi; info.name = "AVI";
    } else if (header.size() >= 389 && ((header[0] == 0x47 && header[188] == 0x47 && header[376] == 0x47)
        || (header[4] == 0x47 && header[196] == 0x47 && header[388] == 0x47))) {
        info.type = ContainerType::mpeg_ts; info.name = "MPEG-TS";
    } else if (header.size() >= 4 && bigEndian(header.data(), 4) == 0x000001ba) {
        info.type = ContainerType::mpeg_ps; info.name = "MPEG Program Stream";
    }
    return info;
}

double initialVideoRate(std::int64_t file_bytes, const ContainerInfo& container, double fallback)
{
    if (!container.duration_seconds || !std::isfinite(*container.duration_seconds)
        || *container.duration_seconds < 0.1 || *container.duration_seconds > 30 * 24 * 3600) return fallback;
    const auto rate = file_bytes / *container.duration_seconds;
    return std::isfinite(rate) && rate > 0 && rate <= 1e12 ? rate : fallback;
}

} // namespace intezam
