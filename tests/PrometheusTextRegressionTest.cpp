#include "AllocCounter.h"
#include "TestDir.h"
#include "TestHelpers.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"
#include "metrics/PrometheusText.h"

#include <gtest/gtest.h>

#include <algorithm>
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

namespace {
const char* kCounterHead =
    "# HELP metrics_counter Monotonic total of a slot, by type id, registration id and label.\n"
    "# TYPE metrics_counter counter\n";
const char* kGaugeHead =
    "# HELP metrics_gauge Level of a slot, by type id, registration id and label.\n"
    "# TYPE metrics_gauge gauge\n";
const char* kRateHead =
    "# HELP metrics_rate_per_second Per-second rate of a counter between the reader's last two samples.\n"
    "# TYPE metrics_rate_per_second gauge\n";
}  // namespace

TEST(PrometheusTextRegression, RenderTable) {
    Env env(8);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto c = w.allocate(1000201, MetricKind::Counter, -1, {}, "node=\"1\"");   // regId 1
    auto g = w.allocate(5, MetricKind::Gauge, -1, {}, "a\\b\nc");              // regId 2
    auto c2 = w.allocate(7, MetricKind::Counter, -1, {}, "");                    // regId 3
    auto freed = w.allocate(9, MetricKind::Gauge, -1, {}, "gone");
    ASSERT_EQ(w.free(freed.writer.slot()), FreeStatus::Ok);
    c.writer.set(1000);
    g.writer.set(-7);
    c2.writer.set(3);
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<8, 4>(r);

    char buf[4096];
    // First scrape: one sample, so no rate yet. The caller samples; render does not.
    rt->sample(r, 0);
    RenderResult res = PrometheusText::render(r, *rt, buf);
    const std::string first = std::string(kCounterHead) +
        "metrics_counter{type=\"1000201\",reg_id=\"1\",label=\"node=\\\"1\\\"\"} 1000\n"
        "metrics_counter{type=\"7\",reg_id=\"3\",label=\"\"} 3\n" + kGaugeHead +
        "metrics_gauge{type=\"5\",reg_id=\"2\",label=\"a\\\\b\\nc\"} -7\n" + kRateHead;
    EXPECT_FALSE(res.overflow);
    EXPECT_EQ(std::string(buf, res.length), first);

    // Render alone never samples: a second render gives the same text.
    c.writer.set(3500);
    rt->sample(r, S);
    res = PrometheusText::render(r, *rt, buf);
    const std::string second = std::string(kCounterHead) +
        "metrics_counter{type=\"1000201\",reg_id=\"1\",label=\"node=\\\"1\\\"\"} 3500\n"
        "metrics_counter{type=\"7\",reg_id=\"3\",label=\"\"} 3\n" + kGaugeHead +
        "metrics_gauge{type=\"5\",reg_id=\"2\",label=\"a\\\\b\\nc\"} -7\n" + kRateHead +
        "metrics_rate_per_second{type=\"1000201\",reg_id=\"1\",label=\"node=\\\"1\\\"\"} 2500\n"
        "metrics_rate_per_second{type=\"7\",reg_id=\"3\",label=\"\"} 0\n";
    EXPECT_EQ(std::string(buf, res.length), second);
    res = PrometheusText::render(r, *rt, buf);
    EXPECT_EQ(std::string(buf, res.length), second);

    // A buffer one byte too small overflows; the length is what was written and never exceeds the buffer.
    const size_t need = second.size();
    std::vector<char> small(need - 1, '#');
    res = PrometheusText::render(r, *rt, std::span<char>(small));
    EXPECT_TRUE(res.overflow);
    EXPECT_LE(res.length, small.size());
    std::vector<char> exact(need, '#');
    res = PrometheusText::render(r, *rt, std::span<char>(exact));
    EXPECT_FALSE(res.overflow);
    EXPECT_EQ(res.length, need);
    EXPECT_EQ(std::string(exact.data(), res.length), second);
    // An empty buffer too.
    res = PrometheusText::render(r, *rt, std::span<char>());
    EXPECT_TRUE(res.overflow);
    EXPECT_EQ(res.length, 0U);
}

// A rate belongs to a regId: after the slot is reused, the new owner shows no rate until it is sampled twice.
TEST(PrometheusTextRegression, RateAfterReuseRegression) {
    Env env(2);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "old");
    a.writer.set(100);
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<2, 4>(r);
    rt->sample(r, 0);
    a.writer.set(1100);
    rt->sample(r, S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    auto b = w.allocate(1, MetricKind::Counter, -1, {}, "new");
    b.writer.set(5);
    char buf[2048];
    const RenderResult res = PrometheusText::render(r, *rt, buf);  // no sample since the reuse
    const std::string text(buf, res.length);
    EXPECT_NE(text.find("label=\"new\"} 5\n"), std::string::npos) << text;
    EXPECT_EQ(text.find("metrics_rate_per_second{"), std::string::npos) << "a rate printed for the new owner: " << text;
    EXPECT_FALSE(rt->perSecond(0, b.regId).has_value());
}

namespace {
// Allocates a new counter during the N-th slot read of a render.
struct GrowState { MetricsWriter* w; int reads = 0; int fireAt = 0; bool fired = false; };
struct GrowHook {
    GrowState* st;
    void operator()(ReadStep step, int32_t) const noexcept {
        if (step != ReadStep::AfterValue) return;
        if (++st->reads == st->fireAt && !st->fired) {
            st->fired = true;
            (void)st->w->allocate(2, MetricKind::Counter, -1, {}, "late");
        }
    }
};
}  // namespace

TEST(PrometheusTextRegression, PrometheusSinglePassRegression) {
    for (int fireAt = 1; fireAt <= 8; ++fireAt) {
        for (size_t cap : {size_t{0}, size_t{40}, size_t{300}, size_t{700}, size_t{4096}}) {
            Env env(8);
            MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
            w.allocate(1, MetricKind::Counter, -1, {}, "x").writer.set(5);
            MetricsReader plain(env.file);
            auto rt = metrics::testing::makeRates<8, 4>(plain);
            rt->sample(plain, 0);
            rt->sample(plain, S);
            GrowState st{&w, 0, fireAt, false};
            BasicMetricsReader<GrowHook> r(env.file, GrowHook{&st});
            std::vector<char> buf(cap, '#');
            const RenderResult res = PrometheusText::render(r, *rt, std::span<char>(buf));
            // Never a length larger than the buffer, whatever changed mid-render.
            ASSERT_LE(res.length, buf.size()) << "fireAt " << fireAt << " cap " << cap;
            if (!res.overflow) {
                const std::string text(buf.data(), res.length);
                ASSERT_FALSE(text.empty());
                EXPECT_EQ(text.back(), '\n');
                EXPECT_NE(text.find(kRateHead), std::string::npos) << text;  // consistent: all three families
                EXPECT_NE(text.find(kCounterHead), std::string::npos);
                EXPECT_NE(text.find(kGaugeHead), std::string::npos);
            }
        }
    }
}

TEST(PrometheusTextRegression, ZeroAllocRenderRegression) {
    Env env(4);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    w.allocate(1, MetricKind::Counter, -1, {}, "x").writer.set(5);
    w.allocate(2, MetricKind::Gauge, -1, {}, "y").writer.set(6);
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<4, 4>(r);
    char buf[1024];
    char tiny[8];
    metrics::testing::AllocScope scope;
    for (int i = 0; i < 5; ++i) {
        rt->sample(r, i * S);
        (void)PrometheusText::render(r, *rt, buf);
        (void)PrometheusText::render(r, *rt, tiny);
    }
    EXPECT_EQ(scope.count(), 0);
}

// putEscaped writes runs of plain characters in one put: escapes at the start, the end, back to back and alone
// must give the same text as one escape per character, and a cut buffer at every length gives a prefix.
TEST(PrometheusTextRegression, EscapeRunsTableRegression) {
    struct Case {
        const char* label;
        const char* escaped;
    };
    const Case cases[] = {
        {"", ""},
        {"plain", "plain"},
        {"\"lead", "\\\"lead"},
        {"trail\\", "trail\\\\"},
        {"\n", "\\n"},
        {"\\\"\n", "\\\\\\\"\\n"},
        {"a\"\"b", "a\\\"\\\"b"},
        {"x\ny\\z\"", "x\\ny\\\\z\\\""},
    };
    for (const Case& k : cases) {
        SCOPED_TRACE(k.escaped);
        Env env(2);
        MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
        auto g = w.allocate(5, MetricKind::Gauge, -1, {}, k.label);
        ASSERT_EQ(g.status, AllocStatus::Ok);
        g.writer.set(4);
        MetricsReader r(env.file);
        auto rt = metrics::testing::makeRates<2, 4>(r);
        rt->sample(r, 0);
        char buf[1024];
        const RenderResult res = PrometheusText::render(r, *rt, buf);
        ASSERT_FALSE(res.overflow);
        const std::string full(buf, res.length);
        const std::string want = std::string(kCounterHead) + kGaugeHead + "metrics_gauge{type=\"5\",reg_id=\"1\",label=\"" +
                                 k.escaped + "\"} 4\n" + kRateHead;
        EXPECT_EQ(full, want);
        for (size_t n = 0; n < full.size(); ++n) {
            std::vector<char> cut(n + 1, '#');
            const RenderResult c = PrometheusText::render(r, *rt, std::span<char>(cut.data(), n));
            ASSERT_TRUE(c.overflow) << n;
            ASSERT_LE(c.length, n);
            ASSERT_EQ(std::string(cut.data(), c.length), full.substr(0, c.length)) << n;
            ASSERT_EQ(cut[n], '#') << n;  // never writes past the span
        }
    }
}
