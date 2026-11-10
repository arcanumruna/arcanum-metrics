// rune#908: SystemMetrics.h is included by Rune's BrokerNode.h, so it must compile where there is no /proc and no
// getdents64 (macOS).  This TU forces the non-/proc build on any host and poisons the Linux-only syscall number:
// an ungated use of it in the header's code is a compile error here.  The poison comes after every other header: on
// Linux, libstdc++'s <atomic> includes <sys/syscall.h> itself, which defines (not uses) the name.
#define METRICS_SYSTEM_METRICS_PROC 0
#include "metrics/MetricsFile.h"
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsWriter.h"
#include "TestDir.h"
#include "TestHelpers.h"

#include <fcntl.h>
#include <unistd.h>

#undef SYS_getdents64
#pragma GCC poison SYS_getdents64
#include "metrics/SystemMetrics.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace metrics;

TEST(SystemMetricsPortableRegression, InitReportsUnsupportedAndAllocatesNothingRegression) {
    static_assert(!SystemMetrics::kProcSupported);
    metrics::testing::TestDir dir;
    MetricsFile file;
    std::vector<int32_t> stack(65);
    const std::string p = dir.path("s.dat");
    CreateParams cp{p.c_str(), 64, 64, 1, 1, {}};
    ASSERT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
    MetricsWriter w = metrics::testing::makeWriter(file, stack);
    const std::byte key[1] = {std::byte{1}};

    SystemMetrics sm;
    // Even a real /proc root is refused: without the /proc build there is nothing to sample with.
    EXPECT_EQ(sm.init(w, SystemMetricsParams{1'000'000'000, "/proc/self"}, key), SystemMetrics::Status::Unsupported);
    EXPECT_EQ(sm.procDirFd(), -1);
    EXPECT_EQ(sm.doWork(5'000'000'000), 0) << "an unsupported SystemMetrics never samples";
    // No slot was taken: the next allocation still gets slot 0.
    const AllocResult r = w.allocate(SystemMetrics::kTypeOpenFds, MetricKind::Gauge, -1, key, "probe");
    ASSERT_EQ(r.status, AllocStatus::Ok);
    EXPECT_EQ(r.writer.slot(), 0);
}
