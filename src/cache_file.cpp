#include "cache_file.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <linux/magic.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace intezam {
namespace {

struct ScopedDescriptor {
    int value;
    ~ScopedDescriptor() { if (value >= 0) close(value); }
};

[[noreturn]] void systemFailure(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

std::int64_t allocated(const fs::path& path)
{
    struct stat status{};
    if (lstat(path.c_str(), &status) != 0) systemFailure("stat cache allocation");
    return status.st_blocks * std::int64_t{512};
}

bool ownedDirectory(const fs::path& path)
{
    struct stat status{};
    return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == getuid();
}

void removeSessionFiles(const fs::path& directory) noexcept
{
    unlink((directory / "stream.cache").c_str());
    unlink((directory / "session.lock").c_str());
    rmdir(directory.c_str());
}

} // namespace

fs::path CacheFile::defaultRoot()
{
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && fs::path{cache}.is_absolute()) {
        return fs::path{cache} / "intezam";
    }
    const char* home_directory = std::getenv("HOME");
    if (!home_directory || !fs::path{home_directory}.is_absolute()) {
        throw std::runtime_error("Set HOME or an absolute XDG_CACHE_HOME for the temporary cache.");
    }
    return fs::path{home_directory} / ".cache/intezam";
}

void CacheFile::recoverStale(const fs::path& root)
{
    if (!ownedDirectory(root)) return;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto directory = entry.path();
        if (directory.filename().string().rfind("session-", 0) != 0 || !ownedDirectory(directory)) continue;
        ScopedDescriptor lock{open((directory / "session.lock").c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC)};
        if (lock.value < 0) continue;
        if (flock(lock.value, LOCK_EX | LOCK_NB) == 0) {
            bool recognized = true;
            for (const auto& child : fs::directory_iterator(directory)) {
                const auto name = child.path().filename();
                if ((name != "session.lock" && name != "stream.cache")
                    || !fs::is_regular_file(child.symlink_status())) recognized = false;
            }
            if (recognized) removeSessionFiles(directory);
        }
    }
}

std::unique_ptr<CacheFile> CacheFile::create(
    std::int64_t video_size, int piece_length, const fs::path& root)
{
    auto file = std::unique_ptr<CacheFile>(new CacheFile);
    file->initialize(video_size, piece_length, root);
    return file;
}

void CacheFile::initialize(std::int64_t video_size, int piece_length, const fs::path& root)
{
    if (video_size <= 0 || piece_length <= 0) throw std::invalid_argument("Invalid cache dimensions");
    budget_ = video_size / 40;
    root_ = root;
    fs::create_directories(root);
    if (!ownedDirectory(root)) throw std::runtime_error("Cache root must be a directory owned by this user.");
    if (chmod(root.c_str(), 0700) != 0) systemFailure("protect cache root");
    struct statfs backing{};
    if (statfs(root.c_str(), &backing) != 0) systemFailure("query cache filesystem type");
    if (backing.f_type == TMPFS_MAGIC || backing.f_type == RAMFS_MAGIC) {
        throw std::runtime_error("Disk cache requires disk-backed storage; tmpfs/ramfs is unsupported. Choose XDG_CACHE_HOME on disk.");
    }
    recoverStale(root);
    struct statvfs filesystem{};
    if (statvfs(root.c_str(), &filesystem) != 0) systemFailure("query cache filesystem");
    const std::int64_t block = filesystem.f_frsize ? filesystem.f_frsize : filesystem.f_bsize;
    if (block <= 0) throw std::runtime_error("Cannot determine cache filesystem allocation unit.");
    slot_size_ = ((std::int64_t{piece_length} + block - 1) / block) * block;
    if (budget_ < slot_size_ + 2 * block) {
        throw std::runtime_error("2.5% cache budget cannot fit one whole torrent piece plus filesystem overhead.");
    }
    auto pattern = (root / "session-XXXXXX").string();
    const auto created = mkdtemp(pattern.data());
    if (!created) systemFailure("create temporary cache session");
    directory_ = created;
    lock_fd_ = open((directory_ / "session.lock").c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) systemFailure("lock cache session");
    fd_ = open((directory_ / "stream.cache").c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) systemFailure("create bounded cache file");
    const auto overhead = allocatedBytes();
    // Reserve one extra allocation unit for filesystem rounding/metadata changes.
    if (budget_ <= overhead + block || budget_ - overhead - block < slot_size_) {
        throw std::runtime_error("2.5% cache budget is too small after filesystem overhead.");
    }
    slots_ = static_cast<std::size_t>((budget_ - overhead - block) / slot_size_);
    const auto capacity = static_cast<std::int64_t>(slots_) * slot_size_;
    const int result = posix_fallocate(fd_, 0, capacity);
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), "preallocate bounded disk cache");
    }
    if (allocatedBytes() > budget_) {
        throw std::runtime_error("Filesystem allocation exceeds the 2.5% budget; cache refused.");
    }
}

CacheFile::~CacheFile()
{
    if (fd_ >= 0) close(fd_);
    if (!directory_.empty()) removeSessionFiles(directory_);
    if (lock_fd_ >= 0) close(lock_fd_);
}

std::int64_t CacheFile::allocatedBytes() const
{
    return allocated(root_) + allocated(directory_) + allocated(directory_ / "session.lock")
         + allocated(directory_ / "stream.cache");
}

void CacheFile::write(std::int64_t offset, const char* bytes, std::size_t length)
{
    const auto capacity = static_cast<std::int64_t>(slots_) * slot_size_;
    if (offset < 0 || offset > capacity || length > static_cast<std::uint64_t>(capacity - offset)) {
        throw std::out_of_range("Write would exceed fixed cache capacity.");
    }
    while (length != 0) {
        const auto written = pwrite(fd_, bytes, length, offset);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) systemFailure("write bounded cache");
        if (written == 0) throw std::runtime_error("Disk cache write made no progress.");
        offset += written;
        bytes += written;
        length -= written;
    }
    if (allocatedBytes() > budget_) throw std::runtime_error("Cache allocation exceeded budget; stop session.");
}

void CacheFile::read(std::int64_t offset, char* bytes, std::size_t length) const
{
    const auto capacity = static_cast<std::int64_t>(slots_) * slot_size_;
    if (offset < 0 || offset > capacity || length > static_cast<std::uint64_t>(capacity - offset)) {
        throw std::out_of_range("Read exceeds fixed cache capacity.");
    }
    while (length != 0) {
        const auto received = pread(fd_, bytes, length, offset);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) systemFailure("read bounded cache");
        if (received == 0) throw std::runtime_error("Unexpected end of disk cache.");
        offset += received;
        bytes += received;
        length -= received;
    }
}

} // namespace intezam
