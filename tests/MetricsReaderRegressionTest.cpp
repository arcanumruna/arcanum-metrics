#include "AllocCounter.h"
#include "TestDir.h"
#include "TestHelpers.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    const auto key = bytes(4, 0x11);
    auto a = w.allocate(42, MetricKind::Gauge, 9, key, "lab");  // 9 is the owner tag
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
    EXPECT_EQ(l.regId, a.regId);
    EXPECT_EQ(l.ownerTag, 9);
    EXPECT_EQ(l.value, -5);
    ASSERT_EQ(l.key().size(), 4U);
    EXPECT_EQ(l.key()[0], std::byte{0x11});
    EXPECT_EQ(l.label(), "lab");
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(1)));  // freed
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(2)));  // unused
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(-1)));
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(4)));
    EXPECT_TRUE(std::holds_alternative<Empty>(r.read(INT32_MAX)));
}

TEST(MetricsReaderRegression, ReuseMidReadDetectedRegression) {
    for (ReadStep when : {ReadStep::AfterFirstRegId, ReadStep::AfterValue}) {
        Env env(2);
        MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    const auto k1 = bytes(4, 1), k2 = bytes(4, 2), k3 = bytes(3, 2);
    auto a = w.allocate(7, MetricKind::Counter, -1, k1, "a");
    auto b = w.allocate(7, MetricKind::Counter, -1, k2, "b");
    auto c = w.allocate(8, MetricKind::Counter, -1, k2, "c");
    MetricsReader r(env.file);
    EXPECT_EQ(r.findFirst(7, k1).slot, a.writer.slot());
    EXPECT_EQ(r.findFirst(7, k1).regId, a.regId);
    EXPECT_EQ(r.findFirst(7, k2).slot, b.writer.slot());
    EXPECT_EQ(r.findFirst(8, k2).slot, c.writer.slot());
    EXPECT_EQ(r.findFirst(7, k3).slot, -1);
    EXPECT_EQ(r.findFirst(9, k1).slot, -1);
    ASSERT_EQ(w.free(b.writer.slot()), FreeStatus::Ok);
    EXPECT_EQ(r.findFirst(7, k2).slot, -1);  // a freed match is skipped
    auto d = w.allocate(7, MetricKind::Counter, -1, k2, "d");
    EXPECT_EQ(r.findFirst(7, k2).slot, d.writer.slot());
}

TEST(RateTrackerRegression, RateFromTotalsRegression) {
    Env env(2);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<2, 4>(r);
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<1, 4>(r);
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<1, 4>(r);
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Gauge, -1, {}, "a");
    MetricsReader r(env.file);
    auto ws = metrics::testing::makeWindow<2, 4>(r);
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
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    for (int i = 0; i < 5; ++i) w.allocate(i + 1, MetricKind::Counter, -1, bytes(4, 3), "x").writer.set(i);
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<8, 4>(r);
    auto ws = metrics::testing::makeWindow<8, 4>(r);
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
        found += r.findFirst(3, probeKey).slot;
    }
    EXPECT_EQ(scope.count(), 0);
    EXPECT_EQ(seen, 50);
    (void)found;
}

namespace {
// Raw stores into the file at a read step, simulating the writer part-way through free + allocate.
struct RawState {
    MetricsFile* file = nullptr;
    int64_t newRegId = -1;  // -1: leave the regId alone (writer paused before the regId store)
    bool fired = false;
};
struct RawHook {
    RawState* st;
    void operator()(ReadStep step, int32_t slot) const noexcept {
        if (st->fired || step != ReadStep::AfterFirstRegId) return;
        st->fired = true;
        std::byte* meta = st->file->base() + st->file->layout().metadataOffset() + int64_t{slot} * kMetadataRecordLength;
        std::byte* val = st->file->base() + st->file->layout().valuesOffset() + int64_t{slot} * kValueRecordLength;
        auto ref32 = [&](int off) { return std::atomic_ref<int32_t>(*reinterpret_cast<int32_t*>(meta + off)); };
        ref32(kMetaStateOffset).store(-1, std::memory_order_release);                               // free
        std::atomic_ref<int64_t>(*reinterpret_cast<int64_t*>(val)).store(0, std::memory_order_release);  // zero for the next owner
        if (st->newRegId >= 0) {
            std::atomic_ref<int64_t>(*reinterpret_cast<int64_t*>(meta + kMetaRegIdOffset))
                .store(st->newRegId, std::memory_order_release);
            ref32(kMetaStateOffset).store(1, std::memory_order_release);  // published
        }
    }
};
}  // namespace

// Found while designing the Task 6 stress test (the interleaving is rare on x86): free + zero before the new regId is stored leaves the old regId next to a
// zeroed value; the reader must re-check the state, not only the regId.
TEST(MetricsReaderRegression, StaleRegIdWithZeroedValueRegression) {
    for (int64_t newRegId : {int64_t{-1}, int64_t{500}}) {
        Env env(1);
        MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
        auto a = w.allocate(1, MetricKind::Counter, -1, {}, "old");
        a.writer.set(12345);
        RawState st{&env.file, newRegId, false};
        BasicMetricsReader<RawHook> r(env.file, RawHook{&st});
        const SlotView v = r.read(0);
        ASSERT_TRUE(st.fired);
        if (newRegId < 0) {
            EXPECT_TRUE(std::holds_alternative<Empty>(v));  // freed mid-read: never Live{oldRegId, 0}
        } else {
            ASSERT_TRUE(std::holds_alternative<Reused>(v));
            EXPECT_EQ(std::get<Reused>(v).oldRegId, a.regId);
        }
    }
}

// ---- regId is unique per allocation; a caller's id is only an owner tag -----------------------------------
TEST(MetricsReaderRegression, AutoExplicitRegIdCollisionRegression) {
    Env env(4);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "auto");  // first regId
    auto b = w.allocate(1, MetricKind::Counter, a.regId, {}, "explicit");  // asks for the same number as a tag
    auto c = w.allocate(1, MetricKind::Counter, 1, {}, "explicit1");
    EXPECT_NE(a.regId, b.regId);
    EXPECT_NE(a.regId, c.regId);
    EXPECT_NE(b.regId, c.regId);
    MetricsReader r(env.file);
    EXPECT_EQ(std::get<Live>(r.read(1)).ownerTag, a.regId);
    EXPECT_EQ(std::get<Live>(r.read(1)).regId, b.regId);
    EXPECT_EQ(std::get<Live>(r.read(2)).ownerTag, 1);
    EXPECT_EQ(std::get<Live>(r.read(0)).ownerTag, 0);  // -1: no tag
}

TEST(MetricsReaderRegression, SameRegIdReuseRegression) {
    Env env(1);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, 5, {}, "old");
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<1, 4>(r);
    constexpr int64_t S = 1'000'000'000;
    a.writer.set(1000);
    rt->sample(r, 0);
    a.writer.set(2000);
    rt->sample(r, S);
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    auto b = w.allocate(1, MetricKind::Counter, 5, {}, "new");  // the same owner tag again
    ASSERT_EQ(b.status, AllocStatus::Ok);
    EXPECT_NE(a.regId, b.regId);
    b.writer.set(10);
    rt->sample(r, 2 * S);
    EXPECT_FALSE(rt->perSecond(0).has_value()) << "a rate across two owners";
    b.writer.set(510);
    rt->sample(r, 3 * S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    EXPECT_DOUBLE_EQ(*rt->perSecond(0), 500.0);
}

TEST(RateTrackerRegression, NegativeDeltaRestartsRegression) {
    Env env(1);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, {}, "a");
    MetricsReader r(env.file);
    auto rt = metrics::testing::makeRates<1, 4>(r);
    constexpr int64_t S = 1'000'000'000;
    a.writer.set(2000);
    rt->sample(r, 0);
    a.writer.set(2500);
    rt->sample(r, S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    a.writer.set(10);  // a total that went backwards under the same regId: rebaseline, no negative rate
    rt->sample(r, 2 * S);
    EXPECT_FALSE(rt->perSecond(0).has_value());
    a.writer.set(110);
    rt->sample(r, 3 * S);
    ASSERT_TRUE(rt->perSecond(0).has_value());
    EXPECT_DOUBLE_EQ(*rt->perSecond(0), 100.0);
}

namespace {
struct SameTagState { MetricsWriter* w; bool fired = false; };
struct SameTagHook {
    SameTagState* st;
    void operator()(ReadStep step, int32_t slot) const noexcept {
        if (st->fired || step != ReadStep::AfterValue) return;
        st->fired = true;
        (void)st->w->free(slot);
        (void)st->w->allocate(99, MetricKind::Gauge, 5, {}, "NEW");
    }
};
}  // namespace

TEST(MetricsReaderRegression, SameRegIdReuseMixesOwnersRegression) {
    Env env(1);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, 5, {}, "old");
    a.writer.set(4242);
    SameTagState st{&w};
    BasicMetricsReader<SameTagHook> r(env.file, SameTagHook{&st});
    const SlotView v = r.read(0);
    ASSERT_TRUE(st.fired);
    ASSERT_TRUE(std::holds_alternative<Reused>(v)) << "a Live must describe one owner";
    EXPECT_EQ(std::get<Reused>(v).oldRegId, a.regId);
}

TEST(WindowSamplerRegression, ReuseUnderSameOwnerTagRestartsRegression) {
    Env env(1);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Gauge, 5, {}, "old");
    MetricsReader r(env.file);
    auto ws = metrics::testing::makeWindow<1, 4>(r);
    a.writer.set(100);
    ws->sample(r, 0);
    a.writer.set(200);
    ws->sample(r, 1);
    EXPECT_EQ(*ws->max(0), 200);
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    auto b = w.allocate(1, MetricKind::Gauge, 5, {}, "new");
    b.writer.set(1);
    ws->sample(r, 2);
    EXPECT_EQ(*ws->max(0), 1);  // not 200: the window restarted for the new owner
    EXPECT_DOUBLE_EQ(*ws->avg(0), 1.0);
}

// A label is copied out with the rest of the slot and validated after the copy: a reader never pairs the label of
// one owner with the value of another, and never races the allocator rewriting the label bytes.
TEST(MetricsReaderRegression, LabelRacesReallocateRegression) {
    Env env(2);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    const AllocResult done = w.allocate(30, MetricKind::Gauge, -1, {}, "done");  // slot 0, written by main only
    (void)w.allocate(1, MetricKind::Gauge, -1, {}, "A");                          // slot 1 is the one reused
    (void)w.free(1);
    constexpr int kCycles = 100'000;
    constexpr int64_t kMaxScans = 50'000'000;  // hard cap: fails instead of hanging
    int64_t mismatches = 0, seen = 0, scans = 0;
    bool sawDone = false;
    std::thread reader([&] {
        MetricsReader r(env.file);
        while (scans < kMaxScans && !sawDone) {
            ++scans;
            r.forEachLive([&](const Live& l) {
                if (l.typeId == 30) { sawDone = l.value == 1; return; }
                if (l.value == 0) return;
                ++seen;
                const char want = l.value == 1 ? 'A' : 'B';
                if (l.label().empty() || l.label()[0] != want || l.label().back() != want) ++mismatches;
            });
        }
    });
    for (int c = 0; c < kCycles; ++c) {
        const bool a = (c & 1) == 0;
        auto res = w.allocate(1, MetricKind::Gauge, -1, {},
                              a ? "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                                : "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
        res.writer.set(a ? 1 : 2);
        (void)w.free(1);
    }
    done.writer.set(1);
    reader.join();
    EXPECT_TRUE(sawDone) << "reader never saw the done slot in " << scans << " scans";
    EXPECT_GT(seen, 0);
    EXPECT_EQ(mismatches, 0) << "label of one owner paired with the value of another, seen " << seen;
}

TEST(MetricsReaderRegression, LabelCopiedBeforeCheckRegression) {
    for (ReadStep when : {ReadStep::AfterFirstRegId, ReadStep::AfterValue}) {
        Env env(2);
        MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
        auto a = w.allocate(1, MetricKind::Counter, -1, bytes(4, 7), "old-label");
        a.writer.set(5);
        ReuseState st{&w, when, false};  // frees the slot and allocates "new" with value 777
        BasicMetricsReader<ReuseHook> r(env.file, ReuseHook{&st});
        const SlotView v = r.read(0);
        ASSERT_TRUE(st.fired);
        // The label, key and value were copied before the owner changed: never a Live of mixed owners.
        EXPECT_FALSE(std::holds_alternative<Live>(v)) << "step " << static_cast<int>(when);
        const SlotView again = r.read(0);
        ASSERT_TRUE(std::holds_alternative<Live>(again));
        EXPECT_EQ(std::get<Live>(again).label(), "new");
        EXPECT_EQ(std::get<Live>(again).key().size(), 0U);
        EXPECT_EQ(std::get<Live>(again).value, 777);
    }
}

TEST(MetricsReaderRegression, LiveIsACopyRegression) {
    Env env(1);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    auto a = w.allocate(1, MetricKind::Counter, -1, bytes(3, 9), "first");
    MetricsReader r(env.file);
    const Live copy = std::get<Live>(r.read(0));
    ASSERT_EQ(w.free(0), FreeStatus::Ok);
    (void)w.allocate(2, MetricKind::Gauge, -1, bytes(5, 1), "second-owner-label");
    // The copy still describes the first owner.
    EXPECT_EQ(copy.label(), "first");
    EXPECT_EQ(copy.key().size(), 3U);
    EXPECT_EQ(copy.key()[0], std::byte{9});
    EXPECT_EQ(copy.typeId, 1);
    // No view escapes into the mapping.
    const auto* base = reinterpret_cast<const char*>(env.file.base());
    EXPECT_FALSE(copy.label().data() >= base && copy.label().data() < base + env.file.length());
    EXPECT_FALSE(reinterpret_cast<const char*>(copy.key().data()) >= base &&
                 reinterpret_cast<const char*>(copy.key().data()) < base + env.file.length());
    (void)a;
}

TEST(RateTrackerRegression, CapacitySmallerThanReaderRefusedRegression) {
    Env env(3);
    MetricsWriter w = metrics::testing::makeWriter(env.file, env.stack);
    for (int i = 0; i < 3; ++i) w.allocate(1, MetricKind::Counter, -1, {}, "x").writer.set(10 * (i + 1));
    MetricsReader r(env.file);
    constexpr int64_t S = 1'000'000'000;
    {
        RateTracker<2, 4> small;  // 2 < 3 slots
        EXPECT_EQ(small.bind(r), TrackerStatus::CapacityTooSmall);
        small.sample(r, 0);       // unbound: samples nothing, never a silent partial scan
        small.sample(r, S);
        for (int32_t s = 0; s < 4; ++s) EXPECT_FALSE(small.perSecond(s).has_value());
    }
    {
        WindowSampler<2, 4> small;
        EXPECT_EQ(small.bind(r), TrackerStatus::CapacityTooSmall);
        small.sample(r, 0);
        for (int32_t s = 0; s < 4; ++s) EXPECT_FALSE(small.max(s).has_value());
    }
    RateTracker<3, 4> exact;
    EXPECT_EQ(exact.bind(r), TrackerStatus::Ok);
    RateTracker<8, 4> bigger;
    EXPECT_EQ(bigger.bind(r), TrackerStatus::Ok);
    WindowSampler<3, 4> wexact;
    EXPECT_EQ(wexact.bind(r), TrackerStatus::Ok);
    exact.sample(r, 0);
    exact.sample(r, S);
    EXPECT_TRUE(exact.perSecond(2).has_value());
}
