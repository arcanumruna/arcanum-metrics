// Writer / allocator / reader stress. Run under TSan: every cross-thread word goes through std::atomic_ref.
//
// Threads (the library starts none; the test owns them):
//  - 4 writer threads, each owns 8 slots and adds 1 to each, kWriterIterations times;
//  - the allocator thread (owner of MetricsWriter) frees and reallocates one block of 8 slots kBlockCycles times,
//    each cycle under a new owner tag, writing tag * 1e9 + k into the block's slots;
//  - a reader thread scans continuously until the "done" slot, written by the main thread, reads 1; the main
//    thread stores "joined" first, and the reader then has a bounded number of scans to see "done".
// The reader never reads key or label bytes: those are plain bytes by design and are only valid until reuse.
#include "TestDir.h"
#include "TestHelpers.h"

#include "metrics/MetricsFile.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace metrics;

namespace {
constexpr int kWriters = 4;
constexpr int kSlotsPerWriter = 8;
constexpr int64_t kWriterIterations = 1'000'000;
constexpr int kBlockSlots = 8;
constexpr int kBlockCycles = 10'000;
constexpr int kUpdatesPerCycle = 3;
constexpr int64_t kRegIdScale = 1'000'000'000;
constexpr int64_t kFirstBlockRegId = 1000;
constexpr int kBlockFirstSlot = kWriters * kSlotsPerWriter;
constexpr int kJoinedSlot = kBlockFirstSlot + kBlockSlots;  // main: writers and allocator have been joined
constexpr int kDoneSlot = kJoinedSlot + 1;                  // main: stored right after "joined"
constexpr int kTotalSlots = kDoneSlot + 1;
constexpr uint64_t kSeed = 0x5EEDC0DEULL;
// Hard caps: a stuck run fails in seconds instead of hanging. A normal run needs about 25 thousand scans before
// "joined" (the writers take seconds); once "joined" is visible, "done" must follow within kScansAfterJoined.
constexpr int64_t kMaxScansBeforeJoined = 400'000;  // about 16x the scans of a normal TSan run
constexpr int64_t kScansAfterJoined = 10'000;

struct ReaderResult {
    int64_t scans = 0;
    int64_t violations = 0;
    int64_t blockTagsSeen = 0;
    int64_t reusedSeen = 0;
    bool timedOut = false;     // never saw "joined" within the cap
    bool missedDone = false;   // saw "joined" but not "done" within kScansAfterJoined scans
};

void readerLoop(const MetricsFile& file, ReaderResult& out) {
    MetricsReader reader(file);
    struct Last { int64_t regId = -1; int64_t value = 0; };
    std::array<Last, kTotalSlots> last{};
    int64_t lastBlockTag = -1;
    int64_t scansSinceJoined = -1;  // -1: not joined yet
    for (int64_t scan = 0;; ++scan) {
        if (scansSinceJoined < 0 && scan >= kMaxScansBeforeJoined) { out.timedOut = true; return; }
        if (scansSinceJoined >= kScansAfterJoined) { out.missedDone = true; return; }
        if (scansSinceJoined >= 0) ++scansSinceJoined;
        ++out.scans;
        bool done = false;
        for (int32_t s = 0; s < kTotalSlots; ++s) {
            const SlotView v = reader.read(s);
            if (std::holds_alternative<Reused>(v)) { ++out.reusedSeen; continue; }
            const Live* l = std::get_if<Live>(&v);
            if (l == nullptr) continue;
            Last& lv = last[static_cast<size_t>(s)];
            if (lv.regId == l->regId && l->value < lv.value) ++out.violations;  // monotonic per regId
            lv.regId = l->regId;
            lv.value = l->value;
            if (s < kBlockFirstSlot) {
                if (l->value < 0 || l->value > kWriterIterations) ++out.violations;
            } else if (s < kJoinedSlot) {
                // the block writes ownerTag * 1e9 + k; 0 is the zeroed value before its first update
                if (l->value != 0 && (l->value / kRegIdScale != l->ownerTag ||
                                      l->value % kRegIdScale > kUpdatesPerCycle)) {
                    ++out.violations;
                }
                if (l->ownerTag != lastBlockTag) { ++out.blockTagsSeen; lastBlockTag = l->ownerTag; }
            } else if (s == kJoinedSlot) {
                if (l->value == 1 && scansSinceJoined < 0) scansSinceJoined = 0;
            } else if (l->value == 1) {
                done = true;
            }
        }
        if (done) return;
    }
}
}  // namespace

TEST(MetricsConcurrencyRegression, WriterReaderAllocatorUnderTsanRegression) {
    metrics::testing::TestDir dir;
    const std::string path = dir.path("c.dat");
    MetricsFile file;
    CreateParams cp{path.c_str(), kTotalSlots, kTotalSlots, 1, 1, {}};
    ASSERT_EQ(MetricsFile::create(cp, file), FileStatus::Ok);
    std::vector<int32_t> stack(kTotalSlots + 1);
    MetricsWriter alloc = metrics::testing::makeWriter(file, stack);

    // Allocation order fixes the slot numbers: writer slots 0..31, block 32..39, done 40. The block is
    // allocated by the allocator thread below; here the writer slots and "done" are allocated, then the block
    // is allocated and freed once so its slots exist at the right numbers.
    std::array<std::array<SlotWriter, kSlotsPerWriter>, kWriters> mine;
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < kSlotsPerWriter; ++i) {
            const AllocResult r = alloc.allocate(10 + w, MetricKind::Counter, -1, {}, "w");
            ASSERT_EQ(r.status, AllocStatus::Ok);
            mine[static_cast<size_t>(w)][static_cast<size_t>(i)] = r.writer;
        }
    }
    std::array<SlotWriter, kBlockSlots> block0;
    for (int i = 0; i < kBlockSlots; ++i) {
        const AllocResult r = alloc.allocate(20, MetricKind::Gauge, kFirstBlockRegId - 1, {}, "b");  // owner tag
        ASSERT_EQ(r.status, AllocStatus::Ok);
        block0[static_cast<size_t>(i)] = r.writer;
    }
    const AllocResult joinedR = alloc.allocate(31, MetricKind::Gauge, -1, {}, "joined");
    ASSERT_EQ(joinedR.writer.slot(), kJoinedSlot);
    const AllocResult doneR = alloc.allocate(30, MetricKind::Gauge, -1, {}, "done");
    ASSERT_EQ(doneR.status, AllocStatus::Ok);
    ASSERT_EQ(doneR.writer.slot(), kDoneSlot);
    for (const SlotWriter& b : block0) ASSERT_EQ(alloc.free(b.slot()), FreeStatus::Ok);

    ReaderResult result;
    std::thread reader([&] { readerLoop(file, result); });

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            std::mt19937_64 rng(kSeed + static_cast<uint64_t>(w));
            const size_t rot = static_cast<size_t>(rng() % kSlotsPerWriter);  // seeded start of the slot rotation
            auto& slots = mine[static_cast<size_t>(w)];
            for (int64_t it = 0; it < kWriterIterations; ++it) {
                for (size_t k = 0; k < kSlotsPerWriter; ++k) slots[(k + rot) % kSlotsPerWriter].add(1);
            }
        });
    }
    std::thread allocator([&] {
        for (int cycle = 0; cycle < kBlockCycles; ++cycle) {
            const int64_t tag = kFirstBlockRegId + cycle;  // shared by the block; each slot still gets its own regId
            std::array<SlotWriter, kBlockSlots> b;
            for (int i = 0; i < kBlockSlots; ++i) {
                const AllocResult r = alloc.allocate(20, MetricKind::Gauge, tag, {}, "b");
                if (r.status != AllocStatus::Ok) { ADD_FAILURE() << "block allocate failed"; return; }
                b[static_cast<size_t>(i)] = r.writer;
            }
            for (int k = 1; k <= kUpdatesPerCycle; ++k) {
                for (const SlotWriter& s : b) s.set(tag * kRegIdScale + k);
            }
            for (const SlotWriter& s : b) {
                if (alloc.free(s.slot()) != FreeStatus::Ok) { ADD_FAILURE() << "block free failed"; return; }
            }
        }
    });

    for (std::thread& t : writers) t.join();
    allocator.join();
    joinedR.writer.set(1);  // main is the single writer of both slots
    doneR.writer.set(1);
    reader.join();

    RecordProperty("reader_scans", std::to_string(result.scans));
    EXPECT_FALSE(result.timedOut) << "the reader never saw 'joined' in " << result.scans << " scans";
    EXPECT_FALSE(result.missedDone) << "'done' was not visible " << kScansAfterJoined << " scans after 'joined'";
    EXPECT_EQ(result.violations, 0);
    EXPECT_GT(result.scans, 0);
    EXPECT_GT(result.blockTagsSeen, 0) << "the reader never overlapped the allocator: vacuous run";
    MetricsReader check(file);
    for (int s = 0; s < kBlockFirstSlot; ++s) {
        const SlotView v = check.read(s);
        ASSERT_TRUE(std::holds_alternative<Live>(v)) << "slot " << s;
        EXPECT_EQ(std::get<Live>(v).value, kWriterIterations) << "slot " << s;
    }
}
