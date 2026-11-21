#include "TestDir.h"
#include "TestHelpers.h"

#include "metrics/Families.h"
#include "metrics/Histogram.h"
#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"
#include "metrics/PrometheusText.h"

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
    std::vector<Live> live;
    std::vector<int32_t> order;
    std::vector<char> buf;
    explicit Env(int32_t n) : stack(static_cast<size_t>(n) + 1), live(128), order(128), buf(1 << 16) {
        const std::string p = dir.path("h.dat");
        CreateParams cp{p.c_str(), n, n, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
        w = metrics::testing::makeWriter(file, stack);
    }
    void publishH(int32_t id, const char* name, uint8_t scale) {
        const Family f{id, name, "H.", FamilyType::Histogram, Merge::Sum, LabelMode::Set, id + 500, scale};
        ASSERT_EQ(publishFamilies(w, std::span<const Family>(&f, 1)), FamilyStatus::Ok);
    }
    std::string render() {
        MetricsReader r(file);
        const RenderResult rr = PrometheusText::renderNamed(r, NamedScratch{live, order}, std::span<char>(buf));
        EXPECT_FALSE(rr.overflow);
        EXPECT_EQ(rr.skipped, 0u);
        return std::string(buf.data(), rr.length);
    }
};

std::string fmt(int64_t raw, uint8_t scale) {
    char b[48];
    return std::string(b, formatScaled(raw, scale, std::span<char>(b)));
}
}  // namespace

TEST(Histogram, FormatScaled) {
    EXPECT_EQ(fmt(500, 6), "0.0005");
    EXPECT_EQ(fmt(2500, 3), "2.5");
    EXPECT_EQ(fmt(7, 0), "7");
    EXPECT_EQ(fmt(1000, 3), "1");
    EXPECT_EQ(fmt(-5, 1), "-0.5");
    EXPECT_EQ(fmt(0, 6), "0");
    EXPECT_EQ(fmt(INT64_MIN, 0), "-9223372036854775808");
    char tiny[2];
    EXPECT_EQ(formatScaled(12345, 0, std::span<char>(tiny)), 0u);  // does not fit
}

TEST(Histogram, AllocateLayout) {
    Env env(8);
    HistogramWriter h;
    const int64_t bounds[] = {500, 1000};
    ASSERT_EQ(allocateHistogram(env.w, 5, 6, "lane=\"0\"", bounds, 6, h), HistogramStatus::Ok);
    EXPECT_TRUE(h.bound());
    EXPECT_EQ(h.bounds(), 2u);
    EXPECT_EQ(env.w.allocatedCount(), 4);
    MetricsReader r(env.file);
    std::vector<std::string> got;
    r.forEachLive([&](const Live& l) {
        got.push_back(std::to_string(l.typeId) + "/" + std::to_string(static_cast<int>(l.key()[0])) + "/" +
                      std::string(l.label()));
    });
    std::sort(got.begin(), got.end());
    const std::vector<std::string> want{"5/0/lane=\"0\",le=\"0.0005\"", "5/1/lane=\"0\",le=\"0.001\"",
                                        "5/2/lane=\"0\",le=\"+Inf\"", "6/255/lane=\"0\""};
    EXPECT_EQ(got, want);

    HistogramWriter e;
    ASSERT_EQ(allocateHistogram(env.w, 7, 8, "", std::span<const int64_t>(bounds, 1), 6, e), HistogramStatus::Ok);
    bool found = false;
    r.forEachLive([&](const Live& l) { found = found || (l.typeId == 7 && l.label() == "le=\"0.0005\""); });
    EXPECT_TRUE(found);
}

TEST(Histogram, ObserveCumulative) {
    Env env(8);
    HistogramWriter h;
    const int64_t bounds[] = {500, 1000};
    ASSERT_EQ(allocateHistogram(env.w, 5, 6, "", bounds, 6, h), HistogramStatus::Ok);
    for (const int64_t v : {int64_t{0}, int64_t{-3}, int64_t{500}, int64_t{501}, int64_t{10'000}}) h.observe(v);
    EXPECT_EQ(h.bucket(0), 3);
    EXPECT_EQ(h.bucket(1), 4);
    EXPECT_EQ(h.bucket(2), 5);
    EXPECT_EQ(h.sum(), 10'998);
}

TEST(Histogram, RejectsBadBounds) {
    Env env(32);
    HistogramWriter h;
    const auto tryIt = [&](std::span<const int64_t> b, uint8_t scale) {
        const HistogramStatus st = allocateHistogram(env.w, 5, 6, "", b, scale, h);
        EXPECT_EQ(env.w.allocatedCount(), 0);
        EXPECT_FALSE(h.bound());
        return st;
    };
    EXPECT_EQ(tryIt({}, 0), HistogramStatus::BadBounds);
    std::vector<int64_t> many(17);
    for (size_t i = 0; i < many.size(); ++i) many[i] = static_cast<int64_t>(i) + 1;
    EXPECT_EQ(tryIt(many, 0), HistogramStatus::BadBounds);
    const int64_t same[] = {5, 5};
    EXPECT_EQ(tryIt(same, 0), HistogramStatus::BadBounds);
    const int64_t zero[] = {0, 1};
    EXPECT_EQ(tryIt(zero, 0), HistogramStatus::BadBounds);
    const int64_t ok[] = {1};
    EXPECT_EQ(tryIt(ok, 10), HistogramStatus::BadScale);
    EXPECT_EQ(allocateHistogram(env.w, 5, 6, "", std::span<const int64_t>(many).first(16), 0, h),
              HistogramStatus::Ok);  // 16 is allowed
}

TEST(Histogram, AllOrNothing) {
    Env env(3);
    HistogramWriter h;
    const int64_t bounds[] = {1, 2};
    EXPECT_EQ(allocateHistogram(env.w, 5, 6, "", bounds, 0, h), HistogramStatus::Full);
    EXPECT_EQ(env.w.allocatedCount(), 0);
    EXPECT_FALSE(h.bound());
    MetricsReader r(env.file);
    int live = 0;
    r.forEachLive([&](const Live&) { ++live; });
    EXPECT_EQ(live, 0);
}

TEST(Histogram, FreeReleasesAll) {
    Env env(8);
    HistogramWriter h;
    const int64_t bounds[] = {1, 2};
    ASSERT_EQ(allocateHistogram(env.w, 5, 6, "", bounds, 0, h), HistogramStatus::Ok);
    EXPECT_EQ(env.w.allocatedCount(), 4);
    freeHistogram(env.w, h);
    EXPECT_EQ(env.w.allocatedCount(), 0);
    EXPECT_FALSE(h.bound());
    freeHistogram(env.w, h);  // an unbound writer is a no-op
    EXPECT_EQ(env.w.allocatedCount(), 0);
}

TEST(Histogram, RenderedCumulative) {
    Env env(16);
    env.publishH(5, "h", 0);
    HistogramWriter h;
    const int64_t bounds[] = {1, 2};
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "", bounds, 0, h), HistogramStatus::Ok);
    for (const int64_t v : {1, 2, 2, 9}) h.observe(v);
    EXPECT_EQ(env.render(),
              "# HELP h H.\n# TYPE h histogram\nh_bucket{le=\"1\"} 1\nh_bucket{le=\"2\"} 3\n"
              "h_bucket{le=\"+Inf\"} 4\nh_sum 14\nh_count 4\n");
}

TEST(Histogram, ScaledSum) {
    Env env(16);
    env.publishH(5, "h", 6);
    HistogramWriter h;
    const int64_t bounds[] = {1000};
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "", bounds, 6, h), HistogramStatus::Ok);
    h.observe(1500);
    const std::string t = env.render();
    EXPECT_NE(t.find("h_sum 0.0015\n"), std::string::npos) << t;
    EXPECT_NE(t.find("h_bucket{le=\"0.001\"} 0\n"), std::string::npos) << t;
}

TEST(Histogram, BoundOrderNotTextOrder) {
    Env env(16);
    env.publishH(5, "h", 0);
    HistogramWriter h;
    const int64_t bounds[] = {2, 10};
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "", bounds, 0, h), HistogramStatus::Ok);
    h.observe(3);
    const std::string t = env.render();
    const size_t a = t.find("le=\"2\""), b = t.find("le=\"10\""), c = t.find("le=\"+Inf\"");
    ASSERT_NE(a, std::string::npos);
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(c, std::string::npos);
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
}

TEST(Histogram, CountEqualsInfAcrossWriters) {
    Env env(16);
    env.publishH(5, "h", 0);
    HistogramWriter a, b;
    const int64_t bounds[] = {1};
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "lane=\"0\"", bounds, 0, a), HistogramStatus::Ok);
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "lane=\"0\"", bounds, 0, b), HistogramStatus::Ok);
    for (int i = 0; i < 3; ++i) a.observe(7);
    for (int i = 0; i < 5; ++i) b.observe(7);
    const std::string t = env.render();
    EXPECT_NE(t.find("h_bucket{lane=\"0\",le=\"+Inf\"} 8\n"), std::string::npos) << t;
    EXPECT_NE(t.find("h_count{lane=\"0\"} 8\n"), std::string::npos) << t;
    EXPECT_NE(t.find("h_sum{lane=\"0\"} 56\n"), std::string::npos) << t;
    EXPECT_EQ(t.find("# TYPE h histogram"), t.rfind("# TYPE h histogram"));
}

TEST(Histogram, SeveralBasesAndDuplicateFamily) {
    Env env(32);
    env.publishH(5, "h", 0);
    env.publishH(9, "h", 0);  // same name, higher typeId: not printed
    HistogramWriter a, b, d;
    const int64_t bounds[] = {1};
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "lane=\"1\"", bounds, 0, a), HistogramStatus::Ok);
    ASSERT_EQ(allocateHistogram(env.w, 5, 505, "lane=\"0\"", bounds, 0, b), HistogramStatus::Ok);
    ASSERT_EQ(allocateHistogram(env.w, 9, 509, "lane=\"2\"", bounds, 0, d), HistogramStatus::Ok);
    a.observe(1);
    b.observe(5);
    d.observe(5);
    MetricsReader r(env.file);
    const RenderResult rr = PrometheusText::renderNamed(r, NamedScratch{env.live, env.order}, std::span<char>(env.buf));
    const std::string t(env.buf.data(), rr.length);
    EXPECT_EQ(rr.skipped, 3u);  // two buckets and the sum of the duplicate
    EXPECT_EQ(t,
              "# HELP h H.\n# TYPE h histogram\n"
              "h_bucket{lane=\"0\",le=\"1\"} 0\nh_bucket{lane=\"0\",le=\"+Inf\"} 1\nh_sum{lane=\"0\"} 5\nh_count{lane=\"0\"} 1\n"
              "h_bucket{lane=\"1\",le=\"1\"} 1\nh_bucket{lane=\"1\",le=\"+Inf\"} 1\nh_sum{lane=\"1\"} 1\nh_count{lane=\"1\"} 1\n");
}
