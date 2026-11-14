#pragma once

#include <cstdint>
#include <vector>

namespace intezam {

enum class PlaybackMovement { sequential, seek };

struct VideoRange {
    std::int64_t offset;
    std::int64_t length;
    bool operator==(const VideoRange& other) const { return offset == other.offset && length == other.length; }
    bool operator!=(const VideoRange& other) const { return !(*this == other); }
};

struct PieceGeometry {
    std::int64_t video_size;
    std::int64_t video_offset;
    std::int64_t torrent_size;
    int piece_length;

    void validate() const;
    int pieceCount() const;
    int pieceSize(int piece) const;
    std::vector<int> piecesForRange(std::int64_t offset, std::int64_t length) const;
};

} // namespace intezam
