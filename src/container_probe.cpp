#include "container_probe.hpp"

#include <algorithm>
#include <array>
#include <cstring>
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

ContainerInfo probeEbml(const std::vector<unsigned char>& header)
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
    if (header.size() >= 5 && bigEndian(header.data(), 4) == 0x1a45dfa3) return probeEbml(header);
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

} // namespace intezam
