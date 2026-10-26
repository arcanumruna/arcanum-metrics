#pragma once
// A metrics file mapping. Create is tmp + rename (never O_TRUNC, never flock); open is a read-only mapping that
// waits a bounded number of polls for the version word. No allocation, no exceptions, no clock.
#include "metrics/MetricsLayout.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace metrics {

enum class FileStatus : int32_t {
    Ok,
    PathTooLong,
    CapacityTooSmall,
    CapacityTooLarge,
    IoError,
    NotReady,
    VersionMismatch,
    BadLength,
    TooLarge  // maxSlots above kMaxSlots
};

struct CreateParams {
    const char* path;
    int32_t maxSlots;
    int64_t requiredSlots;
    int64_t pid;
    int64_t startEpochNs;
    std::span<const std::byte> userTag;
};

// Two concurrent create() calls for the same path in one process are a caller error (the tmp name is per process).
class MetricsFile {
public:
    // Room for ".tmp." and a 20-digit pid in the tmp name, plus the terminating NUL.
    static constexpr size_t kMaxPathLength = PATH_MAX - 5 - 20 - 1;

    MetricsFile() noexcept = default;
    MetricsFile(const MetricsFile&) = delete;
    MetricsFile& operator=(const MetricsFile&) = delete;
    MetricsFile(MetricsFile&& o) noexcept { *this = static_cast<MetricsFile&&>(o); }
    MetricsFile& operator=(MetricsFile&& o) noexcept {
        if (this != &o) {
            release();
            base_ = o.base_;
            length_ = o.length_;
            maxSlots_ = o.maxSlots_;
            polls_ = o.polls_;
            writable_ = o.writable_;
            dev_ = o.dev_;
            ino_ = o.ino_;
            o.base_ = nullptr;
            o.length_ = 0;
            o.maxSlots_ = 0;
        }
        return *this;
    }
    ~MetricsFile() { release(); }  // munmap only; never unlinks

    [[nodiscard]] static FileStatus create(const CreateParams& p, MetricsFile& out) noexcept {
        const size_t pathLen = boundedLength(p.path);
        if (pathLen == 0 || pathLen > kMaxPathLength) return FileStatus::PathTooLong;
        if (p.requiredSlots > INT32_MAX) return FileStatus::CapacityTooLarge;
        if (p.maxSlots < 0 || int64_t{p.maxSlots} < p.requiredSlots) return FileStatus::CapacityTooSmall;
        if (p.maxSlots > kMaxSlots) return FileStatus::TooLarge;
        if (p.userTag.size() > static_cast<size_t>(kHdrUserTagLength)) return FileStatus::BadLength;

        char tmp[PATH_MAX];
        std::memcpy(tmp, p.path, pathLen);
        size_t n = pathLen;
        std::memcpy(tmp + n, ".tmp.", 5);
        n += 5;
        n += writeDecimal(tmp + n, static_cast<uint64_t>(::getpid()));
        tmp[n] = '\0';

        const FileLayout layout{p.maxSlots};
        const int64_t length = layout.fileLength();

        int fd = ::open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
        if (fd < 0 && errno == EEXIST) {  // stale tmp of a crashed process whose pid we reuse
            (void)!::unlink(tmp);
            fd = ::open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
        }
        if (fd < 0) return FileStatus::IoError;

        FileStatus st = FileStatus::IoError;
        void* m = MAP_FAILED;
        if (::ftruncate(fd, static_cast<off_t>(length)) == 0) {
            m = ::mmap(nullptr, static_cast<size_t>(length), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        if (m != MAP_FAILED) {
            auto* b = static_cast<std::byte*>(m);  // zero-filled by ftruncate; version word is still 0
            store<int64_t>(b, kHdrPidOffset, p.pid);
            store<int64_t>(b, kHdrStartEpochNsOffset, p.startEpochNs);
            store<int32_t>(b, kHdrMaxSlotsOffset, p.maxSlots);
            store<int32_t>(b, kHdrMetadataRecordLengthOffset, kMetadataRecordLength);
            store<int32_t>(b, kHdrValueRecordLengthOffset, kValueRecordLength);
            if (!p.userTag.empty()) std::memcpy(b + kHdrUserTagOffset, p.userTag.data(), p.userTag.size());
            versionRef(b).store(kLayoutVersion, std::memory_order_release);  // last
            struct stat sb;
            if (::fsync(fd) == 0 && ::fstat(fd, &sb) == 0 && ::rename(tmp, p.path) == 0) {
                if (syncParentDirectory(p.path, pathLen)) {
                    out.release();
                    out.base_ = b;
                    out.length_ = static_cast<size_t>(length);
                    out.maxSlots_ = p.maxSlots;
                    out.polls_ = 0;
                    out.writable_ = true;
                    out.dev_ = sb.st_dev;
                    out.ino_ = sb.st_ino;
                    st = FileStatus::Ok;
                } else {
                    ::munmap(m, static_cast<size_t>(length));  // renamed but not durable: the caller must not trust it
                    ::close(fd);
                    return FileStatus::IoError;
                }
            } else {
                ::munmap(m, static_cast<size_t>(length));
            }
        }
        ::close(fd);
        if (st != FileStatus::Ok) (void)!::unlink(tmp);
        return st;
    }

    [[nodiscard]] static FileStatus open(const char* path, int32_t maxVersionPolls, MetricsFile& out) noexcept {
        const size_t pathLen = boundedLength(path);
        if (pathLen == 0 || pathLen > kMaxPathLength) return FileStatus::PathTooLong;
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return FileStatus::IoError;
        struct stat sb;
        if (::fstat(fd, &sb) != 0) {
            ::close(fd);
            return FileStatus::IoError;
        }
        if (sb.st_size < kHeaderLength) {
            ::close(fd);
            return FileStatus::BadLength;
        }
        const size_t size = static_cast<size_t>(sb.st_size);
        void* m = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return FileStatus::IoError;
        auto* b = static_cast<std::byte*>(m);

        int32_t polls = 0;
        int32_t version = 0;
        while (polls < maxVersionPolls) {
            ++polls;
            version = versionRef(b).load(std::memory_order_acquire);
            if (version != 0) break;
        }
        out.polls_ = polls;
        FileStatus st = FileStatus::Ok;
        int32_t maxSlots = 0;
        if (version == 0) {
            st = FileStatus::NotReady;
        } else if (version != kLayoutVersion) {
            st = FileStatus::VersionMismatch;
        } else {
            maxSlots = load<int32_t>(b, kHdrMaxSlotsOffset);
            if (load<int32_t>(b, kHdrMetadataRecordLengthOffset) != kMetadataRecordLength ||
                load<int32_t>(b, kHdrValueRecordLengthOffset) != kValueRecordLength) {
                st = FileStatus::VersionMismatch;
            } else if (maxSlots < 0 || maxSlots > kMaxSlots ||
                       FileLayout{maxSlots}.fileLength() > static_cast<int64_t>(size)) {
                st = FileStatus::BadLength;
            }
        }
        if (st != FileStatus::Ok) {
            ::munmap(m, size);
            return st;
        }
        out.release();
        out.base_ = b;
        out.length_ = size;
        out.maxSlots_ = maxSlots;
        out.writable_ = false;
        out.dev_ = sb.st_dev;
        out.ino_ = sb.st_ino;
        return FileStatus::Ok;
    }

    // True when the file now at `path` is this mapping's file: same device, inode and size, and the same pid and
    // startEpochNs in its header (a recreated file with identical header fields still has a new inode).
    [[nodiscard]] bool sameInstanceAs(const char* path) const noexcept {
        if (base_ == nullptr) return false;
        const size_t pathLen = boundedLength(path);
        if (pathLen == 0 || pathLen > kMaxPathLength) return false;
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat sb;
        std::byte hdr[kHdrStartEpochNsOffset + 8];
        const bool ok = ::fstat(fd, &sb) == 0 && ::pread(fd, hdr, sizeof hdr, 0) == static_cast<ssize_t>(sizeof hdr);
        ::close(fd);
        if (!ok) return false;
        return sb.st_dev == dev_ && sb.st_ino == ino_ && static_cast<size_t>(sb.st_size) == length_ &&
               load<int64_t>(hdr, kHdrPidOffset) == load<int64_t>(base_, kHdrPidOffset) &&
               load<int64_t>(hdr, kHdrStartEpochNsOffset) == load<int64_t>(base_, kHdrStartEpochNsOffset);
    }

    [[nodiscard]] FileLayout layout() const noexcept { return FileLayout{maxSlots_}; }
    // Writable only for a mapping made by create(); an open() mapping is read-only.
    [[nodiscard]] std::byte* base() const noexcept { return base_; }
    [[nodiscard]] size_t length() const noexcept { return length_; }
    // True for a mapping made by create(); an open() mapping is read-only and must not be written through.
    [[nodiscard]] bool writable() const noexcept { return writable_; }
    // Version-word polls made by the last open() into this object.
    [[nodiscard]] int32_t openPolls() const noexcept { return polls_; }

private:
    static size_t boundedLength(const char* s) noexcept {
        if (s == nullptr) return 0;
        size_t n = 0;
        while (n <= PATH_MAX && s[n] != '\0') ++n;
        return n;
    }
    // fsync of the directory holding `path`, so the rename itself is durable.
    static bool syncParentDirectory(const char* path, size_t pathLen) noexcept {
        char dir[PATH_MAX];
        size_t slash = pathLen;
        while (slash > 0 && path[slash - 1] != '/') --slash;
        if (slash == 0) {
            dir[0] = '.';
            dir[1] = '\0';
        } else {
            const size_t n = slash == 1 ? 1 : slash - 1;  // keep "/" for a file in the root
            std::memcpy(dir, path, n);
            dir[n] = '\0';
        }
        const int dfd = ::open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0) return false;
        const bool ok = ::fsync(dfd) == 0;
        ::close(dfd);
        return ok;
    }
    static size_t writeDecimal(char* dst, uint64_t v) noexcept {
        char rev[20];
        size_t k = 0;
        do { rev[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0);
        for (size_t i = 0; i < k; ++i) dst[i] = rev[k - 1 - i];
        return k;
    }
    template <class T>
    static void store(std::byte* b, int32_t off, T v) noexcept { std::memcpy(b + off, &v, sizeof v); }
    template <class T>
    static T load(const std::byte* b, int32_t off) noexcept {
        T v;
        std::memcpy(&v, b + off, sizeof v);
        return v;
    }
    static std::atomic_ref<int32_t> versionRef(std::byte* b) noexcept {
        return std::atomic_ref<int32_t>(*reinterpret_cast<int32_t*>(b + kHdrLayoutVersionOffset));
    }
    void release() noexcept {
        if (base_ != nullptr) ::munmap(base_, length_);
        base_ = nullptr;
        length_ = 0;
    }

    std::byte* base_ = nullptr;
    size_t length_ = 0;
    int32_t maxSlots_ = 0;
    int32_t polls_ = 0;
    bool writable_ = false;
    dev_t dev_ = 0;
    ino_t ino_ = 0;
};

}  // namespace metrics
