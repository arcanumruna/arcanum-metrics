#include "AllocCounter.h"
#include "TestDir.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"
#include "metrics/SystemMetrics.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace metrics;

namespace {

constexpr int64_t kSec = 1'000'000'000;

struct Env {
    metrics::testing::TestDir dir;
    MetricsFile file;
    std::vector<int32_t> stack;
    Env() : stack(65) {
        const std::string p = dir.path("s.dat");
        CreateParams cp{p.c_str(), 64, 64, 1, 1, {}};
        EXPECT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
    }
};

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::trunc) << text;
}

// A fake procRoot: statm, status and an fd directory with `fds` entries.
struct FakeProc {
    metrics::testing::TestDir dir;
    std::string root;
    FakeProc() {
        root = dir.path("proc");
        std::filesystem::create_directories(root + "/fd");
    }
    void statm(const std::string& t) { writeFile(root + "/statm", t); }
    void status(const std::string& t) { writeFile(root + "/status", t); }
    void fds(int n) {
        // Keep the directory itself (SystemMetrics holds it open); only change its entries.
        for (const auto& e : std::filesystem::directory_iterator(root + "/fd")) std::filesystem::remove(e.path());
        for (int i = 0; i < n; ++i) writeFile(root + "/fd/" + std::to_string(i), "");
    }
};

const std::byte kKey[4] = {std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};

int64_t gauge(const MetricsReader& r, int32_t typeId) {
    const int32_t s = r.findFirst(typeId, kKey);
    if (s < 0) return INT64_MIN;
    return std::get<Live>(r.read(s)).value;
}

int threadsOfSelf() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Threads:", 0) == 0) return std::stoi(line.substr(8));
    }
    return -1;
}

}  // namespace

TEST(SystemMetricsRegression, NoThreadRegression) {
    Env env;
    MetricsWriter w(env.file, env.stack);
    const int before = threadsOfSelf();
    ASSERT_GT(before, 0);
    SystemMetrics sm;
    ASSERT_EQ(sm.init(w, SystemMetricsParams{kSec, "/proc/self"}, kKey), SystemMetrics::Status::Ok);
    int work = 0;
    for (int i = 0; i < 100; ++i) work += sm.doWork(i * kSec);
    EXPECT_EQ(work, 100);
    EXPECT_EQ(threadsOfSelf(), before);
    MetricsReader r(env.file);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), before);
    EXPECT_GT(gauge(r, SystemMetrics::kTypeOpenFds), 2);
    EXPECT_GT(gauge(r, SystemMetrics::kTypeResidentBytes), 0);
    EXPECT_GE(gauge(r, SystemMetrics::kTypeVirtualBytes), gauge(r, SystemMetrics::kTypeResidentBytes));
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeReadErrors), 0);
}

TEST(SystemMetricsRegression, IntervalOnInjectedClockRegression) {
    Env env;
    MetricsWriter w(env.file, env.stack);
    FakeProc p;
    p.statm("10 5 0 0 0 0 0\n");
    p.status("Name:\tx\nThreads:\t3\n");
    p.fds(2);
    SystemMetrics sm;
    ASSERT_EQ(sm.init(w, SystemMetricsParams{kSec, p.root.c_str()}, kKey), SystemMetrics::Status::Ok);
    MetricsReader r(env.file);
    EXPECT_EQ(sm.doWork(0), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 3);
    p.status("Name:\tx\nThreads:\t4\n");
    EXPECT_EQ(sm.doWork(kSec / 2), 0);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 3);  // not due: unchanged
    EXPECT_EQ(sm.doWork(kSec), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 4);
    p.status("Name:\tx\nThreads:\t5\n");
    EXPECT_EQ(sm.doWork(kSec + kSec / 2), 0);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 4);
    EXPECT_EQ(sm.doWork(kSec / 4), 0);  // clock stepping backwards is not due
    EXPECT_EQ(sm.doWork(2 * kSec), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 5);
}

TEST(SystemMetricsRegression, FakeProcTable) {
    Env env;
    MetricsWriter w(env.file, env.stack);
    FakeProc p;
    const int64_t page = ::sysconf(_SC_PAGESIZE);
    p.statm("1000 200 5 6 0 7 0\n");
    p.status("Name:\tfake\nUmask:\t0022\nThreads:\t17\nVmRSS:\t1 kB\n");
    p.fds(5);
    SystemMetrics sm;
    ASSERT_EQ(sm.init(w, SystemMetricsParams{kSec, p.root.c_str()}, kKey), SystemMetrics::Status::Ok);
    MetricsReader r(env.file);
    EXPECT_EQ(sm.doWork(0), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeVirtualBytes), 1000 * page);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeResidentBytes), 200 * page);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 17);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeOpenFds), 5);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeReadErrors), 0);

    // Truncated statm and a status without Threads: gauges keep their last values, one error each.
    p.statm("1000");
    p.status("Name:\tfake\n");
    p.fds(7);
    EXPECT_EQ(sm.doWork(kSec), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeVirtualBytes), 1000 * page);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeResidentBytes), 200 * page);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 17);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeOpenFds), 7);  // the intact source still updates
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeReadErrors), 2);

    p.statm("");
    EXPECT_EQ(sm.doWork(2 * kSec), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeReadErrors), 4);

    p.statm("2000 300 0 0 0 0 0\n");
    p.status("Threads:\t2\n");
    EXPECT_EQ(sm.doWork(3 * kSec), 1);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeVirtualBytes), 2000 * page);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeThreads), 2);
    EXPECT_EQ(gauge(r, SystemMetrics::kTypeReadErrors), 4);
}

TEST(SystemMetricsRegression, InitFailuresRegression) {
    {
        Env env;
        MetricsWriter w(env.file, env.stack);
        SystemMetrics sm;
        EXPECT_EQ(sm.init(w, SystemMetricsParams{kSec, "/nonexistent/proc"}, kKey), SystemMetrics::Status::OpenFailed);
        EXPECT_EQ(w.allocatedCount(), 0);
    }
    {
        // A file with too few slots: allocation fails and nothing stays allocated.
        metrics::testing::TestDir dir;
        const std::string path = dir.path("tiny.dat");
        MetricsFile f;
        CreateParams cp{path.c_str(), 3, 3, 1, 1, {}};
        ASSERT_EQ(MetricsFile::create(cp, f), FileStatus::Ok);
        std::vector<int32_t> stack(4);
        MetricsWriter w(f, stack);
        SystemMetrics sm;
        EXPECT_EQ(sm.init(w, SystemMetricsParams{kSec, "/proc/self"}, kKey), SystemMetrics::Status::AllocFailed);
        EXPECT_EQ(w.allocatedCount(), 0);
        EXPECT_EQ(sm.doWork(0), 0);  // never initialised: no work
    }
}

TEST(SystemMetricsRegression, ZeroAllocDoWorkRegression) {
    Env env;
    MetricsWriter w(env.file, env.stack);
    FakeProc p;
    p.statm("10 5 0 0 0 0 0\n");
    p.status("Threads:\t3\n");
    p.fds(3);
    SystemMetrics fake, real;
    ASSERT_EQ(fake.init(w, SystemMetricsParams{kSec, p.root.c_str()}, kKey), SystemMetrics::Status::Ok);
    ASSERT_EQ(real.init(w, SystemMetricsParams{kSec, "/proc/self"}, kKey), SystemMetrics::Status::Ok);
    metrics::testing::AllocScope scope;
    int work = 0;
    for (int i = 0; i < 20; ++i) work += fake.doWork(i * kSec) + real.doWork(i * kSec);
    EXPECT_EQ(scope.count(), 0);
    EXPECT_EQ(work, 40);
}
