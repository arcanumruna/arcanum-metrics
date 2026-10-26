#include "metrics/MetricsLayout.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace metrics;

TEST(MetricsLayoutRegression, OffsetsTable) {
    EXPECT_EQ(kLayoutVersion, 2);
    EXPECT_EQ(kHeaderLength, 4096);
    EXPECT_EQ(kMetadataRecordLength, 512);
    EXPECT_EQ(kValueRecordLength, 128);
    EXPECT_EQ(kMaxKeyLength, 112);
    EXPECT_EQ(kMaxLabelLength, 364);

    // header
    EXPECT_EQ(kHdrLayoutVersionOffset, 0);
    EXPECT_EQ(kHdrPadOffset, 4);
    EXPECT_EQ(kHdrPidOffset, 8);
    EXPECT_EQ(kHdrStartEpochNsOffset, 16);
    EXPECT_EQ(kHdrMaxSlotsOffset, 24);
    EXPECT_EQ(kHdrMetadataRecordLengthOffset, 28);
    EXPECT_EQ(kHdrValueRecordLengthOffset, 32);
    EXPECT_EQ(kHdrUserTagOffset, 36);
    EXPECT_EQ(kHdrUserTagLength, 4060);
    EXPECT_EQ(kHdrUserTagOffset + kHdrUserTagLength, kHeaderLength);

    // metadata record
    EXPECT_EQ(kMetaStateOffset, 0);
    EXPECT_EQ(kMetaTypeIdOffset, 4);
    EXPECT_EQ(kMetaRegIdOffset, 8);
    EXPECT_EQ(kMetaOwnerTagOffset, 16);
    EXPECT_EQ(kMetaKindOffset, 24);
    EXPECT_EQ(kMetaKeyLengthOffset, 28);
    EXPECT_EQ(kMetaKeyOffset, 32);
    EXPECT_EQ(kMetaLabelLengthOffset, 144);
    EXPECT_EQ(kMetaLabelOffset, 148);
    EXPECT_EQ(kMetaLabelOffset + kMaxLabelLength, kMetadataRecordLength);

    // value record
    EXPECT_EQ(kValueOffset, 0);
    EXPECT_EQ(kValueRecordLength % 64, 0);
    for (int32_t slot : {0, 1, 2, 1023}) {
        const FileLayout l{1024};
        EXPECT_EQ((l.valuesOffset() + int64_t{slot} * kValueRecordLength) % 64, 0);
    }

    EXPECT_EQ(static_cast<int32_t>(SlotState::Unused), 0);
    EXPECT_EQ(static_cast<int32_t>(SlotState::Allocated), 1);
    EXPECT_EQ(static_cast<int32_t>(SlotState::Freed), -1);
    EXPECT_EQ(static_cast<int32_t>(MetricKind::Counter), 1);
    EXPECT_EQ(static_cast<int32_t>(MetricKind::Gauge), 2);
}

TEST(MetricsLayoutRegression, RequiredSlotsTable) {
    EXPECT_EQ(requiredSlots({0, 0, 0, 0, 0, 0}), 0);
    EXPECT_EQ(requiredSlots({10, 0, 5, 3, 7, 16}), 26);
    // one namespace: 10 + 1 * (5 * 3 + 7) + 16
    EXPECT_EQ(requiredSlots({10, 1, 5, 3, 7, 16}), 48);
    // 1024 namespaces x 3 threads: 40 + 1024 * (6 * 3 + 4) + 16
    EXPECT_EQ(requiredSlots({40, 1024, 6, 3, 4, 16}), 40 + 1024 * 22 + 16);
    // product passes INT32_MAX: 70000 * (70000 * 1 + 0) = 4.9e9
    const int64_t big = requiredSlots({0, 70000, 70000, 1, 0, 16});
    EXPECT_EQ(big, int64_t{70000} * 70000 + 16);
    EXPECT_GT(big, std::numeric_limits<int32_t>::max());
}

TEST(MetricsLayoutRegression, FileLengthTable) {
    EXPECT_EQ((FileLayout{0}.valuesOffset()), 4096);
    EXPECT_EQ((FileLayout{0}.fileLength()), 4096);
    EXPECT_EQ((FileLayout{1}.valuesOffset()), 4096 + 512);
    EXPECT_EQ((FileLayout{1}.fileLength()), 4096 + 512 + 128);
    EXPECT_EQ((FileLayout{1024}.metadataOffset()), 4096);
    EXPECT_EQ((FileLayout{1024}.valuesOffset()), 4096 + 1024 * 512);
    EXPECT_EQ((FileLayout{1024}.fileLength()), 4096 + 1024 * 512 + 1024 * 128);
    EXPECT_EQ((FileLayout{1024}.fileLength()), 4096 + 1024 * 640);
}
