# arcanum-metrics

A header-only C++23 library for process metrics in a memory-mapped file, modelled on Aeron's counters
(`CountersManager`). One process writes; any number of other processes (or threads) read, with no locks, no
threads, no allocation and no exceptions on any call. Time never comes from the library: every call that needs it
takes `nowNs` from the caller.

Version 2.0.0 is a clean break from 1.x (see [Migrating from 1.x](#migrating-from-1x)).

## Contents

- [Model](#model)
- [File layout](#file-layout)
- [Writer](#writer)
- [Reader](#reader)
- [Rates and windows](#rates-and-windows)
- [Prometheus text](#prometheus-text)
- [SystemMetrics](#systemmetrics)
- [Capacity](#capacity)
- [Using it from Rune](#using-it-from-rune)
- [Migrating from 1.x](#migrating-from-1x)
- [Build, test and benchmark](#build-test-and-benchmark)
- [Packaging](#packaging)

## Model

- A file holds a fixed number of **slots**. A slot is a metadata record (type id, kind, key, label, registration id)
  plus a value record (one `int64`).
- Only **totals and levels** are written. A `Counter` is a monotonic total (`add`); a `Gauge` is a level (`set`).
  Throughput (Δtotal/Δt) and windowed max/average are computed by the reader from samples it takes at its own period.
- **One writer per slot** for the slot's whole allocated life. The writer does a plain load of its own value, an
  add, and a release store (`std::atomic_ref<int64_t>::store(v, release)`, the same code as Aeron's
  `putLongOrdered`). There is no `fetch_add`, no compare-exchange and no mutex anywhere in the headers; a CMake
  check (`cmake/CheckNoAtomicRmw.cmake`) fails the configure if one appears.
- **One allocator per file**, on one owner thread. `allocate` and `free` never run concurrently with each other.
  A `SlotWriter` is handed to its writer thread before that thread starts (thread start gives happens-before), or
  over a ring.
- **Readers never write.** They use acquire loads and detect slot reuse by the registration id (`regId`).
- Capacity is fixed when the file is created. `allocate` past the end returns `Full`; nothing grows or aborts.

## File layout

Little-endian. `[header 4096 B][maxSlots x 512 B metadata][maxSlots x 128 B values]`. All offsets are in
`include/metrics/MetricsLayout.h` with a `static_assert` each.

| Header | offset | |
| --- | --- | --- |
| `layoutVersion` (i32) | 0 | written last with a release store; 0 means "being created" |
| pad | 4 | |
| `pid` (i64) | 8 | |
| `startEpochNs` (i64) | 16 | passed in by the caller |
| `maxSlots` (i32) | 24 | |
| `metadataRecordLength` (i32) | 28 | 512 |
| `valueRecordLength` (i32) | 32 | 128 |
| user tag | 36 | up to the end of the 4096-byte header (4060 bytes) |

| Metadata record | offset | |
| --- | --- | --- |
| `state` (i32) | 0 | `Unused = 0`, `Allocated = 1`, `Freed = -1` |
| `typeId` (i32) | 4 | |
| `regId` (i64) | 8 | |
| `ownerTag` (i64) | 16 | |
| `kind` (i32) | 24 | `Counter = 1`, `Gauge = 2` |
| `keyLength` (i32) | 28 | |
| key | 32 | 112 bytes |
| `labelLength` (i32) | 144 | |
| label | 148 | 364 bytes, ends at 512 |

A value record is an `i64` at offset 0, padded to 128 bytes (two cache lines) against false sharing.

Creating a file is `<path>.tmp.<pid>` (`O_CREAT|O_EXCL`), `ftruncate`, header with `layoutVersion = 0`, release store
of the version, `fsync`, `rename` over `<path>`. A reader that still maps the old file keeps reading its inode until
it re-opens; there is no `O_TRUNC` and no `flock`. A reader re-opens when `sameInstanceAs(path)` is false (the `pid`
or `startEpochNs` at the path differs from its mapping). A reader that opens a file being created polls the version
word a bounded number of times and then fails with `NotReady`; it never reads a half-written header.

## Writer

```cpp
#include <metrics/MetricsWriter.h>

metrics::MetricsFile file;
metrics::CreateParams params{path, maxSlots, metrics::requiredSlots(plan), pid, startEpochNs, userTag};
if (metrics::MetricsFile::create(params, file) != metrics::FileStatus::Ok) { /* refuse to start */ }

std::vector<int32_t> freeStack(maxSlots);            // storage for the free list, owned by the caller
metrics::MetricsWriter writer(file, freeStack);

auto r = writer.allocate(/*typeId*/ 1'000'201, metrics::MetricKind::Gauge, /*regId*/ -1, key, "label text");
if (r.status != metrics::AllocStatus::Ok) { /* fatal at startup: log the metric's name */ }
metrics::SlotWriter slot = r.writer;                 // trivially copyable; give it to exactly one writer thread
slot.add(1);                                         // Counter
slot.set(42);                                        // Gauge
(void)writer.free(slot.slot());                      // owner thread only
```

- `allocate` checks its arguments first (`InvalidType`: negative type id or unknown kind; `InvalidRegId`: below -1;
  `KeyTooLong` above 112 bytes; `LabelTooLong` above 364 bytes), then takes a free slot (LIFO) or the next unused
  one, or returns `Full`. A failed call changes nothing in the file.
- It zeroes the value (release), writes the regId (release), then sets the state to `Allocated` (release), so a new
  owner never inherits the old total. `regId = -1` takes the next number from the allocator's counter; an explicit
  regId (including a duplicate) is allowed, e.g. one regId for a block of slots.
- There is no reuse grace period: a freed slot is reused at once. Readers detect reuse by the regId, so no timing
  constant is involved.
- `add` wraps in two's complement across `INT64_MAX`.

## Reader

```cpp
#include <metrics/MetricsReader.h>

metrics::MetricsFile file;
if (metrics::MetricsFile::open(path, /*maxVersionPolls*/ 100, file) != metrics::FileStatus::Ok) { ... }
metrics::MetricsReader reader(file);

reader.forEachLive([](const metrics::Live& l) { /* l.typeId, l.kind, l.regId, l.value, l.key, l.label */ });
int32_t slot = reader.findFirst(typeId, key);        // -1 if none
```

`read(slot)` returns `SlotView = std::variant<Empty, Live, Reused>`. It reads the state, the regId, the value, then
the state and the regId again (acquire each). A slot freed while it was being read is `Empty`; one reallocated
while it was being read is `Reused{slot, oldRegId}`, never `Live` with an old regId and a new owner's value.
`forEachLive` reads a `Reused` slot once more. The `key` and `label` views point into the mapping and are valid
until the slot is reused; read the slot again to find out.

## Rates and windows

`RateTracker<Capacity, Window>` and `WindowSampler<Capacity, Window>` keep a fixed array with one entry per slot
(no allocation). An entry restarts when its regId changes or its slot is freed, so a rate never spans two owners.

```cpp
metrics::RateTracker<4096, 60> rates;
rates.sample(reader, nowNs);                         // call at your own period
std::optional<double> perSecond = rates.perSecond(slot);   // none until two samples, none for a reused slot

metrics::WindowSampler<4096, 60> window;
window.sample(reader, nowNs);
std::optional<int64_t> peak = window.max(slot);      // max of the last Window sampled levels
std::optional<double> mean = window.avg(slot);       // mean of the last Window samples
```

A windowed max is the max of the *sampled* levels. A writer that needs an exact peak keeps its own `Gauge` high-water
mark and resets it on its own schedule.

## Prometheus text

`metrics::PrometheusText::render(reader, rates, nowNs, std::span<char> out)` samples the tracker and writes the text
into the caller's buffer. It returns the length the text needs; when that is larger than the buffer, nothing is
written, so retry with a larger buffer. It opens no sockets and allocates nothing.

```
# TYPE metrics_counter counter
metrics_counter{type="1000201",slot="0",label="node=\"1\""} 3500
# TYPE metrics_gauge gauge
metrics_gauge{type="5",slot="1",label="..."} -7
# TYPE metrics_rate_per_second gauge
metrics_rate_per_second{type="1000201",slot="0",label="node=\"1\""} 2500
```

The label text is emitted as one quoted label value (backslash, quote and newline escaped).

## SystemMetrics

`metrics::SystemMetrics` publishes `process_open_fds`, `process_threads`, `process_resident_memory_bytes` and
`process_virtual_memory_bytes` gauges, and a `process_metrics_read_errors_total` counter. It starts no thread: the
owner calls `doWork(nowNs)` on its duty cycle, and the call returns 0 at once until `intervalNs` has passed on that
clock. `statm`, `status` and the `fd` directory are opened once in `init` and re-read with `pread` and `getdents64`
into fixed buffers. A source that fails to read keeps its last value and counts one read error. `procRoot` is
injectable (`"/proc/self"`) for tests. Type ids are `1'000'301..1'000'305`.

## Capacity

`requiredSlots(plan)` returns an `int64_t`:

```
requiredSlots = nodeMetrics + maxNamespaces * (perThreadNsMetrics * threads + perNodeNsMetrics) + headroom
```

`headroom` (Rune uses 16) covers `SystemMetrics` and future node metrics. The product can pass `INT32_MAX`; the caller
checks. `MetricsFile::create` refuses with `CapacityTooLarge` above `INT32_MAX` and `CapacityTooSmall` when
`maxSlots < requiredSlots`, so a node refuses to start instead of dropping a metric later.

## Using it from Rune

Rune owns one file per node (`RUNE_METRICS_FILE`, default `${RUNE_DATA_DIR}/metrics/rune-metrics.dat`, directory mode
0750, file mode 0640). Every node metric is allocated before any agent thread starts, so each writer receives a valid
`SlotWriter`; an allocation failure aborts the start with the metric's name. The control agent calls
`SystemMetrics::doWork(nowNs)`. `rune_role` and the test rigs are readers: a missing file is an error, never a silent
fallback.

## Migrating from 1.x

2.0.0 removes the 1.x API (`MetricsRegistry`, `CounterMetric`, `GaugeMetric`, `ThroughputMetric`,
`WindowedMaxGauge`, `WindowedAvgGauge`, and the `SystemMetrics` thread). A v1 file is refused with `VersionMismatch`.

| 1.x | 2.0.0 |
| --- | --- |
| `MetricsRegistry::create(path, n)` (`O_TRUNC`, `flock` wait loop, throws) | `MetricsFile::create(params, out)` (tmp + rename, status code) |
| `MetricsRegistry::open(path)` | `MetricsFile::open(path, polls, out)` + `MetricsReader` |
| `register_counter(name)` / `register_gauge(name)` | `MetricsWriter::allocate(typeId, kind, regId, key, label)` |
| `CounterMetric::increment` (`fetch_add`) | `SlotWriter::add` (load, add, release store) |
| `GaugeMetric::set` | `SlotWriter::set` |
| `ThroughputMetric`, `WindowedMaxGauge`, `WindowedAvgGauge` (writer-side, own clock) | `RateTracker`, `WindowSampler` (reader-side, caller's `nowNs`) |
| `SystemMetrics` with its own thread | `SystemMetrics::doWork(nowNs)` on the owner's duty cycle |
| metric names with labels in a string | `typeId` + `key` bytes + `label` text |
| exporter reading the registry | `PrometheusText::render` |

## Build, test and benchmark

```
cmake -S . -B build -G Ninja -DMETRICS_BUILD_TESTS=ON -DMETRICS_BUILD_BENCH=ON
cmake --build build
ctest --test-dir build --output-on-failure -j8
# sanitizers
cmake -S . -B build-asan -G Ninja -DMETRICS_BUILD_TESTS=ON -DMETRICS_SANITIZE=address,undefined
cmake -S . -B build-tsan -G Ninja -DMETRICS_BUILD_TESTS=ON -DMETRICS_SANITIZE=thread
```

CMake 3.31 or newer; googletest 1.16.0 and Google Benchmark 1.8.2 are fetched. Every test is a `*Regression*`
test. The tests advance an injected clock and never sleep.

Benchmark (`bench/MetricsBench.cpp`, Release, gcc, one shared host with load average about 5, so read the
numbers as upper bounds):

| Benchmark | Time |
| --- | --- |
| `SlotWriter::add` | about 0.45 ns (a single store; the loop is hoisted) |
| `SlotWriter::set` | about 0.47 ns |
| `MetricsReader::read` | about 12 ns |
| `forEachLive` over 4096 slots | about 50 us |
| `RateTracker::sample` over 4096 slots | about 66 us |

## Packaging

CI builds and tests the library (gcc-13 and clang-18, Debug, ASan+UBSan and TSan, plus a Release bench smoke). CI
publishes the package; there are no local conan steps.
