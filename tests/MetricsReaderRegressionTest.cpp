#include "AllocCounter.h"
#include "TestDir.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"

#include <gtest/gtest.h>

#include <climits>
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
    explicit Env(int32_t maxSlots) : stack(static_cast<size_t>(maxSlots) + 1) {
        const std::string p = dir.path("r.dat");
        CreateParams cp{p.c_str(), maxSlots, maxSlots, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
    }
};

std::vector<std::byte> bytes(size_t n, uint8_t v) { return std::vector<std::byte>(n, std::byte{v}); }

// One-shot reader hook: at the chosen step, free + reallocate the slot on the writer.
struct ReuseState {
    MetricsWriter* writer = nullptr;
    ReadStep when = ReadStep::AfterFirstRegId;
    bool fired = false;
};
struct ReuseHook {
    ReuseState* st;
    void operator()(ReadStep step, int32_t slot) const noexcept {
        if (st->fired || step != st->when) return;
        st->fired = true;
        (void)st->writer->free(slot);
        auto r = st->writer->allocate(1, MetricKind::Counter, -1, {}, "new");
        r.writer.set(777);
    }
};

}  // namespace

TEST(MetricsReaderRegression, ReadStatesTable) {
    Env env(4);
    MetricsWriter w(env.file, env.stack);
    const auto key = bytes(4, 0x11);
    auto a = w.allocate(42, MetricKind::Gauge, 9, key, "lab");
    a.writer.set(-5);
    auto b = w.allocate(43, MetricKind::Counter, -1, {}, "");
    ASSERT_EQ(w.free(b.writer.slot()), FreeStatus::Ok);

    MetricsReader r(env.file);
    const SlotView v0 = r.read(0);
    ASSERT_TRUE(std::holds_alternative<Live>(v0));
    const Live& l = std::get<Live>(v0);
    EXPECT_EQ(l.slot, 0);
    EXPECT_EQ(l.typeId, 42);
    EXPECT_EQ(l.kind, MetricKind::Gauge);
    EXPECT_EQ(l.regId, 9);
    EXPECT_EQ(l.value, -5);
    ASSERT_EQ(l.key.size(), 4U);
    EXPECT_EQ(l.key[0], std::byte{0x11});
    EXPECT_EQ(l.label, "lab");
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(1)));  // freed
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(2)));  // unused
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(-1)));
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(4)));
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(INT32_MAX)));
}

TEST(MetricsReaderRegression, ReuseMidReadDetectedRegression) {
    for (ReadStep when : {ReadStep::AfterFirstRegId, ReadStep::AfterValue}) {
        Env env(2);
        MetricsWriter w(env.file, env.stack);
        auto a = w.allocate(1, MetricKind::Counter, -1, {}, "old");
        a.writer.set(5);
        ReuseState st{&w, when, false};
        BasicMetricsReader<ReuseHook> r(env.file, ReuseHook{&st});
        const SlotView v = r.read(0);
        EXPECT_TRUE(st.fired);
        ASSERT_TRUE(std::holds_alternative<Reused>(v)) << "step " << static_cast<int>(when);
        EXPECT_EQ(std::get<Reused>(v).oldRegId, a.regId);
        EXPECT_EQ(std::get<Reused>(v).slot, 0);
        // The next read sees the new owner with its own value.
        const SlotView v2 = r.read(0);
        ASSERT_TRUE(std::holds_alternative<Live>(v2));
        EXPECT_NE(std::get<Live>(v2).regId, a.regId);
        EXPECT_EQ(std::get<Live>(v2).value, 777);
    }
}

TEST(MetricsReaderRegression, ForEachLiveRetriesReusedOnceRegression) {
    Env env(2);
    MetricsWriter w(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "old");
    a.writer.set(5);
    ReuseState st{&w, ReadStep::AfterValue, false};
    BasicMetricsReader<ReuseHook> r(env.file, ReuseHook{&st});
    int seen = 0;
    int64_t value = 0;
    r.forEachLive([&](const Live& l) { ++seen; value = l.value; });
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(value, 777);
}

TEST(MetricsReaderRegression, FindFirstByTypeAndKeyRegression) {
    Env env(5);
    MetricsWriter w(env.file, env.stack);
    const auto k1 = bytes(4, 1), k2 = bytes(4, 2), k3 = bytes(3, 2);
    auto a = w.allocate(7, MetricKind::Counter, -1, k1, "a");
    auto b = w.allocate(7, MetricKind::Counter, -1, k2, "b");
    auto c = w.allocate(8, MetricKind::Counter, -1, k2, "c");
    MetricsReader r(env.file);
    EXPECT_EQ(r.findFirst(7, k1), a.writer.slot());
    EXPECT_EQ(r.findFirst(7, k2), b.writer.slot());
    EXPECT_EQ(r.findFirst(8, k2), c.writer.slot());
    EXPECT_EQ(r.findFirst(7, k3), -1);
    EXPECT_EQ(r.findFirst(9, k1), -1);
    ASSERT_EQ(w.free(b.writer.slot()), FreeStatus::Ok);
    EXPECT_EQ(r.findFirst(7, k2), -1);  // a freed match is skipped
    auto d = w.allocate(7, MetricKind::Counter, -1, k2, "d");
    EXPECT_EQ(r.findFirst(7, k2), d.writer.slot());
}

TEST(RateTrackerRegression, RateFromTotalsRegression) {
    Env env(2);
    MetricsWriter w(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = std::make_unique<RateTracker<2, 4>>();
    constexpr int64_t S = 1'000'000'000;
    a.writer.set(0);
    rt->sample(r, 0);
    EXPECT_FALSE(rt->perSecond(0).has_value());
    a.writer.set(1000);
    rt->sample(r, S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    EXPECT_DOUBLE_EQ(*rt->perSecond(0), 1000.0);
    a.writer.set(3000);
    rt->sample(r, 2 * S);
    EXPECT_DOUBLE_EQ(*rt->perSecond(0), 2000.0);
    EXPECT_FALSE(rt->perSecond(1).has_value());   // unused slot
    EXPECT_FALSE(rt->perSecond(-1).has_value());
    EXPECT_FALSE(rt->perSecond(2).has_value());
    // A zero or backwards step in time gives no rate rather than a divide by zero.
    a.writer.set(4000);
    rt->sample(r, 2 * S);
    EXPECT_FALSE(rt->perSecond(0).has_value());
}

TEST(RateTrackerRegression, RestartOnReuseRegression) {
    Env env(1);
    MetricsWriter w(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = std::make_unique<RateTracker<1, 4>>();
    constexpr int64_t S = 1'000'000'000;
    a.writer.set(1'000'000);
    rt->sample(r, 0);
    a.writer.set(2'000'000);
    rt->sample(r, S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    auto b = w.allocate(1, MetricKind::Counter, -1, {}, "b");
    b.writer.set(10);
    rt->sample(r, 2 * S);
    EXPECT_FALSE(rt->perSecond(0).has_value());  // never 10 - 2'000'000
    b.writer.set(510);
    rt->sample(r, 3 * S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    EXPECT_DOUBLE_EQ(*rt->perSecond(0), 500.0);
    // A freed slot drops the entry.
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    rt->sample(r, 4 * S);
    EXPECT_FALSE(rt->perSecond(0).has_value());
}

TEST(RateTrackerRegression, WrapIsOneDeltaRegression) {
    Env env(1);
    MetricsWriter w(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = std::make_unique<RateTracker<1, 4>>();
    constexpr int64_t S = 1'000'000'000;
    a.writer.set(INT64_MAX - 499);
    rt->sample(r, 0);
    a.writer.add(1000);  // wraps past INT64_MAX
    rt->sample(r, S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    EXPECT_DOUBLE_EQ(*rt->perSecond(0), 1000.0);
}

TEST(WindowSamplerRegression, MaxAvgTable) {
    Env env(2);
    MetricsWriter w(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Gauge, -1, {}, "a");
    MetricsReader r(env.file);
    auto ws = std::make_unique<WindowSampler<2, 4>>();
    EXPECT_FALSE(ws->max(0).has_value());
    EXPECT_FALSE(ws->avg(0).has_value());

    struct Step { int64_t ns; int64_t level; int64_t max; double avg; };
    // Non-uniform injected clock: the result depends on samples only, never on the spacing.
    const Step steps[] = {
        {0, 5, 5, 5.0},
        {10, 1, 5, 3.0},
        {1'000'000, 9, 9, 5.0},
        {1'000'001, 2, 9, 4.25},   // window full: 5 1 9 2
        {7'000'000'000, 2, 9, 3.5},  // rolls: 1 9 2 2
        {7'000'000'001, 0, 9, 3.25},  // 9 2 2 0
        {7'000'000'002, 0, 2, 1.0},   // 2 2 0 0
        {7'000'000'003, -3, 2, -0.25},  // 2 0 0 -3
        {7'000'000'004, -3, 0, -1.5},   // 0 0 -3 -3
        {7'000'000'005, -3, 0, -2.25},  // 0 -3 -3 -3
        {7'000'000'006, -3, -3, -3.0},  // -3 -3 -3 -3
    };
    for (const Step& s : steps) {
        a.writer.set(s.level);
        ws->sample(r, s.ns);
        ASSERT_TRUE(ws->max(0).has_value());
        EXPECT_EQ(*ws->max(0), s.max) << "ns " << s.ns;
        EXPECT_DOUBLE_EQ(*ws->avg(0), s.avg) << "ns " << s.ns;
    }
    // After a reuse the window restarts for the new owner.
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    auto b = w.allocate(1, MetricKind::Gauge, -1, {}, "b");
    b.writer.set(7);
    ws->sample(r, 8'000'000'000);
    EXPECT_EQ(*ws->max(0), 7);
    EXPECT_DOUBLE_EQ(*ws->avg(0), 7.0);
    // A freed slot drops the entry.
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    ws->sample(r, 9'000'000'000);
    EXPECT_FALSE(ws->max(0).has_value());
    EXPECT_FALSE(ws->max(-1).has_value());
    EXPECT_FALSE(ws->avg(2).has_value());
}

TEST(MetricsReaderRegression, ZeroAllocScanRegression) {
    Env env(8);
    MetricsWriter w(env.file, env.stack);
    for (int i = 0; i < 5; ++i) w.allocate(i + 1, MetricKind::Counter, -1, bytes(4, 3), "x").writer.set(i);
    MetricsReader r(env.file);
    auto rt = std::make_unique<RateTracker<8, 4>>();
    auto ws = std::make_unique<WindowSampler<8, 4>>();
    int seen = 0;
    int32_t found = 0;
    const auto probeKey = bytes(4, 3);
    metrics::testing::AllocScope scope;
    for (int64_t t = 0; t < 10; ++t) {
        r.forEachLive([&](const Live&) { ++seen; });
        rt->sample(r, t * 1'000'000'000);
        ws->sample(r, t * 1'000'000'000);
        (void)rt->perSecond(1);
        (void)ws->max(1);
        (void)ws->avg(1);
        found += r.findFirst(3, probeKey);
    }
    EXPECT_EQ(scope.count(), 0);
    EXPECT_EQ(seen, 50);
    (void)found;
}
