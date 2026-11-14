#pragma once

#include "local_video_torrent.hpp"

namespace test_support {

// Seeder input only. Client storage is always the separate managed slot cache.
class SyntheticMedia {
public:
    explicit SyntheticMedia(int size_mebibytes = 32)
    {
        auto pattern = (fs::current_path() / "intezam-benchmark-XXXXXX").string();
        require(mkdtemp(pattern.data()) != nullptr, "benchmark directory");
        directory_ = pattern;
        std::ofstream file(path(), std::ios::binary);
        std::vector<char> block(1024 * 1024);
        for (std::size_t index = 0; index < block.size(); ++index) block[index] = static_cast<char>(index % 251);
        for (int count = 0; count < size_mebibytes; ++count) file.write(block.data(), block.size());
        if (!file.good()) {
            file.close();
            fs::remove_all(directory_);
            throw std::runtime_error("write synthetic seeder input");
        }
    }
    ~SyntheticMedia() { std::error_code error; fs::remove_all(directory_, error); }
    fs::path path() const { return directory_ / "fixture.mkv"; }
private:
    fs::path directory_;
};

} // namespace test_support
