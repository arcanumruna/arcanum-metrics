#pragma once

// Process-level system metrics collected from /proc/self and published into the
// shared mmap registry, so the metrics-exporter sidecar serves them alongside the
// application metrics. This is the piece that turns the metrics library into a
// system exporter: the app owns /proc/self and the registry writer, whereas the
// exporter runs in its own PID namespace and cannot see the app process.
//
// Usage (once, near startup, kept alive for the process lifetime):
//     SystemMetrics systemMetrics(registry);            // self-updates in the background
//     SystemMetrics systemMetrics(registry, false);     // manual: call update() yourself
//
// All values are cheap gauges; the background refresh runs at a low cadence
// (default 2s) so cost is negligible and off the hot path.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <dirent.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "metrics/Metrics.h"

class SystemMetrics {
public:
    explicit SystemMetrics(MetricsRegistry& registry,
                           bool autoRefresh = true,
                           int refreshIntervalMs = 2000)
        : openFds_(registry.register_gauge("process_open_fds"))
        , maxFds_(registry.register_gauge("process_max_fds"))
        , residentBytes_(registry.register_gauge("process_resident_memory_bytes"))
        , virtualBytes_(registry.register_gauge("process_virtual_memory_bytes"))
        , threads_(registry.register_gauge("process_threads"))
        , cpuSeconds_(registry.register_gauge("process_cpu_seconds_total"))
        , refreshIntervalMs_(refreshIntervalMs)
    {
        update();  // publish an initial sample immediately
        if (autoRefresh) {
            running_.store(true, std::memory_order_release);
            worker_ = std::thread([this] { run(); });
        }
    }

    ~SystemMetrics() {
        if (running_.exchange(false, std::memory_order_acq_rel)) {
            { std::lock_guard<std::mutex> lk(mtx_); }
            cv_.notify_all();
            if (worker_.joinable()) worker_.join();
        }
    }

    SystemMetrics(const SystemMetrics&) = delete;
    SystemMetrics& operator=(const SystemMetrics&) = delete;
    SystemMetrics(SystemMetrics&&) = delete;
    SystemMetrics& operator=(SystemMetrics&&) = delete;

    // Read /proc/self and republish. Safe to call from any thread; each setter is
    // a single relaxed atomic store. No-op on platforms without /proc.
    void update() noexcept {
        openFds_.set(count_dir_entries("/proc/self/fd"));
        threads_.set(count_dir_entries("/proc/self/task"));

        if (rlimit rl{}; getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            maxFds_.set(rl.rlim_cur == RLIM_INFINITY
                            ? -1
                            : static_cast<int64_t>(rl.rlim_cur));
        }

        read_statm();
        read_cpu();
    }

private:
    void run() noexcept {
        std::unique_lock<std::mutex> lk(mtx_);
        while (running_.load(std::memory_order_acquire)) {
            cv_.wait_for(lk, std::chrono::milliseconds(refreshIntervalMs_),
                         [this] { return !running_.load(std::memory_order_acquire); });
            if (!running_.load(std::memory_order_acquire)) break;
            update();
        }
    }

    static int64_t count_dir_entries(const char* path) noexcept {
        DIR* d = ::opendir(path);
        if (!d) return 0;
        int64_t n = 0;
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
                continue;  // skip . and ..
            }
            ++n;
        }
        ::closedir(d);
        return n;
    }

    void read_statm() noexcept {
        // /proc/self/statm: size resident shared text lib data dt  (all in pages)
        FILE* f = ::fopen("/proc/self/statm", "r");
        if (!f) return;
        long long sizePages = 0, residentPages = 0;
        if (::fscanf(f, "%lld %lld", &sizePages, &residentPages) == 2) {
            const int64_t pageSize = ::sysconf(_SC_PAGESIZE);
            virtualBytes_.set(static_cast<int64_t>(sizePages) * pageSize);
            residentBytes_.set(static_cast<int64_t>(residentPages) * pageSize);
        }
        ::fclose(f);
    }

    void read_cpu() noexcept {
        // /proc/self/stat: utime (field 14) + stime (field 15), in clock ticks.
        // comm (field 2) may contain spaces/parens, so parse after the last ')'.
        FILE* f = ::fopen("/proc/self/stat", "r");
        if (!f) return;
        char buf[4096];
        const size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
        ::fclose(f);
        if (n == 0) return;
        buf[n] = '\0';

        const char* p = buf + n;
        while (p > buf && *p != ')') --p;   // last ')'
        if (*p != ')') return;
        ++p;

        // Tokens after ')': [0]=state(field3) ... utime=field14=token[11], stime=token[12]
        long long fields[16] = {0};
        int count = 0;
        const char* s = p;
        while (count < 16) {
            while (*s == ' ') ++s;
            if (*s == '\0') break;
            fields[count++] = ::strtoll(s, const_cast<char**>(&s), 10);
        }
        if (count > 12) {
            const long clk = ::sysconf(_SC_CLK_TCK);
            if (clk > 0) {
                const long long ticks = fields[11] + fields[12];  // utime + stime
                cpuSeconds_.set(static_cast<int64_t>(ticks / clk));
            }
        }
    }

    GaugeMetric openFds_;
    GaugeMetric maxFds_;
    GaugeMetric residentBytes_;
    GaugeMetric virtualBytes_;
    GaugeMetric threads_;
    GaugeMetric cpuSeconds_;

    int refreshIntervalMs_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
};
