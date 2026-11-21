#include "AllocCounter.h"
#include "TestDir.h"
#include "TestHelpers.h"

#include "metrics/Families.h"
#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"
#include "metrics/PrometheusText.h"
#ifdef __linux__  // SystemMetrics.h reads /proc through getdents64
#include "metrics/SystemMetrics.h"
#endif

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
    explicit Env(int32_t n, size_t scratch = 64) : stack(static_cast<size_t>(n) + 1), live(scratch), order(scratch), buf(1 << 16) {
        const std::string p = dir.path("n.dat");
        CreateParams cp{p.c_str(), n, n, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
        w = metrics::testing::makeWriter(file, stack);
    }
    void publish(const Family& f) { ASSERT_EQ(publishFamilies(w, std::span<const Family>(&f, 1)), FamilyStatus::Ok); }
    SlotWriter slot(int32_t typeId, MetricKind kind, std::string_view label, int32_t keyByte = 0) {
        const std::byte key[1] = {static_cast<std::byte>(keyByte)};
        const AllocResult r = w.allocate(typeId, kind, -1, key, label);
        EXPECT_EQ(r.status, AllocStatus::Ok);
        return r.writer;
    }
    RenderResult render(size_t outSize = 1 << 16) {
        MetricsReader r(file);
        return PrometheusText::renderNamed(r, NamedScratch{live, order}, std::span<char>(buf.data(), outSize));
    }
    std::string text(const RenderResult& rr) const { return std::string(buf.data(), rr.length); }
};
}  // namespace

TEST(PrometheusNamed, CounterSlotsSumIntoOneSeries) {
    Env env(16);
    env.publish({7, "a_total", "A things.", FamilyType::Counter});
    env.slot(7, MetricKind::Counter, "x=\"1\"").add(3);
    env.slot(7, MetricKind::Counter, "x=\"1\"").add(4);
    env.slot(7, MetricKind::Counter, "x=\"2\"").add(5);
    const RenderResult rr = env.render();
    EXPECT_FALSE(rr.overflow);
    EXPECT_EQ(rr.skipped, 0u);
    EXPECT_EQ(env.text(rr),
              "# HELP a_total A things.\n# TYPE a_total counter\na_total{x=\"1\"} 7\na_total{x=\"2\"} 5\n");
}

TEST(PrometheusNamed, GaugeMergeMax) {
    Env env(16);
    env.publish({8, "b_age_seconds", "Age.", FamilyType::Gauge, Merge::Max});
    env.slot(8, MetricKind::Gauge, "").set(3);
    env.slot(8, MetricKind::Gauge, "").set(9);
    const RenderResult rr = env.render();
    EXPECT_EQ(env.text(rr), "# HELP b_age_seconds Age.\n# TYPE b_age_seconds gauge\nb_age_seconds 9\n");
}

TEST(PrometheusNamed, GaugeMergeSum) {
    Env env(16);
    env.publish({8, "b_age_seconds", "Age.", FamilyType::Gauge, Merge::Sum});
    env.slot(8, MetricKind::Gauge, "").set(3);
    env.slot(8, MetricKind::Gauge, "").set(9);
    EXPECT_EQ(env.text(env.render()), "# HELP b_age_seconds Age.\n# TYPE b_age_seconds gauge\nb_age_seconds 12\n");
}

TEST(PrometheusNamed, EmptyHelpPrintsNoHelpLine) {
    Env env(16);
    env.publish({8, "b", "", FamilyType::Gauge});
    env.slot(8, MetricKind::Gauge, "").set(1);
    EXPECT_EQ(env.text(env.render()), "# TYPE b gauge\nb 1\n");
}

TEST(PrometheusNamed, FamiliesInTypeIdOrder) {
    Env env(16);
    env.publish({9, "nine_total", "Nine.", FamilyType::Counter});
    env.publish({7, "seven_total", "Seven.", FamilyType::Counter});
    env.slot(9, MetricKind::Counter, "").add(1);
    env.slot(7, MetricKind::Counter, "").add(2);
    EXPECT_EQ(env.text(env.render()),
              "# HELP seven_total Seven.\n# TYPE seven_total counter\nseven_total 2\n"
              "# HELP nine_total Nine.\n# TYPE nine_total counter\nnine_total 1\n");
}

TEST(PrometheusNamed, UndescribedSlotsPrintGeneric) {
    Env env(16);
    env.publish({7, "a_total", "A.", FamilyType::Counter});
    env.slot(7, MetricKind::Counter, "").add(1);
    SlotWriter c = env.slot(42, MetricKind::Counter, "x");
    c.add(6);
    MetricsReader r(env.file);
    int64_t regId = -1;
    r.forEachLive([&](const Live& l) { if (l.typeId == 42) regId = l.regId; });
    const RenderResult rr = env.render();
    EXPECT_EQ(env.text(rr),
              "# HELP a_total A.\n# TYPE a_total counter\na_total 1\n"
              "# HELP metrics_counter Monotonic total of a slot, by type id, registration id and label.\n"
              "# TYPE metrics_counter counter\n"
              "metrics_counter{type=\"42\",reg_id=\"" + std::to_string(regId) + "\",label=\"x\"} 6\n");
    // The same line render() prints.
    const auto rates = metrics::testing::makeRates<16, 4>(r);
    std::vector<char> plain(1 << 16);
    const RenderResult p = PrometheusText::render(r, *rates, std::span<char>(plain));
    const std::string line = "metrics_counter{type=\"42\",reg_id=\"" + std::to_string(regId) + "\",label=\"x\"} 6\n";
    EXPECT_NE(std::string(plain.data(), p.length).find(line), std::string::npos);
}

TEST(PrometheusNamed, UndescribedGaugePrintsGeneric) {
    Env env(16);
    env.slot(5, MetricKind::Gauge, "g").set(-4);
    const std::string t = env.text(env.render());
    EXPECT_NE(t.find("# TYPE metrics_gauge gauge\nmetrics_gauge{type=\"5\""), std::string::npos);
    EXPECT_EQ(t.find("metrics_counter"), std::string::npos);
}

TEST(PrometheusNamed, DescriptorsAreNeverPrinted) {
    Env env(16);
    env.publish({7, "a_total", "A.", FamilyType::Counter});
    const RenderResult rr = env.render();
    EXPECT_EQ(env.text(rr).find("1000306"), std::string::npos);
    EXPECT_EQ(env.text(rr).find("metrics_"), std::string::npos);
}

TEST(PrometheusNamed, LabelModeNonePrintsNoLabel) {
    Env env(16);
    env.publish({3, "n_bytes", "N.", FamilyType::Gauge, Merge::Sum, LabelMode::None});
    env.slot(3, MetricKind::Gauge, "n_bytes").set(11);
    EXPECT_EQ(env.text(env.render()), "# HELP n_bytes N.\n# TYPE n_bytes gauge\nn_bytes 11\n");
}

namespace {
bool hasLine(const std::string& t, const std::string& line) {
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        if (t.compare(p, e - p, line) == 0 && e - p == line.size()) return true;
        p = e + 1;
    }
    return false;
}
bool hasNumberLine(const std::string& t, const std::string& name) {
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        const std::string l = t.substr(p, e - p);
        if (l.size() > name.size() + 1 && l.compare(0, name.size() + 1, name + " ") == 0 &&
            l.find_first_not_of("0123456789", name.size() + 1) == std::string::npos) {
            return true;
        }
        p = e + 1;
    }
    return false;
}
}  // namespace

// Portable: the SystemMetrics slots written by hand (label = the metric name), as SystemMetrics::init does.
TEST(PrometheusNamed, SystemMetricsFamiliesPrintWithoutLabel) {
    Env env(32);
    ASSERT_EQ(publishFamilies(env.w, kSystemMetricsFamilies), FamilyStatus::Ok);
    env.slot(1'000'302, MetricKind::Gauge, "process_threads").set(3);
    env.slot(1'000'305, MetricKind::Counter, "process_metrics_read_errors_total").add(2);
    const std::string t = env.text(env.render());
    EXPECT_TRUE(hasLine(t, "# TYPE process_threads gauge")) << t;
    EXPECT_TRUE(hasLine(t, "process_threads 3")) << t;
    EXPECT_TRUE(hasLine(t, "# TYPE process_metrics_read_errors_total counter")) << t;
    EXPECT_TRUE(hasLine(t, "process_metrics_read_errors_total 2")) << t;
    EXPECT_EQ(t.find("metrics_gauge"), std::string::npos);
}

#ifdef __linux__
namespace {
const std::byte kSysKey[1] = {std::byte{0}};
}  // namespace

TEST(PrometheusNamed, SystemMetricsNamed) {
    Env env(32);
    ASSERT_EQ(publishFamilies(env.w, kSystemMetricsFamilies), FamilyStatus::Ok);
    SystemMetrics sm;
    ASSERT_EQ(sm.init(env.w, SystemMetricsParams{1'000'000'000, "/proc/self"}, kSysKey), SystemMetrics::Status::Ok);
    EXPECT_EQ(sm.doWork(0), 1);
    const std::string t = env.text(env.render());
    EXPECT_NE(t.find("# TYPE process_threads gauge\n"), std::string::npos);
    EXPECT_TRUE(hasNumberLine(t, "process_threads")) << t;
}

TEST(PrometheusNamed, SystemMetricsNamedFakeProc) {
    Env env(32);
    const std::string root = env.dir.path("proc");
    std::filesystem::create_directories(root + "/fd");
    std::ofstream(root + "/statm") << "10 5 0 0 0 0 0\n";
    std::ofstream(root + "/status") << "Name:\tx\nThreads:\t3\n";
    std::ofstream(root + "/fd/a") << "";
    ASSERT_EQ(publishFamilies(env.w, kSystemMetricsFamilies), FamilyStatus::Ok);
    SystemMetrics sm;
    ASSERT_EQ(sm.init(env.w, SystemMetricsParams{1'000'000'000, root.c_str()}, kSysKey), SystemMetrics::Status::Ok);
    EXPECT_EQ(sm.doWork(0), 1);
    const std::string t = env.text(env.render());
    EXPECT_TRUE(hasLine(t, "process_threads 3")) << t;
    EXPECT_TRUE(hasLine(t, "process_metrics_read_errors_total 0")) << t;
    EXPECT_EQ(t.find("metrics_gauge"), std::string::npos);
}
#endif

TEST(PrometheusNamed, BadLabelSkipped) {
    Env env(16);
    env.publish({7, "a_total", "A.", FamilyType::Counter});
    env.slot(7, MetricKind::Counter, "x=\"1\"}").add(1);
    env.slot(7, MetricKind::Counter, "y=\"2\"").add(2);
    const RenderResult rr = env.render();
    EXPECT_EQ(rr.skipped, 1u);
    EXPECT_EQ(env.text(rr), "# HELP a_total A.\n# TYPE a_total counter\na_total{y=\"2\"} 2\n");
}

TEST(PrometheusNamed, FreedSlotNotPrinted) {
    Env env(16);
    env.publish({7, "a_total", "A.", FamilyType::Counter});
    SlotWriter a = env.slot(7, MetricKind::Counter, "x=\"1\"");
    a.add(1);
    SlotWriter b = env.slot(7, MetricKind::Counter, "x=\"2\"");
    b.add(2);
    ASSERT_EQ(env.w.free(a.slot()), FreeStatus::Ok);
    EXPECT_EQ(env.text(env.render()), "# HELP a_total A.\n# TYPE a_total counter\na_total{x=\"2\"} 2\n");
}

TEST(PrometheusNamed, ScratchShort) {
    Env env(16, 2);
    env.slot(1, MetricKind::Counter, "a");
    env.slot(1, MetricKind::Counter, "b");
    env.slot(1, MetricKind::Counter, "c");
    const RenderResult rr = env.render();
    EXPECT_TRUE(rr.scratchShort);
    EXPECT_TRUE(rr.overflow);
    // Exactly as many slots as scratch is fine.
    Env ok(16, 3);
    ok.slot(1, MetricKind::Counter, "a");
    ok.slot(1, MetricKind::Counter, "b");
    ok.slot(1, MetricKind::Counter, "c");
    const RenderResult r2 = ok.render();
    EXPECT_FALSE(r2.scratchShort);
    EXPECT_FALSE(r2.overflow);
}

TEST(PrometheusNamed, BufferOverflow) {
    Env env(16);
    env.publish({7, "a_total", "A things that are long enough.", FamilyType::Counter});
    env.slot(7, MetricKind::Counter, "x=\"1\"").add(1);
    const RenderResult rr = env.render(16);
    EXPECT_TRUE(rr.overflow);
    EXPECT_LE(rr.length, 16u);
    EXPECT_FALSE(rr.scratchShort);
}

TEST(PrometheusNamed, NoAllocation) {
    Env env(128, 128);
    env.publish({7, "a_total", "A.", FamilyType::Counter});
    env.publish({8, "g_level", "G.", FamilyType::Gauge, Merge::Max});
    for (int i = 0; i < 40; ++i) env.slot(7, MetricKind::Counter, "x=\"" + std::to_string(i % 25) + "\"").add(i);
    for (int i = 0; i < 10; ++i) env.slot(8, MetricKind::Gauge, "").set(i);
    env.slot(99, MetricKind::Counter, "u").add(1);
    MetricsReader r(env.file);
    RenderResult rr{};
    {
        metrics::testing::AllocScope scope;
        rr = PrometheusText::renderNamed(r, NamedScratch{env.live, env.order}, std::span<char>(env.buf));
        EXPECT_EQ(scope.count(), 0);
    }
    EXPECT_FALSE(rr.overflow);
    EXPECT_GT(rr.length, 0u);
}

TEST(PrometheusNamed, DuplicateNameAcrossPublishesPrintedOnce) {
    Env env(16);
    env.publish({7, "dup_total", "First.", FamilyType::Counter});
    env.publish({9, "dup_total", "Second.", FamilyType::Counter});
    env.slot(7, MetricKind::Counter, "x=\"1\"").add(2);
    env.slot(9, MetricKind::Counter, "x=\"2\"").add(5);
    env.slot(9, MetricKind::Counter, "x=\"3\"").add(6);
    const RenderResult rr = env.render();
    EXPECT_FALSE(rr.overflow);
    EXPECT_EQ(rr.skipped, 2u);
    EXPECT_EQ(env.text(rr), "# HELP dup_total First.\n# TYPE dup_total counter\ndup_total{x=\"1\"} 2\n");
}

TEST(PrometheusNamed, UnterminatedLabelSkipped) {
    Env env(16);
    env.publish({7, "a_total", "A.", FamilyType::Counter});
    env.slot(7, MetricKind::Counter, "x=\"a").add(2);
    env.slot(7, MetricKind::Counter, "x=\"b\"").add(3);
    const RenderResult rr = env.render();
    EXPECT_EQ(rr.skipped, 1u);
    EXPECT_EQ(env.text(rr), "# HELP a_total A.\n# TYPE a_total counter\na_total{x=\"b\"} 3\n");
}
