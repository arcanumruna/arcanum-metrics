#include "AllocCounter.h"
#include "TestDir.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsWriter.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

using namespace metrics;

static_assert(std::is_trivially_copyable_v<SlotWriter>);
static_assert(sizeof(SlotWriter) <= 16);
static_assert(std::atomic_ref<int64_t>::is_always_lock_free);

namespace {

struct Env {
    metrics::testing::TestDir dir;
    MetricsFile file;
    std::vector<int32_t> stack;
    explicit Env(int32_t maxSlots) : stack(static_cast<size_t>(maxSlots) + 1) {
        const std::string p = dir.path("w.dat");
        CreateParams cp{p.c_str(), maxSlots, maxSlots, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
    }
    std::byte* meta(int32_t s) { return file.base() + file.layout().metadataOffset() + int64_t{s} * kMetadataRecordLength; }
    std::byte* val(int32_t s) { return file.base() + file.layout().valuesOffset() + int64_t{s} * kValueRecordLength; }
    template <class T> T metaAt(int32_t s, int32_t off) { T v; std::memcpy(&v, meta(s) + off, sizeof v); return v; }
    int64_t value(int32_t s) { int64_t v; std::memcpy(&v, val(s), 8); return v; }
    std::vector<std::byte> metaSnapshot() {
        return {file.base() + file.layout().metadataOffset(), file.base() + file.layout().valuesOffset()};
    }
};

std::vector<std::byte> bytes(size_t n) { return std::vector<std::byte>(n, std::byte{0x5a}); }

// ---- ErrorTable -------------------------------------------------------------------------------------------
struct ErrRow {
    int32_t maxSlots;
    int32_t prefill;
    int32_t typeId;
    int64_t ownerTag;
    size_t keyLen;
    size_t labelLen;
    AllocStatus expect;
};
std::string name(const ::testing::TestParamInfo<ErrRow>& i) { return std::to_string(i.index); }

std::vector<ErrRow> errRows() {
    std::vector<ErrRow> rows;
    // capacity 0 / 1 / full
    rows.push_back({0, 0, 7, -1, 0, 0, AllocStatus::Full});
    rows.push_back({1, 0, 7, -1, 0, 0, AllocStatus::Ok});
    rows.push_back({1, 1, 7, -1, 0, 0, AllocStatus::Full});
    rows.push_back({3, 3, 7, -1, 4, 4, AllocStatus::Full});
    // single-fault matrix over type x regId x key x label on a file with room
    for (int32_t t : {0, -1, INT32_MAX, 1000}) {
        for (int64_t r : {int64_t{-1}, int64_t{0}, int64_t{5}, int64_t{-2}, INT64_MAX, INT64_MIN}) {
            for (size_t k : {size_t{0}, size_t{1}, size_t{112}, size_t{113}}) {
                for (size_t l : {size_t{0}, size_t{1}, size_t{363}, size_t{364}, size_t{365}}) {
                    const int faults = (t < 0) + (r < -1) + (k > 112) + (l > 364);
                    if (faults > 1) continue;
                    AllocStatus e = AllocStatus::Ok;
                    if (t < 0) e = AllocStatus::InvalidType;
                    else if (r < -1) e = AllocStatus::InvalidOwnerTag;
                    else if (k > 112) e = AllocStatus::KeyTooLong;
                    else if (l > 364) e = AllocStatus::LabelTooLong;
                    rows.push_back({4, 0, t, r, k, l, e});
                }
            }
        }
    }
    return rows;
}

class MetricsWriterRegressionErrorTable : public ::testing::TestWithParam<ErrRow> {};

TEST_P(MetricsWriterRegressionErrorTable, Row) {
    const ErrRow r = GetParam();
    Env env(r.maxSlots);
    MetricsWriter w(env.file, env.stack);
    for (int32_t i = 0; i < r.prefill; ++i) {
        ASSERT_EQ(w.allocate(1, MetricKind::Counter, -1, {}, "pre").status, AllocStatus::Ok);
    }
    const auto key = bytes(r.keyLen);
    const std::string label(r.labelLen, 'L');
    const auto before = env.metaSnapshot();
    const int32_t countBefore = w.allocatedCount();
    AllocResult res;
    {
        metrics::testing::AllocScope scope;
        res = w.allocate(r.typeId, MetricKind::Gauge, r.ownerTag, key, label);
        EXPECT_EQ(scope.count(), 0);
    }
    EXPECT_EQ(res.status, r.expect);
    if (r.expect == AllocStatus::Ok) {
        EXPECT_EQ(w.allocatedCount(), countBefore + 1);
        EXPECT_EQ(res.writer.slot(), r.prefill);
        EXPECT_EQ(env.metaAt<int32_t>(res.writer.slot(), kMetaStateOffset), 1);
        EXPECT_EQ(env.metaAt<int32_t>(res.writer.slot(), kMetaTypeIdOffset), r.typeId);
        EXPECT_EQ(env.metaAt<int32_t>(res.writer.slot(), kMetaKindOffset), 2);
        EXPECT_EQ(env.metaAt<int32_t>(res.writer.slot(), kMetaKeyLengthOffset), static_cast<int32_t>(r.keyLen));
        EXPECT_EQ(env.metaAt<int32_t>(res.writer.slot(), kMetaLabelLengthOffset), static_cast<int32_t>(r.labelLen));
        EXPECT_EQ(std::memcmp(env.meta(res.writer.slot()) + kMetaLabelOffset, label.data(), r.labelLen), 0);
        EXPECT_EQ(std::memcmp(env.meta(res.writer.slot()) + kMetaKeyOffset, key.data(), r.keyLen), 0);
        EXPECT_GT(res.regId, 0);
        EXPECT_EQ(env.metaAt<int64_t>(res.writer.slot(), kMetaOwnerTagOffset), r.ownerTag < 0 ? 0 : r.ownerTag);
        EXPECT_EQ(env.metaAt<int64_t>(res.writer.slot(), kMetaRegIdOffset), res.regId);
    } else {
        EXPECT_EQ(w.allocatedCount(), countBefore);
        EXPECT_EQ(env.metaSnapshot(), before);
        EXPECT_EQ(res.writer.slot(), -1);
    }
}
INSTANTIATE_TEST_SUITE_P(ErrorTable, MetricsWriterRegressionErrorTable, ::testing::ValuesIn(errRows()), name);

// ---- FreeTable --------------------------------------------------------------------------------------------
struct FreeRow {
    int32_t slot;          // slot to free
    int32_t freeFirst;     // free this slot before (-1 none)
    FreeStatus expect;
};
std::string fname(const ::testing::TestParamInfo<FreeRow>& i) { return std::to_string(i.index); }

class MetricsWriterRegressionFreeTable : public ::testing::TestWithParam<FreeRow> {};

TEST_P(MetricsWriterRegressionFreeTable, Row) {
    const FreeRow r = GetParam();
    Env env(4);
    MetricsWriter w(env.file, env.stack);
    for (int i = 0; i < 2; ++i) ASSERT_EQ(w.allocate(1, MetricKind::Counter, -1, {}, "x").status, AllocStatus::Ok);
    // slots 0 and 1 allocated; 2 and 3 unused
    if (r.freeFirst >= 0) ASSERT_EQ(w.free(r.freeFirst), FreeStatus::Ok);
    const int32_t before = w.allocatedCount();
    FreeStatus s;
    {
        metrics::testing::AllocScope scope;
        s = w.free(r.slot);
        EXPECT_EQ(scope.count(), 0);
    }
    EXPECT_EQ(s, r.expect);
    EXPECT_EQ(w.allocatedCount(), before - (r.expect == FreeStatus::Ok ? 1 : 0));
    if (r.expect == FreeStatus::Ok) {
        EXPECT_EQ(env.metaAt<int32_t>(r.slot, kMetaStateOffset), -1);
    }
}
INSTANTIATE_TEST_SUITE_P(FreeTable, MetricsWriterRegressionFreeTable,
                         ::testing::Values(FreeRow{0, -1, FreeStatus::Ok}, FreeRow{1, -1, FreeStatus::Ok},
                                           FreeRow{0, 0, FreeStatus::NotAllocated},   // already freed
                                           FreeRow{2, -1, FreeStatus::NotAllocated},  // unused
                                           FreeRow{3, -1, FreeStatus::NotAllocated},
                                           FreeRow{-1, -1, FreeStatus::OutOfRange},
                                           FreeRow{4, -1, FreeStatus::OutOfRange},
                                           FreeRow{INT32_MAX, -1, FreeStatus::OutOfRange},
                                           FreeRow{INT32_MIN, -1, FreeStatus::OutOfRange}),
                         fname);

}  // namespace

// ---- behaviour --------------------------------------------------------------------------------------------
TEST(MetricsWriterRegression, ReuseLifoAndZeroedRegression) {
    Env env(4);
    MetricsWriter w(env.file, env.stack);
    auto a = w.allocate(10, MetricKind::Counter, -1, bytes(4), "A");
    auto other = w.allocate(11, MetricKind::Counter, -1, {}, "other");
    ASSERT_EQ(a.status, AllocStatus::Ok);
    a.writer.add(5);
    EXPECT_EQ(env.value(a.writer.slot()), 5);
    ASSERT_EQ(w.free(a.writer.slot()), FreeStatus::Ok);
    ASSERT_EQ(w.free(other.writer.slot()), FreeStatus::Ok);
    auto b = w.allocate(12, MetricKind::Gauge, -1, {}, "B");  // LIFO: the last freed slot first
    EXPECT_EQ(b.writer.slot(), other.writer.slot());
    auto c = w.allocate(13, MetricKind::Gauge, -1, {}, "C");
    EXPECT_EQ(c.writer.slot(), a.writer.slot());
    EXPECT_EQ(env.value(c.writer.slot()), 0);
    EXPECT_NE(c.regId, a.regId);
    EXPECT_EQ(env.metaAt<int32_t>(c.writer.slot(), kMetaLabelLengthOffset), 1);
    EXPECT_EQ(static_cast<char>(env.meta(c.writer.slot())[kMetaLabelOffset]), 'C');
    EXPECT_EQ(env.metaAt<int32_t>(c.writer.slot(), kMetaKeyLengthOffset), 0);
}

namespace {
struct ProbeState {
    Env* env;
    int32_t slot;
    int64_t newRegId;
    int violations = 0;
    int calls = 0;
};
struct ProbeHook {
    ProbeState* st;
    void operator()(AllocStep, int32_t slot) const noexcept {
        ++st->calls;
        if (slot != st->slot) return;
        // A reader that sees Allocated must see a zeroed value and the new regId.
        if (st->env->metaAt<int32_t>(slot, kMetaStateOffset) == 1) {
            if (st->env->value(slot) != 0) ++st->violations;
            if (st->env->metaAt<int64_t>(slot, kMetaRegIdOffset) == 0) ++st->violations;
        }
    }
};
}  // namespace

TEST(MetricsWriterRegression, ReuseNeverPublishesStaleValueRegression) {
    Env env(2);
    ProbeState st{&env, 0, 0};
    BasicMetricsWriter<ProbeHook> w(env.file, env.stack, ProbeHook{&st});
    auto a = w.allocate(10, MetricKind::Counter, -1, {}, "A");
    a.writer.add(5);
    ASSERT_EQ(w.free(a.writer.slot()), FreeStatus::Ok);
    auto b = w.allocate(10, MetricKind::Counter, -1, {}, "B");
    EXPECT_EQ(b.writer.slot(), 0);
    EXPECT_GE(st.calls, 4);
    EXPECT_EQ(st.violations, 0);
}

TEST(MetricsWriterRegression, AutoRegIdMonotonicRegression) {
    Env env(3);
    MetricsWriter w(env.file, env.stack);
    int64_t last = -1;
    for (int i = 0; i < 200; ++i) {
        auto r = w.allocate(1, MetricKind::Counter, -1, {}, "x");
        ASSERT_EQ(r.status, AllocStatus::Ok);
        EXPECT_GT(r.regId, last);
        last = r.regId;
        if (i % 2 == 1) ASSERT_EQ(w.free(r.writer.slot()), FreeStatus::Ok);
        else if (i % 4 == 0) ASSERT_EQ(w.free(r.writer.slot()), FreeStatus::Ok);
        // an explicit owner tag never becomes a regId, so it cannot collide with the sequence
        auto e = w.allocate(1, MetricKind::Counter, 3, {}, "e");
        ASSERT_EQ(e.status, AllocStatus::Ok);
        EXPECT_GT(e.regId, last);
        last = e.regId;
        ASSERT_EQ(w.free(e.writer.slot()), FreeStatus::Ok);
        if (w.allocatedCount() >= 2) {
            // keep room: free everything still held
            for (int32_t s = 0; s < 3; ++s) (void)w.free(s);
        }
    }
}

TEST(MetricsWriterRegression, AddSetSemanticsRegression) {
    Env env(1);
    MetricsWriter w(env.file, env.stack);
    auto r = w.allocate(1, MetricKind::Counter, -1, {}, "x");
    r.writer.add(3);
    r.writer.add(4);
    EXPECT_EQ(env.value(0), 7);
    EXPECT_EQ(r.writer.get(), 7);
    r.writer.set(-2);
    EXPECT_EQ(env.value(0), -2);
    r.writer.set(INT64_MAX);
    r.writer.add(1);  // two's-complement wrap
    EXPECT_EQ(env.value(0), INT64_MIN);
    r.writer.add(-1);
    EXPECT_EQ(env.value(0), INT64_MAX);
}

TEST(MetricsWriterRegression, ZeroAllocAllOpsRegression) {
    Env env(8);
    MetricsWriter w(env.file, env.stack);
    const auto key = bytes(4);
    metrics::testing::AllocScope scope;
    for (int i = 0; i < 100; ++i) {
        auto r = w.allocate(1, MetricKind::Counter, -1, key, "label");
        r.writer.add(1);
        r.writer.set(5);
        (void)r.writer.get();
        (void)w.free(r.writer.slot());
    }
    EXPECT_EQ(scope.count(), 0);
}
