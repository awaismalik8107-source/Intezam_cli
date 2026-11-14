#include "container_probe.hpp"

#include <algorithm>
#include <cstring>
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
        SparseSource avi(4096);
        avi.put(0, {'R','I','F','F',0,0,0,0,'A','V','I',' '});
        require(intezam::probeContainer(avi).type == intezam::ContainerType::avi, "RIFF AVI");
        SparseSource ts(4096);
        ts.put(0, {0x47}); ts.put(188, {0x47}); ts.put(376, {0x47});
        require(intezam::probeContainer(ts).type == intezam::ContainerType::mpeg_ts, "TS sync probe");
        std::cout << "Container probes passed, including bounded front/tail moov reads.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
