#include "container_probe.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

class SparseSource final : public intezam::VideoByteSource {
public:
    explicit SparseSource(std::int64_t length) : length_(length) {}
    std::int64_t size() const override { return length_; }
    std::size_t read(std::int64_t offset, void* buffer, std::size_t length) override
    {
        if (offset < 0 || offset > length_) throw std::out_of_range("read");
        length = std::min<std::size_t>({length, 113, static_cast<std::size_t>(length_ - offset)});
        auto* bytes = static_cast<unsigned char*>(buffer);
        for (std::size_t index = 0; index < length; ++index) bytes[index] = fragments[offset + index];
        delivered += length;
        return length;
    }
    void put(std::int64_t offset, const std::vector<unsigned char>& bytes)
    { for (auto byte : bytes) fragments[offset++] = byte; }
    void seek(std::int64_t offset) override
    { if (offset < 0 || offset > length_) throw std::out_of_range("seek"); }
    void cancelPendingReads() override {}
    void stop() override {}
    std::map<std::int64_t, unsigned char> fragments;
    std::size_t delivered = 0;
private:
    std::int64_t length_;
};

std::vector<unsigned char> atom(std::uint32_t size, const char* type)
{
    return {static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
            static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size),
            static_cast<unsigned char>(type[0]), static_cast<unsigned char>(type[1]),
            static_cast<unsigned char>(type[2]), static_cast<unsigned char>(type[3])};
}
} // namespace

int main()
{
    try {
        constexpr std::uint32_t length = 64 * 1024 * 1024;
        SparseSource tail(length);
        tail.put(0, atom(24, "ftyp"));
        tail.put(24, atom(length - 1048, "mdat"));
        tail.put(length - 1024, atom(1024, "moov"));
        const auto info = intezam::probeContainer(tail);
        require(info.type == intezam::ContainerType::mp4, "probe MP4 by bytes");
        require(info.moov_at_end, "locate tail moov without reading mdat");
        require(info.isMetadata(length - 1000), "moov range classified as metadata");
        require(!info.isMetadata(length / 2), "mdat payload not metadata");
        require(tail.delivered < 8192, "bounded atom header probing");

        SparseSource front(4096);
        front.put(0, atom(24, "ftyp"));
        front.put(24, atom(128, "moov"));
        front.put(152, atom(3944, "mdat"));
        require(!intezam::probeContainer(front).moov_at_end, "front moov");
        SparseSource duration_mp4(4096);
        duration_mp4.put(0, atom(24, "ftyp"));
        duration_mp4.put(24, atom(36, "moov"));
        duration_mp4.put(32, atom(28, "mvhd"));
        std::vector<unsigned char> movie_header(20, 0);
        movie_header[14] = 3; movie_header[15] = 232; // 1000 ticks/second
        movie_header[17] = 1; movie_header[18] = 0xd4; movie_header[19] = 0xc0; // 120000 ticks
        duration_mp4.put(40, movie_header);
        const auto duration_info = intezam::probeContainer(duration_mp4);
        require(duration_info.duration_seconds && *duration_info.duration_seconds == 120,
            "read MP4 movie duration before VLC startup");
        require(intezam::initialVideoRate(24 * 1024 * 1024, duration_info, 1048576) == 24 * 1024 * 1024 / 120.0,
            "estimate startup consumption from file size and duration");
        require(intezam::initialVideoRate(1000000, {}, 1048576) == 1048576,
            "unknown duration preserves the configured fallback");
        movie_header[14] = movie_header[15] = 0;
        duration_mp4.put(40, movie_header);
        require(!intezam::probeContainer(duration_mp4).duration_seconds, "zero MP4 timescale is not a bitrate estimate");
        duration_mp4.put(24, atom(48, "moov"));
        duration_mp4.put(32, atom(40, "mvhd"));
        movie_header.assign(32, 0);
        movie_header[0] = 1;
        movie_header[22] = 3; movie_header[23] = 232;
        movie_header[27] = 1; // 2^32 ticks, requiring the version-1 64-bit field
        duration_mp4.put(40, movie_header);
        require(*intezam::probeContainer(duration_mp4).duration_seconds == 4294967296.0 / 1000,
            "read the 64-bit version-1 movie duration");
        require(intezam::initialVideoRate(1000000, intezam::probeContainer(duration_mp4), 1048576) == 1048576,
            "implausible duration keeps the safe startup fallback");
        std::fill(movie_header.begin() + 24, movie_header.end(), 255);
        duration_mp4.put(40, movie_header);
        require(!intezam::probeContainer(duration_mp4).duration_seconds, "unknown 64-bit duration is not interpreted as playback time");
        SparseSource fragmented(length);
        fragmented.put(0, atom(24, "ftyp"));
        fragmented.put(24, atom(128, "moov"));
        for (int box = 0; box < 5000; ++box) {
            fragmented.put(152 + box * 16, atom(8, "moof"));
            fragmented.put(160 + box * 16, atom(8, "mdat"));
        }
        require(!intezam::probeContainer(fragmented).moov_at_end && fragmented.delivered < 4200,
                "stop at front moov instead of scanning every media fragment");
        SparseSource invalid(100);
        invalid.put(0, atom(24, "ftyp"));
        invalid.put(24, atom(4, "moov"));
        bool rejected = false;
        try { intezam::probeContainer(invalid); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "malformed atom bounds rejected");

        for (const auto& doc : {std::string("webm"), std::string("matroska")}) {
            SparseSource ebml(4096);
            std::vector<unsigned char> header{0x1a, 0x45, 0xdf, 0xa3,
                static_cast<unsigned char>(0x80 + 3 + doc.size()), 0x42, 0x82,
                static_cast<unsigned char>(0x80 + doc.size())};
            header.insert(header.end(), doc.begin(), doc.end());
            ebml.put(0, header);
            const auto kind = intezam::probeContainer(ebml).type;
            require(kind == (doc == "webm" ? intezam::ContainerType::webm : intezam::ContainerType::matroska),
                    "EBML DocType detection");
        }
        SparseSource timed_ebml(4096);
        std::vector<unsigned char> timed_header{0x1a,0x45,0xdf,0xa3,0x87,0x42,0x82,0x84,'w','e','b','m',
            0x18,0x53,0x80,0x67,0xff, // unknown-length Segment
            0x15,0x49,0xa9,0x66,0x92, // Info, 18 bytes
            0x2a,0xd7,0xb1,0x83,0x0f,0x42,0x40, // 1000000 ns timescale
            0x44,0x89,0x88};
        double ticks = 120000;
        std::uint64_t bits;
        std::memcpy(&bits, &ticks, sizeof(bits));
        for (int shift = 56; shift >= 0; shift -= 8) timed_header.push_back(static_cast<unsigned char>(bits >> shift));
        timed_ebml.put(0, timed_header);
        const auto timed_info = intezam::probeContainer(timed_ebml);
        require(timed_info.duration_seconds && std::abs(*timed_info.duration_seconds - 120) < 0.001,
            "read WebM/Matroska duration from bounded Segment Info");
        require(timed_ebml.delivered < 8192, "duration probing does not scan media clusters");
        SparseSource float_ebml(4096);
        timed_header.resize(17);
        const std::vector<unsigned char> float_info{0x15,0x49,0xa9,0x66,0x8e,
            0x2a,0xd7,0xb1,0x83,0x0f,0x42,0x40,0x44,0x89,0x84,0x47,0xea,0x60,0x00};
        timed_header.insert(timed_header.end(), float_info.begin(), float_info.end());
        float_ebml.put(0, timed_header);
        require(*intezam::probeContainer(float_ebml).duration_seconds == 120, "32-bit Matroska duration is supported");
        SparseSource cluster_first(1024 * 1024);
        timed_header.resize(17);
        timed_header.insert(timed_header.end(), {0x1f,0x43,0xb6,0x75,0xff});
        cluster_first.put(0, timed_header);
        require(!intezam::probeContainer(cluster_first).duration_seconds && cluster_first.delivered < 8192,
            "duration fallback stops before media clusters");
        SparseSource avi(4096);
        avi.put(0, {'R','I','F','F',0,0,0,0,'A','V','I',' '});
        require(intezam::probeContainer(avi).type == intezam::ContainerType::avi, "RIFF AVI");
        SparseSource ts(4096);
        ts.put(0, {0x47}); ts.put(188, {0x47}); ts.put(376, {0x47});
        require(intezam::probeContainer(ts).type == intezam::ContainerType::mpeg_ts, "TS sync probe");
        std::cout << "Container probes passed, including bounded front/tail moov reads.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
