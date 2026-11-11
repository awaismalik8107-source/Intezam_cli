#include "cache_geometry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace intezam {

void PieceGeometry::validate() const
{
    if (video_size <= 0 || video_offset < 0 || torrent_size <= 0 || piece_length < 16384
        || (piece_length & (piece_length - 1)) != 0
        || video_offset > torrent_size || video_size > torrent_size - video_offset
        || (torrent_size - 1) / piece_length >= std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Invalid selected-file / torrent piece geometry.");
    }
}

int PieceGeometry::pieceCount() const
{
    return static_cast<int>((torrent_size - 1) / piece_length + 1);
}

int PieceGeometry::pieceSize(int piece) const
{
    if (piece < 0 || piece >= pieceCount()) throw std::out_of_range("Torrent piece index");
    return static_cast<int>(std::min<std::int64_t>(piece_length,
        torrent_size - std::int64_t{piece} * piece_length));
}

std::vector<int> PieceGeometry::piecesForRange(std::int64_t offset, std::int64_t length) const
{
    if (offset < 0 || length < 0 || offset > video_size || length > video_size - offset) {
        throw std::out_of_range("Playback byte range exceeds the selected video.");
    }
    std::vector<int> pieces;
    if (length == 0) return pieces;
    const int first = static_cast<int>((video_offset + offset) / piece_length);
    const int last = static_cast<int>((video_offset + offset + length - 1) / piece_length);
    pieces.reserve(last - first + 1);
    for (int piece = first; piece <= last; ++piece) pieces.push_back(piece);
    return pieces;
}

} // namespace intezam
