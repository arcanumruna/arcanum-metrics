#include <benchmark/benchmark.h>

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr int32_t kSlots = 4096;

// A file in a private temp directory with kSlots counters allocated.
struct Fixture {
    std::string dir;
    std::string path;
    metrics::MetricsFile file;
    std::vector<int32_t> stack = std::vector<int32_t>(kSlots + 1);
    std::unique_ptr<metrics::MetricsWriter> writer;
    metrics::SlotWriter first;

    Fixture() {
        char tmpl[] = "/tmp/metrics_bench_XXXXXX";
        dir = ::mkdtemp(tmpl);
        path = dir + "/b.dat";
        metrics::CreateParams cp{path.c_str(), kSlots, kSlots, 1, 1, {}};
        if (metrics::MetricsFile::create(cp, file) != metrics::FileStatus::Ok) std::abort();
        writer = std::make_unique<metrics::MetricsWriter>();
        if (metrics::MetricsWriter::create(file, std::span<int32_t>(stack), *writer) != metrics::WriterStatus::Ok) std::abort();
        for (int32_t i = 0; i < kSlots; ++i) {
            const auto r = writer->allocate(1000 + i, metrics::MetricKind::Counter, -1, {}, "bench");
            if (i == 0) first = r.writer;
            r.writer.set(i);
        }
    }
    ~Fixture() {
        ::unlink(path.c_str());
        ::rmdir(dir.c_str());
    }
};

void BM_SlotWriterAdd(benchmark::State& state) {
    Fixture f;
    for (auto _ : state) f.first.add(1);
}
BENCHMARK(BM_SlotWriterAdd);

void BM_SlotWriterSet(benchmark::State& state) {
    Fixture f;
    int64_t v = 0;
    for (auto _ : state) f.first.set(++v);
}
BENCHMARK(BM_SlotWriterSet);

void BM_ReaderRead(benchmark::State& state) {
    Fixture f;
    metrics::MetricsReader r(f.file);
    int32_t s = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(r.read(s));
        s = (s + 1) & (kSlots - 1);
    }
}
BENCHMARK(BM_ReaderRead);

void BM_ForEachLive4096(benchmark::State& state) {
    Fixture f;
    metrics::MetricsReader r(f.file);
    for (auto _ : state) {
        int64_t sum = 0;
        r.forEachLive([&](const metrics::Live& l) { sum += l.value; });
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_ForEachLive4096);

void BM_RateTrackerSample4096(benchmark::State& state) {
    Fixture f;
    metrics::MetricsReader r(f.file);
    auto rt = std::make_unique<metrics::RateTracker<kSlots, 4>>();
    if (rt->bind(r) != metrics::TrackerStatus::Ok) std::abort();
    int64_t now = 0;
    for (auto _ : state) {
        f.first.add(1);
        now += 1'000'000'000;
        rt->sample(r, now);
    }
}
BENCHMARK(BM_RateTrackerSample4096);

}  // namespace
