#include "AllocCounter.h"
#include "TestDir.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"
#include "metrics/PrometheusText.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace metrics;

namespace {
struct Env {
    metrics::testing::TestDir dir;
    MetricsFile file;
    std::vector<int32_t> stack;
    explicit Env(int32_t n) : stack(static_cast<size_t>(n) + 1) {
        const std::string p = dir.path("p.dat");
        CreateParams cp{p.c_str(), n, n, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
    }
};
constexpr int64_t S = 1'000'000'000;
}  // namespace

TEST(PrometheusTextRegression, RenderTable) {
    Env env(8);
    MetricsWriter w(env.file, env.stack);
    auto c = w.allocate(1000201, MetricKind::Counter, -1, {}, "node=\"1\"");
    auto g = w.allocate(5, MetricKind::Gauge, -1, {}, "a\\b\nc");
    auto c2 = w.allocate(7, MetricKind::Counter, -1, {}, "");
    auto freed = w.allocate(9, MetricKind::Gauge, -1, {}, "gone");
    ASSERT_EQ(w.free(freed.writer.slot()), FreeStatus::Ok);
    c.writer.set(1000);
    g.writer.set(-7);
    c2.writer.set(3);
    MetricsReader r(env.file);
    auto rt = std::make_unique<RateTracker<8, 4>>();

    char buf[4096];
    // First render: no rate yet (one sample).
    size_t n = PrometheusText::render(r, *rt, 0, buf);
    const std::string first =
        "# TYPE metrics_counter counter\n"
        "metrics_counter{type=\"1000201\",slot=\"0\",label=\"node=\\\"1\\\"\"} 1000\n"
        "metrics_counter{type=\"7\",slot=\"2\",label=\"\"} 3\n"
        "# TYPE metrics_gauge gauge\n"
        "metrics_gauge{type=\"5\",slot=\"1\",label=\"a\\\\b\\nc\"} -7\n"
        "# TYPE metrics_rate_per_second gauge\n";
    EXPECT_EQ(std::string(buf, n), first);

    // Second render one second later: counters get a rate, gauges do not.
    c.writer.set(3500);
    c2.writer.set(3);
    n = PrometheusText::render(r, *rt, S, buf);
    const std::string second =
        "# TYPE metrics_counter counter\n"
        "metrics_counter{type=\"1000201\",slot=\"0\",label=\"node=\\\"1\\\"\"} 3500\n"
        "metrics_counter{type=\"7\",slot=\"2\",label=\"\"} 3\n"
        "# TYPE metrics_gauge gauge\n"
        "metrics_gauge{type=\"5\",slot=\"1\",label=\"a\\\\b\\nc\"} -7\n"
        "# TYPE metrics_rate_per_second gauge\n"
        "metrics_rate_per_second{type=\"1000201\",slot=\"0\",label=\"node=\\\"1\\\"\"} 2500\n"
        "metrics_rate_per_second{type=\"7\",slot=\"2\",label=\"\"} 0\n";
    EXPECT_EQ(std::string(buf, n), second);

    // A buffer one byte too small returns the needed length and writes nothing.
    const size_t need = second.size();
    {
        auto rt2 = std::make_unique<RateTracker<8, 4>>();
        (void)PrometheusText::render(r, *rt2, 0, buf);
        c.writer.set(6000);  // same text length as 3500, rate 2500 again
        std::vector<char> small(need - 1, '#');
        n = PrometheusText::render(r, *rt2, S, std::span<char>(small));
        EXPECT_EQ(n, need);
        for (char ch : small) ASSERT_EQ(ch, '#');
        c.writer.set(8500);
        std::vector<char> exact(need, '#');
        n = PrometheusText::render(r, *rt2, 2 * S, std::span<char>(exact));
        EXPECT_EQ(n, need);
        EXPECT_EQ(std::string(exact.data(), 22), "# TYPE metrics_counter");
        EXPECT_EQ(exact.back(), '\n');
    }
    // An empty buffer too.
    n = PrometheusText::render(r, *rt, 3 * S, std::span<char>());
    EXPECT_GT(n, 0U);
}

TEST(PrometheusTextRegression, ZeroAllocRenderRegression) {
    Env env(4);
    MetricsWriter w(env.file, env.stack);
    w.allocate(1, MetricKind::Counter, -1, {}, "x").writer.set(5);
    w.allocate(2, MetricKind::Gauge, -1, {}, "y").writer.set(6);
    MetricsReader r(env.file);
    auto rt = std::make_unique<RateTracker<4, 4>>();
    char buf[1024];
    char tiny[8];
    metrics::testing::AllocScope scope;
    for (int i = 0; i < 5; ++i) {
        (void)PrometheusText::render(r, *rt, i * S, buf);
        (void)PrometheusText::render(r, *rt, i * S + 1, tiny);
    }
    EXPECT_EQ(scope.count(), 0);
}
