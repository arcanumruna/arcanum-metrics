#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

// Cache line size for padding to avoid false sharing
constexpr size_t CACHE_LINE_SIZE = 64;

// Magic number to identify valid mmap files
constexpr uint64_t MAGIC = 0x4D4D41504D455452ULL; // "MMAPMETR"

// Version for compatibility checking
constexpr uint32_t VERSION = 1;

// Maximum metrics supported
constexpr size_t MAX_METRICS = 4096;

// Maximum length for metric name + labels combined
constexpr size_t MAX_NAME_LEN = 256;

enum class MetricType : uint8_t {
    COUNTER = 1,
    GAUGE = 2,
    HISTOGRAM = 3,    // Future use
    THROUGHPUT = 4,   // Sliding time window rate metric
    WINDOWED_MAX = 5, // Maximum sample value over a sliding time window
    WINDOWED_AVG = 6  // Average sample value over a sliding time window
};

// Metric metadata - stored in metadata region
struct alignas(CACHE_LINE_SIZE) MetricMeta {
    char name[MAX_NAME_LEN];      // Prometheus-style name with labels
    MetricType type;
    uint8_t active;               // 1 if slot is in use
    uint16_t reserved;
    uint32_t value_offset;        // Offset into values region
};

// Header at start of mmap region
struct MetricsHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t max_metrics;
    std::atomic<uint32_t> metric_count;
    uint32_t metadata_offset;
    uint32_t values_offset;
    uint32_t total_size;
    char reserved[CACHE_LINE_SIZE - 32];
};

static_assert(sizeof(MetricsHeader) == CACHE_LINE_SIZE, "Header must be cache-line aligned");

// Value slot - cache-line aligned to prevent false sharing
struct alignas(CACHE_LINE_SIZE) ValueSlot {
    std::atomic<int64_t> value;
    char padding[CACHE_LINE_SIZE - sizeof(std::atomic<int64_t>)];
};

static_assert(sizeof(ValueSlot) == CACHE_LINE_SIZE, "ValueSlot must be cache-line aligned");

// Calculate total mmap size needed
constexpr size_t calc_mmap_size(size_t max_metrics = MAX_METRICS) {
    return sizeof(MetricsHeader) +
           (max_metrics * sizeof(MetricMeta)) + 
           (max_metrics * sizeof(ValueSlot));
}

// Forward declarations
class MetricsRegistry;

// Counter - can only increment
class CounterMetric {
public:
    CounterMetric() : slot_(nullptr) {}
    
    void inc() noexcept {
        if (slot_) slot_->value.fetch_add(1, std::memory_order_relaxed);
    }
    
    void add(int64_t v) noexcept {
        if (slot_ && v > 0) slot_->value.fetch_add(v, std::memory_order_relaxed);
    }
    
    int64_t get() const noexcept {
        return slot_ ? slot_->value.load(std::memory_order_relaxed) : 0;
    }

private:
    friend class MetricsRegistry;
    explicit CounterMetric(ValueSlot* slot) : slot_(slot) {}
    ValueSlot* slot_;
};

// Gauge - can go up or down
class GaugeMetric {
public:
    GaugeMetric() : slot_(nullptr) {}
    
    void set(int64_t v) noexcept {
        if (slot_) slot_->value.store(v, std::memory_order_relaxed);
    }
    
    void inc() noexcept {
        if (slot_) slot_->value.fetch_add(1, std::memory_order_relaxed);
    }
    
    void dec() noexcept {
        if (slot_) slot_->value.fetch_sub(1, std::memory_order_relaxed);
    }
    
    void add(int64_t v) noexcept {
        if (slot_) slot_->value.fetch_add(v, std::memory_order_relaxed);
    }
    
    void sub(int64_t v) noexcept {
        if (slot_) slot_->value.fetch_sub(v, std::memory_order_relaxed);
    }
    
    int64_t get() const noexcept {
        return slot_ ? slot_->value.load(std::memory_order_relaxed) : 0;
    }

private:
    friend class MetricsRegistry;
    explicit GaugeMetric(ValueSlot* slot) : slot_(slot) {}
    ValueSlot* slot_;
};

// ThroughputMetric - measures data transfer rate (bytes/sec) over a sliding time window.
//
// Maintains an in-process ring buffer of per-second byte counters and periodically
// writes the computed rate to two mmap slots visible to reader processes:
//   <name>_rate_bps   - current throughput in bytes/sec  (MetricType::THROUGHPUT)
//   <name>_total_bytes - cumulative bytes transferred     (MetricType::COUNTER)
//
// The ring buffer uses one bucket per second. Stale buckets (outside the window)
// are excluded from the rate computation. The rate is recomputed at most once per
// RATE_UPDATE_INTERVAL_MS milliseconds to keep record() as cheap as possible.
//
// Thread safety: record() and flush() are safe to call concurrently from multiple
// threads. Bucket resets use a CAS; at most one byte per reset may be lost during
// the brief window between a CAS and the subsequent store — an acceptable
// approximation for a metrics library.
class ThroughputMetric {
public:
    // Maximum sliding window (= ring buffer depth). window_seconds must be <= this.
    static constexpr size_t BUCKET_COUNT = 60;
    // Rate is recomputed and pushed to mmap at most once every this many milliseconds.
    static constexpr int64_t RATE_UPDATE_INTERVAL_MS = 100;

    ThroughputMetric() = default;
    ~ThroughputMetric() = default;

    // std::atomic is not movable by default, so we implement move operations manually.
    ThroughputMetric(ThroughputMetric&& other) noexcept
        : buckets_(std::move(other.buckets_))
        , window_seconds_(other.window_seconds_)
        , rate_slot_(other.rate_slot_)
        , total_slot_(other.total_slot_)
        , last_rate_update_ms_(other.last_rate_update_ms_.load(std::memory_order_relaxed)) {
        other.rate_slot_  = nullptr;
        other.total_slot_ = nullptr;
    }

    ThroughputMetric& operator=(ThroughputMetric&& other) noexcept {
        if (this != &other) {
            buckets_       = std::move(other.buckets_);
            window_seconds_ = other.window_seconds_;
            rate_slot_     = other.rate_slot_;
            total_slot_    = other.total_slot_;
            last_rate_update_ms_.store(
                other.last_rate_update_ms_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            other.rate_slot_  = nullptr;
            other.total_slot_ = nullptr;
        }
        return *this;
    }

    ThroughputMetric(const ThroughputMetric&) = delete;
    ThroughputMetric& operator=(const ThroughputMetric&) = delete;

    // Record bytes transferred. Thread-safe and non-blocking.
    // Internally updates the sliding window and, if enough time has elapsed,
    // recomputes and flushes the rate to the mmap slot.
    void record(int64_t bytes) noexcept {
        if (!rate_slot_ || bytes <= 0) return;

        // Accumulate into the total bytes counter (visible to readers immediately).
        total_slot_->value.fetch_add(bytes, std::memory_order_relaxed);

        // Add to the current second's sliding window bucket.
        const int64_t now_sec = current_second();
        const size_t  idx     = static_cast<size_t>(now_sec) % BUCKET_COUNT;

        int64_t epoch = buckets_[idx].epoch_sec.load(std::memory_order_acquire);
        if (epoch != now_sec) {
            // Bucket belongs to a past second. One thread wins the CAS and resets it.
            if (buckets_[idx].epoch_sec.compare_exchange_strong(
                    epoch, now_sec,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                buckets_[idx].bytes.store(0, std::memory_order_release);
            }
            // Losers fall through and fetch_add below, which is correct.
        }
        buckets_[idx].bytes.fetch_add(bytes, std::memory_order_relaxed);

        // Throttle rate computation: recompute at most once per interval.
        const int64_t now_ms = current_millisecond();
        int64_t last = last_rate_update_ms_.load(std::memory_order_relaxed);
        if (now_ms - last >= RATE_UPDATE_INTERVAL_MS) {
            if (last_rate_update_ms_.compare_exchange_weak(
                    last, now_ms, std::memory_order_relaxed)) {
                flush_rate(now_sec);
            }
        }
    }

    // Explicitly recompute and push the current rate to the mmap slot.
    // Useful just before a reader polls, or when the call rate is very low.
    void flush() noexcept {
        if (rate_slot_) flush_rate(current_second());
    }

    // Current throughput in bytes/sec (reads the mmap slot; may be up to
    // RATE_UPDATE_INTERVAL_MS stale unless flush() was called recently).
    int64_t rate() const noexcept {
        return rate_slot_ ? rate_slot_->value.load(std::memory_order_relaxed) : 0;
    }

    // Cumulative bytes transferred since the metric was registered.
    int64_t total() const noexcept {
        return total_slot_ ? total_slot_->value.load(std::memory_order_relaxed) : 0;
    }

private:
    friend class MetricsRegistry;

    // One cache-line per bucket prevents false sharing between concurrent writers
    // that happen to land in adjacent buckets.
    struct alignas(CACHE_LINE_SIZE) Bucket {
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> epoch_sec{-1};
        char padding[CACHE_LINE_SIZE - 2 * sizeof(std::atomic<int64_t>)];
    };
    static_assert(sizeof(Bucket) == CACHE_LINE_SIZE, "Bucket must be one cache line");

    explicit ThroughputMetric(ValueSlot* rate_slot, ValueSlot* total_slot,
                              size_t window_seconds)
        : buckets_(std::make_unique<Bucket[]>(BUCKET_COUNT))
        , window_seconds_(window_seconds < BUCKET_COUNT ? window_seconds : BUCKET_COUNT)
        , rate_slot_(rate_slot)
        , total_slot_(total_slot) {}

    // Sum all buckets within the window and write bytes/sec to the mmap slot.
    void flush_rate(int64_t now_sec) noexcept {
        int64_t window_bytes = 0;
        for (size_t i = 0; i < BUCKET_COUNT; ++i) {
            const int64_t epoch = buckets_[i].epoch_sec.load(std::memory_order_acquire);
            if (epoch >= 0 &&
                (now_sec - epoch) < static_cast<int64_t>(window_seconds_)) {
                window_bytes += buckets_[i].bytes.load(std::memory_order_relaxed);
            }
        }
        const int64_t bps = (window_seconds_ > 0)
            ? window_bytes / static_cast<int64_t>(window_seconds_)
            : 0;
        rate_slot_->value.store(bps, std::memory_order_relaxed);
    }

    static int64_t current_second() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static int64_t current_millisecond() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::unique_ptr<Bucket[]>   buckets_;
    size_t                      window_seconds_{60};
    ValueSlot*                  rate_slot_{nullptr};
    ValueSlot*                  total_slot_{nullptr};
    std::atomic<int64_t>        last_rate_update_ms_{0};
};

// WindowedMaxGauge - records the maximum sample value over a sliding time window.
//
// Internally keeps a circular buffer of (timestamp_ms, value) samples. On every
// update() call, expired samples are evicted and the windowed maximum is written
// atomically to the mmap slot so reader processes see a fresh value.
//
// Thread safety: update() is serialised by an internal mutex. get() is lock-free
// (reads the mmap slot). At typical rates (≤ 100 Hz) the mutex is uncontended
// and held for only a few microseconds.
//
// When the window is empty (no samples yet, or all have expired) get() returns 0.
class WindowedMaxGauge {
public:
    static constexpr size_t DEFAULT_CAPACITY = 1024;

    WindowedMaxGauge() = default;
    ~WindowedMaxGauge() = default;

    // std::mutex is not movable; implement move operations manually.
    // Move is only invoked at construction time (return from register_windowed_max),
    // so there is no concurrent access to the source object and locking is not needed.
    WindowedMaxGauge(WindowedMaxGauge&& other) noexcept
        : ring_(std::move(other.ring_))
        , capacity_(other.capacity_)
        , head_(other.head_)
        , count_(other.count_)
        , window_ms_(other.window_ms_)
        , slot_(other.slot_) {
        other.slot_ = nullptr;
    }

    WindowedMaxGauge& operator=(WindowedMaxGauge&& other) noexcept {
        if (this != &other) {
            ring_       = std::move(other.ring_);
            capacity_   = other.capacity_;
            head_       = other.head_;
            count_      = other.count_;
            window_ms_  = other.window_ms_;
            slot_       = other.slot_;
            other.slot_ = nullptr;
        }
        return *this;
    }

    WindowedMaxGauge(const WindowedMaxGauge&) = delete;
    WindowedMaxGauge& operator=(const WindowedMaxGauge&) = delete;

    // Record a new sample and update the windowed maximum in the mmap slot.
    void update(int64_t val) noexcept {
        if (!slot_) return;
        const int64_t now_ms = current_millisecond();

        std::lock_guard<std::mutex> lock(mtx_);

        // Append sample; overwrite the oldest entry when the ring is full.
        const size_t write_idx = (head_ + count_) % capacity_;
        ring_[write_idx] = {now_ms, val};
        if (count_ < capacity_) {
            ++count_;
        } else {
            head_ = (head_ + 1) % capacity_;
        }

        // Evict samples older than the window from the front.
        const int64_t cutoff = now_ms - window_ms_;
        while (count_ > 0 && ring_[head_].timestamp_ms < cutoff) {
            head_ = (head_ + 1) % capacity_;
            --count_;
        }

        // Compute and publish windowed maximum.
        if (count_ == 0) {
            slot_->value.store(0, std::memory_order_relaxed);
            return;
        }
        int64_t max_val = std::numeric_limits<int64_t>::min();
        for (size_t i = 0; i < count_; ++i) {
            const int64_t v = ring_[(head_ + i) % capacity_].value;
            if (v > max_val) max_val = v;
        }
        slot_->value.store(max_val, std::memory_order_relaxed);
    }

    // Current windowed maximum. Lock-free read from the mmap slot.
    // Returns 0 if no samples are in the window.
    int64_t get() const noexcept {
        return slot_ ? slot_->value.load(std::memory_order_relaxed) : 0;
    }

private:
    friend class MetricsRegistry;

    struct Sample { int64_t timestamp_ms; int64_t value; };

    explicit WindowedMaxGauge(ValueSlot* slot, int64_t window_ms, size_t capacity)
        : ring_(std::make_unique<Sample[]>(capacity))
        , capacity_(capacity)
        , window_ms_(window_ms)
        , slot_(slot) {}

    static int64_t current_millisecond() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::mutex                  mtx_;
    std::unique_ptr<Sample[]>   ring_;
    size_t                      capacity_{DEFAULT_CAPACITY};
    size_t                      head_{0};
    size_t                      count_{0};
    int64_t                     window_ms_{10'000};
    ValueSlot*                  slot_{nullptr};
};

// WindowedAvgGauge - records the average of all samples in a sliding time window.
//
// Identical ring-buffer design as WindowedMaxGauge; the difference is that
// update() computes sum/count rather than max across the active window.
//
// The mmap slot stores the integer average (sum / count). If sub-integer precision
// matters, scale inputs before calling update() (e.g. pass micro-units and divide
// by 1 000 in the reader).
//
// Thread safety: same guarantees as WindowedMaxGauge.
class WindowedAvgGauge {
public:
    static constexpr size_t DEFAULT_CAPACITY = 1024;

    WindowedAvgGauge() = default;
    ~WindowedAvgGauge() = default;

    WindowedAvgGauge(WindowedAvgGauge&& other) noexcept
        : ring_(std::move(other.ring_))
        , capacity_(other.capacity_)
        , head_(other.head_)
        , count_(other.count_)
        , window_ms_(other.window_ms_)
        , slot_(other.slot_) {
        other.slot_ = nullptr;
    }

    WindowedAvgGauge& operator=(WindowedAvgGauge&& other) noexcept {
        if (this != &other) {
            ring_       = std::move(other.ring_);
            capacity_   = other.capacity_;
            head_       = other.head_;
            count_      = other.count_;
            window_ms_  = other.window_ms_;
            slot_       = other.slot_;
            other.slot_ = nullptr;
        }
        return *this;
    }

    WindowedAvgGauge(const WindowedAvgGauge&) = delete;
    WindowedAvgGauge& operator=(const WindowedAvgGauge&) = delete;

    // Record a new sample and update the windowed average in the mmap slot.
    void update(int64_t val) noexcept {
        if (!slot_) return;
        const int64_t now_ms = current_millisecond();

        std::lock_guard<std::mutex> lock(mtx_);

        // Append sample; overwrite the oldest entry when the ring is full.
        const size_t write_idx = (head_ + count_) % capacity_;
        ring_[write_idx] = {now_ms, val};
        if (count_ < capacity_) {
            ++count_;
        } else {
            head_ = (head_ + 1) % capacity_;
        }

        // Evict samples older than the window from the front.
        const int64_t cutoff = now_ms - window_ms_;
        while (count_ > 0 && ring_[head_].timestamp_ms < cutoff) {
            head_ = (head_ + 1) % capacity_;
            --count_;
        }

        // Compute and publish windowed average.
        if (count_ == 0) {
            slot_->value.store(0, std::memory_order_relaxed);
            return;
        }
        int64_t sum = 0;
        for (size_t i = 0; i < count_; ++i) {
            sum += ring_[(head_ + i) % capacity_].value;
        }
        slot_->value.store(sum / static_cast<int64_t>(count_), std::memory_order_relaxed);
    }

    // Current windowed average. Lock-free read from the mmap slot.
    // Returns 0 if no samples are in the window.
    int64_t get() const noexcept {
        return slot_ ? slot_->value.load(std::memory_order_relaxed) : 0;
    }

private:
    friend class MetricsRegistry;

    struct Sample { int64_t timestamp_ms; int64_t value; };

    explicit WindowedAvgGauge(ValueSlot* slot, int64_t window_ms, size_t capacity)
        : ring_(std::make_unique<Sample[]>(capacity))
        , capacity_(capacity)
        , window_ms_(window_ms)
        , slot_(slot) {}

    static int64_t current_millisecond() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::mutex                  mtx_;
    std::unique_ptr<Sample[]>   ring_;
    size_t                      capacity_{DEFAULT_CAPACITY};
    size_t                      head_{0};
    size_t                      count_{0};
    int64_t                     window_ms_{10'000};
    ValueSlot*                  slot_{nullptr};
};

// Main registry - creates/opens the mmap and manages metrics
class MetricsRegistry {
public:
    // Create a new mmap file (server mode)
    static MetricsRegistry create(const std::string& path, size_t max_metrics = MAX_METRICS) {
        size_t size = calc_mmap_size(max_metrics);
        
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            throw std::runtime_error("Failed to create mmap file: " + path);
        }
        
        // Acquire exclusive lock while initializing.
        // Use non-blocking + retry to avoid a deadlock when the producer process
        // restarts (e.g. k8s container restart) while a reader sidecar is still
        // running and holding LOCK_SH on the same file.
        {
            constexpr int kMaxWaitMs = 10'000; // 10 s
            int waited = 0;
            while (true) {
                int r = ::flock(fd, LOCK_EX | LOCK_NB);
                if (r == 0) break;
                if (errno == EINTR) continue;
                if (errno == EWOULDBLOCK) {
                    if (waited >= kMaxWaitMs) {
                        ::close(fd);
                        throw std::runtime_error(
                            "Timeout acquiring exclusive lock on metrics mmap "
                            "(readers still active): " + path);
                    }
                    ::usleep(50'000); // 50 ms
                    waited += 50;
                    continue;
                }
                ::close(fd);
                throw std::runtime_error("Failed to acquire exclusive lock");
            }
        }
        
        if (::ftruncate(fd, size) < 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            throw std::runtime_error("Failed to resize mmap file");
        }
        
        void* ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        
        if (ptr == MAP_FAILED) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            throw std::runtime_error("Failed to mmap file");
        }
        
        // Initialize header
        auto* header = static_cast<MetricsHeader*>(ptr);
        header->magic = MAGIC;
        header->version = VERSION;
        header->max_metrics = max_metrics;
        header->metric_count.store(0, std::memory_order_relaxed);
        header->metadata_offset = sizeof(MetricsHeader);
        header->values_offset = sizeof(MetricsHeader) + (max_metrics * sizeof(MetricMeta));
        header->total_size = size;
        
        // Zero out metadata region
        std::memset(static_cast<char*>(ptr) + header->metadata_offset, 0, 
                    max_metrics * sizeof(MetricMeta));
        
        // Downgrade to shared lock - allows readers, keeps file open
        ::flock(fd, LOCK_SH);
        
        return MetricsRegistry(ptr, size, fd, true);
    }
    
    // Open existing mmap file (reader mode)
    static MetricsRegistry open(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("Failed to open mmap file: " + path);
        }
        
        // Do NOT hold a persistent LOCK_SH: it would prevent the producer from
        // re-acquiring LOCK_EX after a crash/restart while this reader is still
        // running (e.g. metrics-exporter sidecar in the same k8s pod).
        // All metric values use std::atomic, so reads are safe without a lock.
        // The magic/version check below guards against reading an uninitialised file.

        struct stat st;
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            throw std::runtime_error("Failed to stat mmap file");
        }

        void* ptr = ::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

        if (ptr == MAP_FAILED) {
            ::close(fd);
            throw std::runtime_error("Failed to mmap file");
        }

        auto* header = static_cast<MetricsHeader*>(ptr);
        if (header->magic != MAGIC) {
            ::munmap(ptr, st.st_size);
            ::close(fd);
            throw std::runtime_error("Invalid mmap file (bad magic)");
        }

        if (header->version != VERSION) {
            ::munmap(ptr, st.st_size);
            ::close(fd);
            throw std::runtime_error("Version mismatch");
        }
        
        return MetricsRegistry(ptr, st.st_size, fd, false);
    }
    
    ~MetricsRegistry() {
        if (base_) {
            ::munmap(base_, size_);
        }
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    
    // Move only
    MetricsRegistry(MetricsRegistry&& other) noexcept 
        : base_(other.base_), size_(other.size_), fd_(other.fd_), owner_(other.owner_) {
        other.base_ = nullptr;
        other.fd_ = -1;
    }
    
    MetricsRegistry& operator=(MetricsRegistry&& other) noexcept {
        if (this != &other) {
            if (base_) ::munmap(base_, size_);
            if (fd_ >= 0) {
                ::flock(fd_, LOCK_UN);
                ::close(fd_);
            }
            base_ = other.base_;
            size_ = other.size_;
            fd_ = other.fd_;
            owner_ = other.owner_;
            other.base_ = nullptr;
            other.fd_ = -1;
        }
        return *this;
    }
    
    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;
    
    // Register a counter (only valid for owner)
    CounterMetric register_counter(std::string_view name) {
        if (!owner_) throw std::runtime_error("Cannot register metrics on read-only registry");
        return CounterMetric(allocate_slot(name, MetricType::COUNTER));
    }
    
    // Register a gauge (only valid for owner)
    GaugeMetric register_gauge(std::string_view name) {
        if (!owner_) throw std::runtime_error("Cannot register metrics on read-only registry");
        return GaugeMetric(allocate_slot(name, MetricType::GAUGE));
    }

    // Register a throughput metric (only valid for owner).
    //
    // Creates two underlying mmap slots:
    //   <name>_rate_bps    - bytes/sec averaged over the sliding window
    //   <name>_total_bytes - cumulative bytes (monotonic counter)
    //
    // window_seconds: width of the sliding window (1–60 s, default 60).
    ThroughputMetric register_throughput(std::string_view name,
                                         size_t window_seconds = 60) {
        if (!owner_) throw std::runtime_error("Cannot register metrics on read-only registry");
        // Insert suffix before labels: "foo{labels}" → "foo_rate_bps{labels}"
        const std::string base(name);
        const auto lbrace = base.find('{');
        std::string rate_name, total_name;
        if (lbrace != std::string::npos) {
            rate_name  = base.substr(0, lbrace) + "_rate_bps"    + base.substr(lbrace);
            total_name = base.substr(0, lbrace) + "_total_bytes" + base.substr(lbrace);
        } else {
            rate_name  = base + "_rate_bps";
            total_name = base + "_total_bytes";
        }
        ValueSlot* rate_slot  = allocate_slot(rate_name,  MetricType::THROUGHPUT);
        ValueSlot* total_slot = allocate_slot(total_name, MetricType::COUNTER);
        return ThroughputMetric(rate_slot, total_slot, window_seconds);
    }

    // Register a windowed-maximum gauge (only valid for owner).
    //
    // update() records a sample; get() and the mmap slot always reflect the
    // maximum of all samples whose age is less than window_seconds.
    //
    // capacity: ring buffer depth. Must be >= (sample_rate_hz * window_seconds).
    // Default 1024 covers up to ~100 Hz × 10 s.
    WindowedMaxGauge register_windowed_max(std::string_view name,
                                           size_t window_seconds = 10,
                                           size_t capacity = WindowedMaxGauge::DEFAULT_CAPACITY) {
        if (!owner_) throw std::runtime_error("Cannot register metrics on read-only registry");
        ValueSlot* slot = allocate_slot(name, MetricType::WINDOWED_MAX);
        return WindowedMaxGauge(slot,
                                static_cast<int64_t>(window_seconds) * 1000,
                                capacity);
    }

    // Register a windowed-average gauge (only valid for owner).
    //
    // update() records a sample; get() and the mmap slot always reflect the
    // integer average of all samples whose age is less than window_seconds.
    //
    // capacity: ring buffer depth. Must be >= (sample_rate_hz * window_seconds).
    // Default 1024 covers up to ~100 Hz × 10 s.
    WindowedAvgGauge register_windowed_avg(std::string_view name,
                                           size_t window_seconds = 10,
                                           size_t capacity = WindowedAvgGauge::DEFAULT_CAPACITY) {
        if (!owner_) throw std::runtime_error("Cannot register metrics on read-only registry");
        ValueSlot* slot = allocate_slot(name, MetricType::WINDOWED_AVG);
        return WindowedAvgGauge(slot,
                                static_cast<int64_t>(window_seconds) * 1000,
                                capacity);
    }

    // Read access for exporter
    const MetricsHeader* header() const { return static_cast<MetricsHeader*>(base_); }
    
    const MetricMeta* metadata() const {
        return reinterpret_cast<const MetricMeta*>(
            static_cast<char*>(base_) + header()->metadata_offset);
    }
    
    const ValueSlot* values() const {
        return reinterpret_cast<const ValueSlot*>(
            static_cast<char*>(base_) + header()->values_offset);
    }
    
    uint32_t metric_count() const {
        return header()->metric_count.load(std::memory_order_acquire);
    }

private:
    MetricsRegistry(void* base, size_t size, int fd, bool owner)
        : base_(base), size_(size), fd_(fd), owner_(owner) {}
    
    ValueSlot* allocate_slot(std::string_view name, MetricType type) {
        auto* hdr = static_cast<MetricsHeader*>(base_);
        uint32_t idx = hdr->metric_count.fetch_add(1, std::memory_order_acq_rel);
        
        if (idx >= hdr->max_metrics) {
            hdr->metric_count.fetch_sub(1, std::memory_order_relaxed);
            throw std::runtime_error("Max metrics exceeded");
        }
        
        auto* meta = reinterpret_cast<MetricMeta*>(
            static_cast<char*>(base_) + hdr->metadata_offset) + idx;
        
        size_t len = std::min(name.size(), MAX_NAME_LEN - 1);
        std::memcpy(meta->name, name.data(), len);
        meta->name[len] = '\0';
        meta->type = type;
        meta->value_offset = idx;
        meta->active = 1;
        
        auto* slot = reinterpret_cast<ValueSlot*>(
            static_cast<char*>(base_) + hdr->values_offset) + idx;
        slot->value.store(0, std::memory_order_relaxed);
        
        return slot;
    }
    
    void* base_;
    size_t size_;
    int fd_;
    bool owner_;
};