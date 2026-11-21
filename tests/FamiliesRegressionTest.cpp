#include "TestDir.h"
#include "TestHelpers.h"

#include "metrics/Families.h"
#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"
#ifdef __linux__  // SystemMetrics.h reads /proc through getdents64
#include "metrics/SystemMetrics.h"
#endif

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace metrics;

namespace {
struct Env {
    metrics::testing::TestDir dir;
    MetricsFile file;
    std::vector<int32_t> stack;
    MetricsWriter w;
    explicit Env(int32_t n) : stack(static_cast<size_t>(n) + 1) {
        const std::string p = dir.path("f.dat");
        CreateParams cp{p.c_str(), n, n, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
        w = metrics::testing::makeWriter(file, stack);
    }
};
}  // namespace

TEST(Families, DescriptorRoundTrip) {
    Env env(8);
    const Family row{7, "a_total", "A things.", FamilyType::Counter};
    ASSERT_EQ(publishFamilies(env.w, std::span<const Family>(&row, 1)), FamilyStatus::Ok);
    MetricsReader r(env.file);
    int count = 0;
    r.forEachLive([&](const Live& l) {
        ++count;
        EXPECT_EQ(l.typeId, 1'000'306);
        ASSERT_EQ(l.keyLength, 4);
        EXPECT_EQ(l.keyBytes[0], std::byte{7});
        EXPECT_EQ(l.keyBytes[1], std::byte{0});
        EXPECT_EQ(l.keyBytes[2], std::byte{0});
        EXPECT_EQ(l.keyBytes[3], std::byte{0});
        EXPECT_EQ(l.label(), "a_total\tc\t0\ts\tl\t0\tA things.");
        FamilyView v{};
        ASSERT_TRUE(decodeFamily(l, v));
        EXPECT_EQ(v.typeId, 7);
        EXPECT_EQ(v.name, "a_total");
        EXPECT_EQ(v.help, "A things.");
        EXPECT_EQ(v.type, FamilyType::Counter);
        EXPECT_EQ(v.merge, Merge::Sum);
        EXPECT_EQ(v.labels, LabelMode::Set);
        EXPECT_EQ(v.sumTypeId, 0);
        EXPECT_EQ(v.scaleExp, 0);
    });
    EXPECT_EQ(count, 1);
}

TEST(Families, DescriptorRoundTripAllFields) {
    Env env(8);
    const Family row{9, "h_seconds", "Latency.", FamilyType::Histogram, Merge::Max, LabelMode::None, 509, 9};
    ASSERT_EQ(publishFamilies(env.w, std::span<const Family>(&row, 1)), FamilyStatus::Ok);
    MetricsReader r(env.file);
    r.forEachLive([&](const Live& l) {
        EXPECT_EQ(l.label(), "h_seconds\th\t9\tm\tn\t509\tLatency.");
        FamilyView v{};
        ASSERT_TRUE(decodeFamily(l, v));
        EXPECT_EQ(v.type, FamilyType::Histogram);
        EXPECT_EQ(v.merge, Merge::Max);
        EXPECT_EQ(v.labels, LabelMode::None);
        EXPECT_EQ(v.sumTypeId, 509);
        EXPECT_EQ(v.scaleExp, 9);
    });
}

TEST(Families, RefusalsAllocateNothing) {
    Env env(8);
    int32_t failed = -2;
    {
        const std::string longHelp(400, 'h');
        const Family row{1, "a", longHelp, FamilyType::Counter};
        EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(&row, 1), &failed), FamilyStatus::TooLong);
        EXPECT_EQ(failed, 0);
    }
    {
        const Family row{1, "1abc", "x", FamilyType::Counter};
        EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(&row, 1)), FamilyStatus::BadName);
    }
    {
        const Family row{1, "abc", "x\ny", FamilyType::Counter};
        EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(&row, 1)), FamilyStatus::BadHelp);
    }
    {
        const Family rows[] = {{7, "a", "x", FamilyType::Counter}, {7, "b", "x", FamilyType::Counter}};
        EXPECT_EQ(publishFamilies(env.w, rows, &failed), FamilyStatus::Duplicate);
        EXPECT_EQ(failed, 1);
    }
    {
        const Family rows[] = {{7, "a", "x", FamilyType::Counter}, {8, "a", "x", FamilyType::Counter}};
        EXPECT_EQ(publishFamilies(env.w, rows, &failed), FamilyStatus::Duplicate);  // same name
        EXPECT_EQ(failed, 1);
    }
    {
        const Family row{7, "h", "x", FamilyType::Histogram, Merge::Sum, LabelMode::Set, 0, 0};
        EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(&row, 1)), FamilyStatus::BadHistogram);
        const Family same{7, "h", "x", FamilyType::Histogram, Merge::Sum, LabelMode::Set, 7, 0};
        EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(&same, 1)), FamilyStatus::BadHistogram);
        const Family scale{7, "h", "x", FamilyType::Histogram, Merge::Sum, LabelMode::Set, 8, 10};
        EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(&scale, 1)), FamilyStatus::BadHistogram);
    }
    EXPECT_EQ(env.w.allocatedCount(), 0);
}

TEST(Families, FullFreesWhatItAllocated) {
    Env env(2);
    const Family rows[] = {{1, "a", "x", FamilyType::Counter},
                           {2, "b", "x", FamilyType::Counter},
                           {3, "c", "x", FamilyType::Counter}};
    int32_t failed = -2;
    EXPECT_EQ(publishFamilies(env.w, rows, &failed), FamilyStatus::Full);
    EXPECT_EQ(failed, 2);
    EXPECT_EQ(env.w.allocatedCount(), 0);
    MetricsReader r(env.file);
    int live = 0;
    r.forEachLive([&](const Live&) { ++live; });
    EXPECT_EQ(live, 0);
    // The slots are reusable.
    EXPECT_EQ(publishFamilies(env.w, std::span<const Family>(rows, 2)), FamilyStatus::Ok);
    EXPECT_EQ(env.w.allocatedCount(), 2);
}

TEST(Families, DecodeRejectsMalformed) {
    Env env(4);
    const std::byte key[4] = {std::byte{1}, {}, {}, {}};
    FamilyView v{};
    const auto decodes = [&](int32_t typeId, std::string_view label) {
        const AllocResult a = env.w.allocate(typeId, MetricKind::Gauge, -1, key, label);
        EXPECT_EQ(a.status, AllocStatus::Ok);
        MetricsReader r(env.file);
        const bool ok = decodeFamily(std::get<Live>(r.read(a.writer.slot())), v);
        EXPECT_EQ(env.w.free(a.writer.slot()), FreeStatus::Ok);
        return ok;
    };
    EXPECT_TRUE(decodes(kFamilyDescriptorType, "a\tc\t0\ts\tl\t0\t"));
    EXPECT_FALSE(decodes(kFamilyDescriptorType, "a\tc\t0\ts\tl\t0"));        // no help field
    EXPECT_FALSE(decodes(kFamilyDescriptorType, "a\tx\t0\ts\tl\t0\th"));     // bad type
    EXPECT_FALSE(decodes(kFamilyDescriptorType, "1a\tc\t0\ts\tl\t0\th"));    // bad name
    EXPECT_FALSE(decodes(kFamilyDescriptorType, "a\tc\tq\ts\tl\t0\th"));     // bad scale
    EXPECT_FALSE(decodes(42, "a\tc\t0\ts\tl\t0\th"));                        // not a descriptor
}

TEST(Families, SystemMetricsTableMatchesTypeIds) {
#ifdef __linux__
    static_assert(kSystemMetricsFamilies[0].typeId == SystemMetrics::kTypeOpenFds);
    static_assert(kSystemMetricsFamilies[1].typeId == SystemMetrics::kTypeThreads);
    static_assert(kSystemMetricsFamilies[2].typeId == SystemMetrics::kTypeResidentBytes);
    static_assert(kSystemMetricsFamilies[3].typeId == SystemMetrics::kTypeVirtualBytes);
    static_assert(kSystemMetricsFamilies[4].typeId == SystemMetrics::kTypeReadErrors);
#endif
    for (size_t i = 0; i < kSystemMetricsFamilies.size(); ++i) {
        EXPECT_EQ(kSystemMetricsFamilies[i].typeId, 1'000'301 + static_cast<int32_t>(i));
        EXPECT_EQ(kSystemMetricsFamilies[i].labels, LabelMode::None);
    }
    Env env(8);
    EXPECT_EQ(publishFamilies(env.w, kSystemMetricsFamilies), FamilyStatus::Ok);
    EXPECT_EQ(env.w.allocatedCount(), 5);
}
