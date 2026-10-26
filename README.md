# Metrics

A header-only, memory-mapped metrics library for C++23. Designed for low-latency
inter-process communication: one producer process writes metrics into a shared-memory
file; any number of reader processes (e.g. a Prometheus exporter) read them without
coordination overhead.

## Table of Contents

- [Architecture](#architecture)
- [Metric Types](#metric-types)
  - [Counter](#counter)
  - [Gauge](#gauge)
  - [Throughput](#throughput)
  - [WindowedMaxGauge](#windowedmaxgauge)
  - [WindowedAvgGauge](#windowedavggauge)
- [MetricsRegistry](#metricsregistry)
- [Usage Examples](#usage-examples)
  - [Producer process](#producer-process)
  - [Reader / exporter process](#reader--exporter-process)
  - [Measuring network bandwidth](#measuring-network-bandwidth)
  - [10 Hz sensor with 1 Hz exporter](#10-hz-sensor-with-1-hz-exporter)
- [Thread Safety](#thread-safety)
- [Memory Layout](#memory-layout)
- [Build & Integration](#build--integration)

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    mmap file on disk                     │
│                                                          │
│  MetricsHeader (64 B)                                    │
│  ├── magic, version, max_metrics                         │
│  ├── metric_count  (atomic)                              │
│  ├── metadata_offset, values_offset, total_size          │
│                                                          │
│  MetricMeta[max_metrics]  (256 B each, cache-aligned)    │
│  ├── name  (Prometheus-style with optional labels)       │
│  ├── type  (COUNTER | GAUGE | THROUGHPUT | WINDOWED_MAX | WINDOWED_AVG | …) │
│  └── value_offset  (index into ValueSlot array)          │
│                                                          │
│  ValueSlot[max_metrics]  (64 B each, cache-aligned)      │
│  └── value  (atomic int64_t)                             │
└─────────────────────────────────────────────────────────┘
         ▲ mmap(PROT_READ|PROT_WRITE)      ▲ mmap(PROT_READ)
         │                                  │
   Producer process                  Reader process(es)
   MetricsRegistry::create()         MetricsRegistry::open()
```

Key design decisions:

| Property | Detail |
|---|---|
| Transport | POSIX `mmap` + `flock` for safe multi-process initialization |
| Alignment | Every `ValueSlot` and `MetricMeta` is cache-line (64 B) aligned to eliminate false sharing |
| Atomics | `std::atomic<int64_t>` with `memory_order_relaxed` for near-zero overhead reads and writes |
| Header-only | The entire library is `include/metrics/Metrics.h`; no separate compilation required |
| Standard | C++23 |

---

## Metric Types

### Counter

A monotonically increasing 64-bit integer. Use it to count events (requests, errors,
bytes sent, etc.). Counters must never decrease; they only reset when the process
restarts.

```cpp
CounterMetric requests = registry.register_counter("http_requests_total");
```

**API**

| Method | Description |
|---|---|
| `inc()` | Increment by 1 |
| `add(int64_t v)` | Add `v` (ignored if `v <= 0`) |
| `get() -> int64_t` | Read current value |

---

### Gauge

A 64-bit integer that can go up or down. Use it for values that represent a current
state: active connections, queue depth, memory usage, CPU temperature, etc.

```cpp
GaugeMetric connections = registry.register_gauge("active_connections");
```

**API**

| Method | Description |
|---|---|
| `set(int64_t v)` | Set to an absolute value |
| `inc()` | Increment by 1 |
| `dec()` | Decrement by 1 |
| `add(int64_t v)` | Add `v` |
| `sub(int64_t v)` | Subtract `v` |
| `get() -> int64_t` | Read current value |

---

### Throughput

A rate metric that measures **data transfer throughput** (bytes/sec) using a
**sliding time window**. It is the right tool for measuring network bandwidth, disk
I/O throughput, message rates, etc.

```cpp
ThroughputMetric bw = registry.register_throughput("net_rx", 60 /* window seconds */);
```

**How it works**

Internally `ThroughputMetric` maintains a ring buffer of **60 per-second buckets**
(one per calendar second of `steady_clock`). Each call to `record(bytes)`:

1. Atomically adds `bytes` to the current second's bucket.
2. Resets stale buckets via a CAS when the second rolls over.
3. At most once every **100 ms**, sums all buckets within the window, divides by
   `window_seconds`, and stores the result (bytes/sec) into the mmap slot so readers
   see an up-to-date rate.

The total cumulative byte count is written to a separate mmap slot on every call, so
readers always have an exact monotonic total.

**`register_throughput` creates two mmap slots**

| Slot name | Type | Description |
|---|---|---|
| `<name>_rate_bps` | `THROUGHPUT` | Current throughput in bytes/sec |
| `<name>_total_bytes` | `COUNTER` | Cumulative bytes transferred (monotonic) |

**API**

| Method | Description |
|---|---|
| `record(int64_t bytes)` | Record bytes transferred; updates sliding window and throttled rate |
| `flush()` | Force-recompute and push rate to mmap immediately |
| `rate() -> int64_t` | Read current bytes/sec from the mmap slot |
| `total() -> int64_t` | Read cumulative total bytes from the mmap slot |

**Parameters to `register_throughput`**

| Parameter | Default | Description |
|---|---|---|
| `name` | — | Base name; two slots `<name>_rate_bps` and `<name>_total_bytes` are registered |
| `window_seconds` | `60` | Width of the sliding window in seconds (1–60) |

---

### WindowedMaxGauge

Tracks the **maximum** of all samples recorded within a sliding time window.
Purpose-built for cases where the consumer polls at a lower frequency than the
producer: even if the exporter only wakes once per second, it will see the true peak
of every sample recorded during that interval.

```cpp
// Sensor runs at 10 Hz; exporter at 1 Hz; window = 10 s
WindowedMaxGauge peak_temp = registry.register_windowed_max("cpu_temp_max_C", 10);
```

**How it works**

Internally maintains a circular buffer of `(timestamp_ms, value)` pairs. On every
`update()`:

1. The new sample is appended to the ring buffer.
2. Samples older than `window_seconds` are evicted from the front.
3. The maximum of the remaining samples is computed and written atomically to the
   mmap slot so readers see a fresh value immediately.

An internal `std::mutex` serialises writes. At ≤ 100 Hz this adds ~100 ns of
latency per call in the worst case; for the stated 10 Hz use case the overhead is
negligible.

**API**

| Method | Description |
|---|---|
| `update(int64_t val)` | Record a sample; evicts expired samples and updates the mmap slot |
| `get() -> int64_t` | Lock-free read of the current windowed maximum from the mmap slot |

**Parameters to `register_windowed_max`**

| Parameter | Default | Description |
|---|---|---|
| `name` | — | Metric name registered in the mmap |
| `window_seconds` | `10` | Width of the sliding window |
| `capacity` | `1024` | Ring buffer depth. Must satisfy `capacity ≥ sample_rate_hz × window_seconds` |

Returns 0 when no samples are in the window.

---

### WindowedAvgGauge

Tracks the **average** of all samples recorded within a sliding time window.
Use it to smooth a noisy high-frequency signal before it reaches a low-frequency
exporter.

```cpp
// Sensor runs at 10 Hz; exporter at 1 Hz; window = 10 s → average over last 100 samples
WindowedAvgGauge avg_latency = registry.register_windowed_avg("rpc_latency_us", 10);
```

**How it works**

Identical ring-buffer design as `WindowedMaxGauge`. On every `update()` the window
sum is divided by the sample count and stored in the mmap slot as an `int64_t`
(integer average, truncated). If sub-integer precision is required, scale inputs
before calling `update()` — for example, pass values in micro-units and divide by
1 000 in the reader.

**API**

| Method | Description |
|---|---|
| `update(int64_t val)` | Record a sample; evicts expired samples and updates the mmap slot |
| `get() -> int64_t` | Lock-free read of the current windowed average from the mmap slot |

**Parameters to `register_windowed_avg`**

| Parameter | Default | Description |
|---|---|---|
| `name` | — | Metric name registered in the mmap |
| `window_seconds` | `10` | Width of the sliding window |
| `capacity` | `1024` | Ring buffer depth. Must satisfy `capacity ≥ sample_rate_hz × window_seconds` |

Returns 0 when no samples are in the window.

---

## MetricsRegistry

`MetricsRegistry` manages the mmap file. It is **move-only** and cannot be copied.

### Creating a registry (producer)

```cpp
auto registry = MetricsRegistry::create("/run/myapp/metrics.mmap");
```

`create()` opens the file with `O_CREAT | O_TRUNC`, acquires an exclusive `flock`
during initialization, writes the header, then downgrades to a shared lock so readers
can attach immediately.

| Parameter | Default | Description |
|---|---|---|
| `path` | — | Path to the mmap file (created if absent, truncated if present) |
| `max_metrics` | `4096` | Maximum number of mmap slots to pre-allocate |

### Opening a registry (reader)

```cpp
auto registry = MetricsRegistry::open("/run/myapp/metrics.mmap");
```

`open()` opens the file read-only, acquires a shared `flock` (blocks until the
producer finishes initialization), validates the magic number and version, then maps
the file. Readers cannot register metrics.

### Registering metrics

Registration is only permitted on the owner (created with `create()`). It is
**not thread-safe** — register all metrics during startup before spawning worker
threads.

```cpp
auto requests   = registry.register_counter("http_requests_total");
auto queue_len  = registry.register_gauge("queue_length");
auto bandwidth  = registry.register_throughput("net_tx_bytes", 30);
auto peak_temp  = registry.register_windowed_max("cpu_temp_max_C", 10);
auto avg_lat    = registry.register_windowed_avg("rpc_latency_us",  10);
```

### Exporter / reader API

```cpp
uint32_t            n    = registry.metric_count();
const MetricMeta*   meta = registry.metadata();   // array of n entries
const ValueSlot*    vals = registry.values();      // array of n entries

for (uint32_t i = 0; i < n; ++i) {
    if (!meta[i].active) continue;
    int64_t v = vals[meta[i].value_offset].value.load(std::memory_order_relaxed);
    // All metric types expose a single int64_t value in the mmap slot:
    //   COUNTER       - cumulative count
    //   GAUGE         - current value
    //   THROUGHPUT    - bytes/sec (the _rate_bps slot)
    //   WINDOWED_MAX  - peak sample in the window
    //   WINDOWED_AVG  - average sample in the window
    // emit meta[i].name, meta[i].type, v  ...
}
```

---

## Usage Examples

### Producer process

```cpp
#include <metrics/Metrics.h>
#include <thread>

int main() {
    auto registry = MetricsRegistry::create("/run/myapp/metrics.mmap");

    auto requests  = registry.register_counter("http_requests_total");
    auto errors    = registry.register_counter("http_errors_total");
    auto queue_len = registry.register_gauge("queue_length");
    auto bandwidth = registry.register_throughput("net_tx_bytes", 60);

    // --- worker loop ---
    while (true) {
        // simulate work
        requests.inc();
        queue_len.set(42);

        // record 1400 bytes sent (e.g. one Ethernet frame payload)
        bandwidth.record(1400);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

### Reader / exporter process

```cpp
#include <metrics/Metrics.h>
#include <cstdio>

int main() {
    auto registry = MetricsRegistry::open("/run/myapp/metrics.mmap");

    while (true) {
        uint32_t          n    = registry.metric_count();
        const MetricMeta* meta = registry.metadata();
        const ValueSlot*  vals = registry.values();

        for (uint32_t i = 0; i < n; ++i) {
            if (!meta[i].active) continue;
            int64_t v = vals[meta[i].value_offset].value.load(std::memory_order_relaxed);
            std::printf("# TYPE %s %s\n%s %ld\n",
                meta[i].name,
                meta[i].type == MetricType::COUNTER   ? "counter" :
                meta[i].type == MetricType::GAUGE      ? "gauge"   :
                meta[i].type == MetricType::THROUGHPUT ? "gauge"   : "untyped",
                meta[i].name, v);
        }
        std::this_thread::sleep_for(std::chrono::seconds(15));
    }
}
```

### Measuring network bandwidth

```cpp
#include <metrics/Metrics.h>

class NetworkSender {
public:
    explicit NetworkSender(MetricsRegistry& reg)
        : tx_(reg.register_throughput("net_tx_bytes", 60))
        , rx_(reg.register_throughput("net_rx_bytes", 60)) {}

    void send(const void* buf, size_t len) {
        // ... actual send syscall ...
        tx_.record(static_cast<int64_t>(len));
    }

    void recv(const void* buf, size_t len) {
        // ... actual recv syscall ...
        rx_.record(static_cast<int64_t>(len));
    }

    // Call periodically (e.g. from a stats thread) to guarantee freshness
    // even when traffic is very low.
    void flush_stats() {
        tx_.flush();
        rx_.flush();
    }

    // Read current bandwidth (bytes/sec) without touching the mmap file.
    int64_t tx_bps() const { return tx_.rate(); }
    int64_t rx_bps() const { return rx_.rate(); }

private:
    ThroughputMetric tx_;
    ThroughputMetric rx_;
};
```

A reader seeing the mmap file will find slots named `net_tx_bytes_rate_bps`,
`net_tx_bytes_total_bytes`, `net_rx_bytes_rate_bps`, and `net_rx_bytes_total_bytes`.

---

### 10 Hz sensor with 1 Hz exporter

The scenario that motivated `WindowedMaxGauge` and `WindowedAvgGauge`: a sensor
produces 10 samples per second but the Prometheus exporter scrapes once per second.
A plain `GaugeMetric` would show only the most recent sample, losing the other 9.
The windowed types retain all samples and expose the statistic the exporter cares
about.

```cpp
#include <metrics/Metrics.h>
#include <thread>
#include <cmath>

int main() {
    auto registry = MetricsRegistry::create("/run/myapp/metrics.mmap");

    // 10 s window: at 10 Hz that's 100 samples retained
    auto peak_temp  = registry.register_windowed_max("cpu_temp_max_mC",  10);
    auto avg_temp   = registry.register_windowed_avg("cpu_temp_avg_mC",  10);
    auto peak_rtt   = registry.register_windowed_max("rtt_max_us",       10);
    auto avg_rtt    = registry.register_windowed_avg("rtt_avg_us",       10);

    // Sensor loop at 10 Hz
    while (true) {
        int64_t temp_mC = read_cpu_temp_milli_celsius();  // e.g. 52'300
        int64_t rtt_us  = measure_rtt_microseconds();     // e.g. 420

        peak_temp.update(temp_mC);
        avg_temp.update(temp_mC);
        peak_rtt.update(rtt_us);
        avg_rtt.update(rtt_us);

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10 Hz
    }
    // Exporter (separate process, 1 Hz) reads peak_temp and avg_temp slots
    // and sees the true max / average across all 10 samples of the last 10 s.
}
```

**Sizing the ring buffer**

```
capacity ≥ sample_rate_hz × window_seconds
         = 10 Hz × 10 s = 100 samples          (default 1024 is ample)
```

If you later increase to 100 Hz or a 60-second window, pass an explicit capacity:

```cpp
auto metric = registry.register_windowed_max("name", 60, /*capacity=*/8192);
```

---

## Thread Safety

| Operation | Thread-safe? | Notes |
|---|---|---|
| `CounterMetric::inc` / `add` / `get` | Yes | Lock-free atomic |
| `GaugeMetric::set` / `inc` / `dec` / `add` / `sub` / `get` | Yes | Lock-free atomic |
| `ThroughputMetric::record` / `flush` / `rate` / `total` | Yes | Lock-free; CAS-based bucket reset |
| `WindowedMaxGauge::update` | Yes | Mutex-serialised write; lock-free `get()` |
| `WindowedAvgGauge::update` | Yes | Mutex-serialised write; lock-free `get()` |
| `MetricsRegistry::register_*` | **No** | Call only from a single thread at startup |
| `MetricsRegistry::open` / `create` | Yes (independent instances) | Each call returns a separate instance |

The metric update path (`record`, `inc`, `add`, etc.) uses `memory_order_relaxed`
throughout. This is intentional: metrics do not need strict ordering with application
logic. The trade-off is that a reader process may observe values that lag by one or
two CPU store-buffer flushes — which is entirely acceptable for monitoring.

`WindowedMaxGauge` and `WindowedAvgGauge` use a `std::mutex` to serialise concurrent
`update()` calls. The lock is held only during ring-buffer insertion and the linear
scan to compute max/avg (O(window_samples)). At 10 Hz with a 10-second window (100
samples) this is a sub-microsecond critical section.

---

## Memory Layout

```
Offset 0                      sizeof(MetricsHeader) = 64 B
├── MetricsHeader

Offset 64                     max_metrics × sizeof(MetricMeta) = max_metrics × 256 B
├── MetricMeta[0]
├── MetricMeta[1]
└── …

Offset 64 + max_metrics×256   max_metrics × sizeof(ValueSlot) = max_metrics × 64 B
├── ValueSlot[0]
├── ValueSlot[1]
└── …
```

Default total size with `max_metrics = 4096`:

```
64 + 4096×256 + 4096×64 = 64 + 1,048,576 + 262,144 ≈ 1.25 MB
```

---

## Build & Integration

### CMake (via Conan)

```bash
# From the repo root
./build.sh          # builds, packages, and uploads to Artifactory
```

### CMake consumer

```cmake
find_package(metrics REQUIRED)
target_link_libraries(my_target PRIVATE metrics)
```

### Conan consumer

```python
# conanfile.py
def requirements(self):
    self.requires("metrics/1.0.0@vradu/dev")
```

### Requirements

- C++23 compiler (GCC 13+, Clang 16+, MSVC 19.36+)
- POSIX platform (Linux, macOS) — uses `mmap`, `flock`, `ftruncate`
- No external runtime dependencies
