#include <benchmark/benchmark.h>

#include "metrics/MetricsLayout.h"

// Placeholder so the target builds; the real benchmarks arrive with the writer and reader.
static void BM_RequiredSlots(benchmark::State& state) {
    const metrics::CapacityPlan plan{10, 4, 3, 5, 6, 16};
    for (auto _ : state) {
        benchmark::DoNotOptimize(metrics::requiredSlots(plan));
    }
}
BENCHMARK(BM_RequiredSlots);
