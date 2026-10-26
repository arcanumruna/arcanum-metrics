#pragma once
// On-disk layout of a metrics file, mirroring Aeron CountersManager.
// Little-endian. [header 4 KiB][maxSlots x 512 B metadata][maxSlots x 128 B values].
#include <bit>
#include <cstdint>

static_assert(std::endian::native == std::endian::little, "metrics file layout is little-endian");

namespace metrics {

inline constexpr int32_t kLayoutVersion = 2;
inline constexpr int32_t kHeaderLength = 4096;
inline constexpr int32_t kMetadataRecordLength = 512;
inline constexpr int32_t kValueRecordLength = 128;
inline constexpr int32_t kMaxKeyLength = 112;
inline constexpr int32_t kMaxLabelLength = 364;

// Header: layoutVersion is written last with a release store.
inline constexpr int32_t kHdrLayoutVersionOffset = 0;  // i32
inline constexpr int32_t kHdrPadOffset = 4;            // i32
inline constexpr int32_t kHdrPidOffset = 8;            // i64
inline constexpr int32_t kHdrStartEpochNsOffset = 16;  // i64
inline constexpr int32_t kHdrMaxSlotsOffset = 24;      // i32
inline constexpr int32_t kHdrMetadataRecordLengthOffset = 28;  // i32
inline constexpr int32_t kHdrValueRecordLengthOffset = 32;     // i32
inline constexpr int32_t kHdrUserTagOffset = 36;
inline constexpr int32_t kHdrUserTagLength = kHeaderLength - kHdrUserTagOffset;

// Metadata record.
inline constexpr int32_t kMetaStateOffset = 0;        // i32 SlotState
inline constexpr int32_t kMetaTypeIdOffset = 4;       // i32
inline constexpr int32_t kMetaRegIdOffset = 8;        // i64
inline constexpr int32_t kMetaOwnerTagOffset = 16;    // i64
inline constexpr int32_t kMetaKindOffset = 24;        // i32 MetricKind
inline constexpr int32_t kMetaKeyLengthOffset = 28;   // i32
inline constexpr int32_t kMetaKeyOffset = 32;         // kMaxKeyLength bytes
inline constexpr int32_t kMetaLabelLengthOffset = kMetaKeyOffset + kMaxKeyLength;  // i32
inline constexpr int32_t kMetaLabelOffset = kMetaLabelLengthOffset + 4;            // kMaxLabelLength bytes

// Value record: i64 value at offset 0, padded to two cache lines.
inline constexpr int32_t kValueOffset = 0;

static_assert(kHdrLayoutVersionOffset == 0 && kHdrPadOffset == 4 && kHdrPidOffset == 8 &&
              kHdrStartEpochNsOffset == 16 && kHdrMaxSlotsOffset == 24 &&
              kHdrMetadataRecordLengthOffset == 28 && kHdrValueRecordLengthOffset == 32 &&
              kHdrUserTagOffset == 36 && kHdrUserTagLength == 4060);
static_assert(kHdrPidOffset % 8 == 0 && kHdrStartEpochNsOffset % 8 == 0);
static_assert(kMetaStateOffset == 0 && kMetaTypeIdOffset == 4 && kMetaRegIdOffset == 8 &&
              kMetaOwnerTagOffset == 16 && kMetaKindOffset == 24 && kMetaKeyLengthOffset == 28 &&
              kMetaKeyOffset == 32 && kMetaLabelLengthOffset == 144 && kMetaLabelOffset == 148);
static_assert(kMetaRegIdOffset % 8 == 0 && kMetaOwnerTagOffset % 8 == 0);
static_assert(kMetaLabelOffset + kMaxLabelLength == kMetadataRecordLength,
              "the last metadata field must end exactly at the record length");
static_assert(kValueRecordLength == 128 && kValueRecordLength % 64 == 0 && kHeaderLength % 64 == 0);
static_assert(kMetadataRecordLength % 64 == 0);

enum class SlotState : int32_t { Unused = 0, Allocated = 1, Freed = -1 };
enum class MetricKind : int32_t { Counter = 1, Gauge = 2 };

struct FileLayout {  // computed, never stored separately
    int32_t maxSlots;
    [[nodiscard]] constexpr int64_t metadataOffset() const noexcept { return kHeaderLength; }
    [[nodiscard]] constexpr int64_t valuesOffset() const noexcept {
        return kHeaderLength + int64_t{maxSlots} * kMetadataRecordLength;
    }
    [[nodiscard]] constexpr int64_t fileLength() const noexcept {
        return valuesOffset() + int64_t{maxSlots} * kValueRecordLength;
    }
};

struct CapacityPlan {
    int32_t nodeMetrics, maxNamespaces, perThreadNsMetrics, threads, perNodeNsMetrics, headroom;
};

// int64: the product can pass INT32_MAX; the caller checks and refuses.
[[nodiscard]] constexpr int64_t requiredSlots(const CapacityPlan& p) noexcept {
    return int64_t{p.nodeMetrics} +
           int64_t{p.maxNamespaces} *
               (int64_t{p.perThreadNsMetrics} * p.threads + int64_t{p.perNodeNsMetrics}) +
           int64_t{p.headroom};
}

}  // namespace metrics
