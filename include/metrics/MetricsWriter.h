#pragma once
// Owner-thread allocator and single-writer slot handles. No allocation, no exceptions, no locks.
//  - SlotWriter: one writer thread per slot; plain load of its own value, add, release store.
//  - MetricsWriter: allocate and free run on one owner thread (the allocator's), never concurrently.
#include "metrics/MetricsFile.h"
#include "metrics/MetricsLayout.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace metrics {

enum class AllocStatus : int32_t { Ok, Full, KeyTooLong, LabelTooLong, InvalidType, InvalidRegId };
enum class FreeStatus : int32_t { Ok, NotAllocated, OutOfRange };

class SlotWriter {  // trivially copyable value type; exactly one writer thread per slot
public:
    SlotWriter() noexcept = default;
    SlotWriter(int64_t* value, int32_t slot) noexcept : value_(value), slot_(slot) {}

    void add(int64_t n) const noexcept {
#ifdef METRICS_TEST_PLAIN_STORE  // mutation switch for the TSan stress test: a non-atomic store
        *value_ = static_cast<int64_t>(static_cast<uint64_t>(*value_) + static_cast<uint64_t>(n));
        return;
#endif
        std::atomic_ref<int64_t> ref(*value_);
        // Unsigned arithmetic: wraps in two's complement instead of signed-overflow UB.
        ref.store(static_cast<int64_t>(static_cast<uint64_t>(ref.load(std::memory_order_relaxed)) +
                                       static_cast<uint64_t>(n)),
                  std::memory_order_release);
    }
    void set(int64_t v) const noexcept {
#ifdef METRICS_TEST_PLAIN_STORE
        *value_ = v;
        return;
#endif
        std::atomic_ref<int64_t>(*value_).store(v, std::memory_order_release);
    }
    [[nodiscard]] int64_t get() const noexcept {
        return std::atomic_ref<int64_t>(*value_).load(std::memory_order_relaxed);
    }
    [[nodiscard]] int32_t slot() const noexcept { return slot_; }

private:
    int64_t* value_ = nullptr;
    int32_t slot_ = -1;
};

struct AllocResult {
    AllocStatus status;
    SlotWriter writer;
    int64_t regId;
};

// Publication steps, for deterministic tests (a hook called between the stores). The default hook is empty.
enum class AllocStep : int32_t { Metadata, Zeroed, RegId, Published };
struct NoHook {
    void operator()(AllocStep, int32_t) const noexcept {}
};

template <class Hook = NoHook>
class BasicMetricsWriter {  // owner thread only
public:
    // freeStackStorage holds the free slots; capacity is min(maxSlots, storage size).
    explicit BasicMetricsWriter(MetricsFile& file, std::span<int32_t> freeStackStorage, Hook hook = Hook{}) noexcept
        : base_(file.base()),
          layout_(file.layout()),
          capacity_(static_cast<int32_t>(
              std::min<int64_t>(file.layout().maxSlots, static_cast<int64_t>(freeStackStorage.size())))),
          freeStack_(freeStackStorage.data()),
          hook_(hook) {}

    [[nodiscard]] AllocResult allocate(int32_t typeId, MetricKind kind, int64_t regId /* -1 = next */,
                                       std::span<const std::byte> key, std::string_view label) noexcept {
        // Argument checks first, in this order; none of them touches the file.
        if (typeId < 0 || (kind != MetricKind::Counter && kind != MetricKind::Gauge)) return fail(AllocStatus::InvalidType);
        if (regId < -1) return fail(AllocStatus::InvalidRegId);
        if (key.size() > static_cast<size_t>(kMaxKeyLength)) return fail(AllocStatus::KeyTooLong);
        if (label.size() > static_cast<size_t>(kMaxLabelLength)) return fail(AllocStatus::LabelTooLong);

        int32_t slot;
        if (freeCount_ > 0) {
            slot = freeStack_[freeCount_ - 1];
        } else if (nextUnused_ < capacity_) {
            slot = nextUnused_;
        } else {
            return fail(AllocStatus::Full);
        }
        if (freeCount_ > 0) --freeCount_; else ++nextUnused_;

        const int64_t id = regId == -1 ? nextRegId_++ : regId;
        std::byte* m = meta(slot);

        std::memcpy(m + kMetaKeyOffset, key.data(), key.size());
        std::memset(m + kMetaKeyOffset + key.size(), 0, kMaxKeyLength - key.size());
        std::memcpy(m + kMetaLabelOffset, label.data(), label.size());
        std::memset(m + kMetaLabelOffset + label.size(), 0, kMaxLabelLength - label.size());
        putPlain(m, kMetaKeyLengthOffset, static_cast<int32_t>(key.size()));
        putPlain(m, kMetaLabelLengthOffset, static_cast<int32_t>(label.size()));
        putPlain(m, kMetaTypeIdOffset, typeId);
        putPlain(m, kMetaKindOffset, static_cast<int32_t>(kind));
        putPlain(m, kMetaOwnerTagOffset, int64_t{0});
        hook_(AllocStep::Metadata, slot);

        int64_t* v = value(slot);
        std::atomic_ref<int64_t>(*v).store(0, std::memory_order_release);  // a new owner never inherits the total
        hook_(AllocStep::Zeroed, slot);

        ref64(m, kMetaRegIdOffset).store(id, std::memory_order_release);
        hook_(AllocStep::RegId, slot);

        ref32(m, kMetaStateOffset).store(static_cast<int32_t>(SlotState::Allocated), std::memory_order_release);
        hook_(AllocStep::Published, slot);

        ++allocated_;
        return AllocResult{AllocStatus::Ok, SlotWriter(v, slot), id};
    }

    [[nodiscard]] FreeStatus free(int32_t slot) noexcept {
        if (slot < 0 || slot >= capacity_) return FreeStatus::OutOfRange;
        std::byte* m = meta(slot);
        if (ref32(m, kMetaStateOffset).load(std::memory_order_relaxed) != static_cast<int32_t>(SlotState::Allocated)) {
            return FreeStatus::NotAllocated;
        }
        ref32(m, kMetaStateOffset).store(static_cast<int32_t>(SlotState::Freed), std::memory_order_release);
        freeStack_[freeCount_++] = slot;
        --allocated_;
        return FreeStatus::Ok;
    }

    [[nodiscard]] int32_t allocatedCount() const noexcept { return allocated_; }

private:
    static AllocResult fail(AllocStatus s) noexcept { return AllocResult{s, SlotWriter(), -1}; }
    std::byte* meta(int32_t slot) const noexcept {
        return base_ + layout_.metadataOffset() + int64_t{slot} * kMetadataRecordLength;
    }
    int64_t* value(int32_t slot) const noexcept {
        return reinterpret_cast<int64_t*>(base_ + layout_.valuesOffset() + int64_t{slot} * kValueRecordLength);
    }
    // Scalar metadata is stored relaxed through atomic_ref: a reader may scan a slot while it is reused.
    // The release store of the regId and the state publishes it.
    static void putPlain(std::byte* m, int32_t off, int32_t v) noexcept { ref32(m, off).store(v, std::memory_order_relaxed); }
    static void putPlain(std::byte* m, int32_t off, int64_t v) noexcept { ref64(m, off).store(v, std::memory_order_relaxed); }
    static std::atomic_ref<int32_t> ref32(std::byte* m, int32_t off) noexcept {
        return std::atomic_ref<int32_t>(*reinterpret_cast<int32_t*>(m + off));
    }
    static std::atomic_ref<int64_t> ref64(std::byte* m, int32_t off) noexcept {
        return std::atomic_ref<int64_t>(*reinterpret_cast<int64_t*>(m + off));
    }

    std::byte* base_;
    FileLayout layout_;
    int32_t capacity_;
    int32_t* freeStack_;
    Hook hook_;
    int32_t freeCount_ = 0;
    int32_t nextUnused_ = 0;
    int32_t allocated_ = 0;
    int64_t nextRegId_ = 1;
};

using MetricsWriter = BasicMetricsWriter<NoHook>;

}  // namespace metrics
