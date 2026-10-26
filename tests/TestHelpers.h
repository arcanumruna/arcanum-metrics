#pragma once
// Factories that assert success, so tests read like constructors.
#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace metrics::testing {

template <class Hook = NoHook>
BasicMetricsWriter<Hook> makeWriter(MetricsFile& file, std::vector<int32_t>& stack, Hook hook = Hook{}) {
    BasicMetricsWriter<Hook> w;
    EXPECT_EQ(BasicMetricsWriter<Hook>::create(file, std::span<int32_t>(stack), w, hook), WriterStatus::Ok);
    return w;
}

template <int32_t C, int32_t W, class Reader>
std::unique_ptr<RateTracker<C, W>> makeRates(const Reader& r) {
    auto p = std::make_unique<RateTracker<C, W>>();
    EXPECT_EQ(p->bind(r), TrackerStatus::Ok);
    return p;
}

template <int32_t C, int32_t W, class Reader>
std::unique_ptr<WindowSampler<C, W>> makeWindow(const Reader& r) {
    auto p = std::make_unique<WindowSampler<C, W>>();
    EXPECT_EQ(p->bind(r), TrackerStatus::Ok);
    return p;
}

}  // namespace metrics::testing
