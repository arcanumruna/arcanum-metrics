#pragma once
// Process stats published as gauges from the owner's duty cycle: open fds, threads, resident and virtual bytes,
// plus a read-error counter. No thread, no clock of its own (the caller passes nowNs), no allocation after init.
// statm, status and the fd directory are opened once in init and re-read with pread / getdents64 into fixed buffers.
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsWriter.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <climits>
#include <span>
#include <variant>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace metrics {

struct SystemMetricsParams {
    int64_t intervalNs;
    const char* procRoot;  // "/proc/self"; injectable for tests
};

class SystemMetrics {
public:
    enum class Status : int32_t { Ok, OpenFailed, AllocFailed };

    // Type ids sit beside Rune's own block (1'000'201..1'000'241) and do not overlap it.
    static constexpr int32_t kTypeOpenFds = 1'000'301;
    static constexpr int32_t kTypeThreads = 1'000'302;
    static constexpr int32_t kTypeResidentBytes = 1'000'303;
    static constexpr int32_t kTypeVirtualBytes = 1'000'304;
    static constexpr int32_t kTypeReadErrors = 1'000'305;

    SystemMetrics() noexcept = default;
    SystemMetrics(const SystemMetrics&) = delete;
    SystemMetrics& operator=(const SystemMetrics&) = delete;
    ~SystemMetrics() { closeAll(); }

    template <class Writer>
    [[nodiscard]] Status init(Writer& writer, const SystemMetricsParams& params, std::span<const std::byte> key) noexcept {
        if (!std::holds_alternative<Uninit>(schedule_)) return Status::Ok;
        if (!openAll(params.procRoot)) return Status::OpenFailed;
        struct Def { SlotWriter* out; int32_t type; MetricKind kind; const char* label; };
        const Def defs[] = {
            {&openFds_, kTypeOpenFds, MetricKind::Gauge, "process_open_fds"},
            {&threads_, kTypeThreads, MetricKind::Gauge, "process_threads"},
            {&residentBytes_, kTypeResidentBytes, MetricKind::Gauge, "process_resident_memory_bytes"},
            {&virtualBytes_, kTypeVirtualBytes, MetricKind::Gauge, "process_virtual_memory_bytes"},
            {&readErrors_, kTypeReadErrors, MetricKind::Counter, "process_metrics_read_errors_total"},
        };
        int32_t done = 0;
        for (const Def& d : defs) {
            const AllocResult r = writer.allocate(d.type, d.kind, -1, key, d.label);
            if (r.status != AllocStatus::Ok) {
                for (int32_t i = 0; i < done; ++i) (void)writer.free(defs[i].out->slot());
                closeAll();
                return Status::AllocFailed;
            }
            *d.out = r.writer;
            ++done;
        }
        intervalNs_ = params.intervalNs;
        pageSize_ = ::sysconf(_SC_PAGESIZE) > 0 ? ::sysconf(_SC_PAGESIZE) : 4096;
        schedule_ = Armed{};
        return Status::Ok;
    }

    // The fd of the opened fd directory (it is listed in its own directory on a real /proc; see sample()).
    [[nodiscard]] int procDirFd() const noexcept { return fdDirFd_; }

    // Returns the number of samples taken (0 or 1). Not due until intervalNs has passed on the caller's clock.
    int doWork(int64_t nowNs) noexcept {
        if (std::holds_alternative<Uninit>(schedule_)) return 0;
        if (const auto* s = std::get_if<Sampled>(&schedule_)) {
            if (nowNs < s->lastNs || nowNs - s->lastNs < intervalNs_) return 0;
        }
        sample();
        schedule_ = Sampled{nowNs};
        return 1;
    }

private:
    struct Uninit {};
    struct Armed {};
    struct Sampled { int64_t lastNs; };

    bool openAll(const char* root) noexcept {
        if (root == nullptr) return false;
        size_t n = 0;  // bounded: never reads past PATH_MAX bytes of an unterminated or huge root
        while (n < PATH_MAX && root[n] != '\0') ++n;
        if (n == 0 || n + 8 >= PATH_MAX) return false;
        char path[PATH_MAX];
        std::memcpy(path, root, n);
        std::memcpy(path + n, "/statm", 7);
        statmFd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        std::memcpy(path + n, "/status", 8);
        statusFd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        std::memcpy(path + n, "/fd", 4);
        fdDirFd_ = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (statmFd_ < 0 || statusFd_ < 0 || fdDirFd_ < 0) {
            closeAll();
            return false;
        }
        return true;
    }
    void closeAll() noexcept {
        for (int* fd : {&statmFd_, &statusFd_, &fdDirFd_}) {
            if (*fd >= 0) ::close(*fd);
            *fd = -1;
        }
        schedule_ = Uninit{};
    }

    static bool parseU64(const char*& p, const char* end, uint64_t& out) noexcept {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end || *p < '0' || *p > '9') return false;
        uint64_t v = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            const uint64_t d = static_cast<uint64_t>(*p - '0');
            if (v > (UINT64_MAX - d) / 10) return false;
            v = v * 10 + d;
            ++p;
        }
        out = v;
        return true;
    }

    // True when `name` is the decimal text of `value`: the directory fd lists itself in /proc/self/fd and is not
    // a descriptor the process holds for its own use.
    static bool isNumber(const char* name, int value) noexcept {
        char tmp[16];
        int n = 0;
        int v = value;
        do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0 && n < 15);
        for (int i = 0; i < n; ++i) {
            if (name[i] != tmp[n - 1 - i]) return false;
        }
        return name[n] == '\0';
    }

    void sample() noexcept {
        char buf[4096];
        bool ok = false;
        // statm: size resident ... (pages). A short read or a missing second field is an error.
        {
            const ssize_t n = ::pread(statmFd_, buf, sizeof buf, 0);
            uint64_t size = 0, resident = 0;
            if (n > 0) {
                const char* p = buf;
                const char* end = buf + n;
                ok = parseU64(p, end, size) && p < end && (*p == ' ' || *p == '\t') && parseU64(p, end, resident) &&
                     size <= static_cast<uint64_t>(INT64_MAX) / static_cast<uint64_t>(pageSize_) &&
                     resident <= static_cast<uint64_t>(INT64_MAX) / static_cast<uint64_t>(pageSize_);
            }
            if (ok) {
                virtualBytes_.set(static_cast<int64_t>(size) * pageSize_);
                residentBytes_.set(static_cast<int64_t>(resident) * pageSize_);
            } else {
                readErrors_.add(1);
            }
        }
        // status: the "Threads:" line.
        {
            const ssize_t n = ::pread(statusFd_, buf, sizeof buf, 0);
            ok = false;
            uint64_t threads = 0;
            if (n > 0) {
                static constexpr char kTag[] = "Threads:";
                const char* end = buf + n;
                for (const char* line = buf; line < end && !ok;) {
                    if (static_cast<size_t>(end - line) > sizeof(kTag) - 1 && std::memcmp(line, kTag, sizeof(kTag) - 1) == 0) {
                        const char* p = line + sizeof(kTag) - 1;
                        ok = parseU64(p, end, threads) && p < end && *p == '\n' &&
                             threads <= static_cast<uint64_t>(INT64_MAX);
                        break;
                    }
                    const void* nl = std::memchr(line, '\n', static_cast<size_t>(end - line));
                    if (nl == nullptr) break;
                    line = static_cast<const char*>(nl) + 1;
                }
            }
            if (ok) threads_.set(static_cast<int64_t>(threads)); else readErrors_.add(1);
        }
        // fd directory: count entries other than "." and "..".
        {
            alignas(8) char dents[4096];
            int64_t count = 0;
            ok = ::lseek(fdDirFd_, 0, SEEK_SET) == 0;
            while (ok) {
                const long got = ::syscall(SYS_getdents64, fdDirFd_, dents, sizeof dents);
                if (got < 0) { ok = false; break; }
                if (got == 0) break;
                for (long off = 0; off < got;) {
                    // linux_dirent64: u64 ino, s64 off, u16 reclen, u8 type, char name[]
                    uint16_t reclen;
                    std::memcpy(&reclen, dents + off + 16, sizeof reclen);
                    if (reclen == 0) { ok = false; break; }
                    const char* name = dents + off + 19;
                    if (!(name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) && !isNumber(name, fdDirFd_)) {
                        ++count;
                    }
                    off += reclen;
                }
            }
            if (ok) openFds_.set(count); else readErrors_.add(1);
        }
    }

    int statmFd_ = -1;
    int statusFd_ = -1;
    int fdDirFd_ = -1;
    int64_t intervalNs_ = 0;
    int64_t pageSize_ = 4096;
    std::variant<Uninit, Armed, Sampled> schedule_{Uninit{}};
    SlotWriter openFds_, threads_, residentBytes_, virtualBytes_, readErrors_;
};

}  // namespace metrics
