#pragma once
// Reader side: acquire loads only, never writes the file. A slot is read as state, regId, value, state, regId;
// a changed regId means the slot was reused mid-read, a state that is no longer Allocated means it was freed. Rates and windows are computed here from sampled totals
// and levels with the caller's nowNs. No allocation, no exceptions, no clock.
#include "metrics/MetricsFile.h"
#include "metrics/MetricsLayout.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <string_view>
#include <variant>

namespace metrics {

struct Empty {};
struct Live {
    int32_t slot;
    int32_t typeId;
    MetricKind kind;
    int64_t regId;
    int64_t ownerTag;
    int64_t value;
    // Point into the mapping. Valid while the slot is not reused; re-read the slot to find out.
    std::span<const std::byte> key;
    std::string_view label;
};
struct Reused {
    int32_t slot;
    int64_t oldRegId;
};
using SlotView = std::variant<Empty, Live, Reused>;

// Steps of a slot read, for deterministic tests. The default hook is empty.
enum class ReadStep : int32_t { AfterFirstRegId, AfterValue };
struct NoReadHook {
    void operator()(ReadStep, int32_t) const noexcept {}
};

template <class Hook = NoReadHook>
class BasicMetricsReader {
public:
    explicit BasicMetricsReader(const MetricsFile& file, Hook hook = Hook{}) noexcept
        : base_(const_cast<std::byte*>(file.base())), layout_(file.layout()), hook_(hook) {}

    [[nodiscard]] int32_t maxSlots() const noexcept { return layout_.maxSlots; }

    [[nodiscard]] SlotView read(int32_t slot) const noexcept {
        if (slot < 0 || slot >= layout_.maxSlots) return Empty{};
        const std::byte* m = meta(slot);
        if (ref32(m, kMetaStateOffset).load(std::memory_order_acquire) != static_cast<int32_t>(SlotState::Allocated)) {
            return Empty{};
        }
        const int64_t regId = ref64(m, kMetaRegIdOffset).load(std::memory_order_acquire);
        hook_(ReadStep::AfterFirstRegId, slot);
        const int32_t typeId = ref32(m, kMetaTypeIdOffset).load(std::memory_order_relaxed);
        const int32_t kind = ref32(m, kMetaKindOffset).load(std::memory_order_relaxed);
        const int64_t ownerTag = ref64(m, kMetaOwnerTagOffset).load(std::memory_order_relaxed);
        const int32_t keyLen = ref32(m, kMetaKeyLengthOffset).load(std::memory_order_relaxed);
        const int32_t labelLen = ref32(m, kMetaLabelLengthOffset).load(std::memory_order_relaxed);
        const int64_t value = std::atomic_ref<int64_t>(*valuePtr(slot)).load(std::memory_order_acquire);
        hook_(ReadStep::AfterValue, slot);
        // State before regId: a value of 0 seen after a free proves the free is visible here, and a state of
        // Allocated then proves the new regId is visible too. (regId first would allow Live{oldRegId, 0}.)
        if (ref32(m, kMetaStateOffset).load(std::memory_order_acquire) != static_cast<int32_t>(SlotState::Allocated)) {
            return Empty{};  // freed while reading
        }
        const int64_t regIdAgain = ref64(m, kMetaRegIdOffset).load(std::memory_order_acquire);
        if (regIdAgain != regId) return Reused{slot, regId};
        return Live{slot,
                    typeId,
                    static_cast<MetricKind>(kind),
                    regId,
                    ownerTag,
                    value,
                    std::span<const std::byte>(m + kMetaKeyOffset, clamp(keyLen, kMaxKeyLength)),
                    std::string_view(reinterpret_cast<const char*>(m + kMetaLabelOffset), clamp(labelLen, kMaxLabelLength))};
    }

    // Calls f(const Live&) for every live slot. A slot found Reused is read once more. Slots are handed out in
    // order, so the first Unused slot ends the scan.
    template <class F>
    void forEachLive(F&& f) const noexcept {
        for (int32_t s = 0; s < layout_.maxSlots; ++s) {
            if (ref32(meta(s), kMetaStateOffset).load(std::memory_order_acquire) == static_cast<int32_t>(SlotState::Unused)) {
                return;
            }
            SlotView v = read(s);
            if (std::holds_alternative<Reused>(v)) v = read(s);
            if (const Live* l = std::get_if<Live>(&v)) f(*l);
        }
    }

    // First live slot with this typeId and key, or -1.
    [[nodiscard]] int32_t findFirst(int32_t typeId, std::span<const std::byte> key) const noexcept {
        int32_t found = -1;
        for (int32_t s = 0; s < layout_.maxSlots && found < 0; ++s) {
            if (ref32(meta(s), kMetaStateOffset).load(std::memory_order_acquire) == static_cast<int32_t>(SlotState::Unused)) {
                break;
            }
            const SlotView v = read(s);
            if (const Live* l = std::get_if<Live>(&v)) {
                if (l->typeId == typeId && l->key.size() == key.size() &&
                    (key.empty() || std::memcmp(l->key.data(), key.data(), key.size()) == 0)) {
                    found = s;
                }
            }
        }
        return found;
    }

private:
    static size_t clamp(int32_t n, int32_t max) noexcept { return static_cast<size_t>(std::clamp(n, 0, max)); }
    const std::byte* meta(int32_t slot) const noexcept {
        return base_ + layout_.metadataOffset() + int64_t{slot} * kMetadataRecordLength;
    }
    int64_t* valuePtr(int32_t slot) const noexcept {
        return reinterpret_cast<int64_t*>(base_ + layout_.valuesOffset() + int64_t{slot} * kValueRecordLength);
    }
    static std::atomic_ref<int32_t> ref32(const std::byte* m, int32_t off) noexcept {
        return std::atomic_ref<int32_t>(*reinterpret_cast<int32_t*>(const_cast<std::byte*>(m) + off));
    }
    static std::atomic_ref<int64_t> ref64(const std::byte* m, int32_t off) noexcept {
        return std::atomic_ref<int64_t>(*reinterpret_cast<int64_t*>(const_cast<std::byte*>(m) + off));
    }

    std::byte* base_;
    FileLayout layout_;
    Hook hook_;
};

using MetricsReader = BasicMetricsReader<NoReadHook>;

// Per-slot rate state: one explicit state each, restarted when the regId changes.
namespace detail {
struct RateAbsent {};
struct RateFirst {
    int64_t regId;
    int64_t value;
    int64_t ns;
};
struct RateRated {
    int64_t regId;
    int64_t value;
    int64_t ns;
    double perSecond;
};
}  // namespace detail

// Window is the sampler depth shared with WindowSampler; a rate needs only the last two samples.
template <int32_t Capacity, int32_t Window>
class RateTracker {
    static_assert(Capacity > 0 && Window > 0);

public:
    template <class Reader>
    void sample(const Reader& reader, int64_t nowNs) noexcept {
        const int32_t n = std::min(Capacity, reader.maxSlots());
        for (int32_t s = 0; s < n; ++s) {
            const SlotView v = reader.read(s);
            const Live* l = std::get_if<Live>(&v);
            if (l == nullptr) {
                entries_[s] = detail::RateAbsent{};
                continue;
            }
            const detail::RateFirst first{l->regId, l->value, nowNs};
            std::visit(
                [&](const auto& prev) {
                    using T = std::decay_t<decltype(prev)>;
                    if constexpr (std::is_same_v<T, detail::RateAbsent>) {
                        entries_[s] = first;
                    } else if (prev.regId != l->regId) {
                        entries_[s] = first;  // reused slot: restart, never a delta across owners
                    } else if (nowNs <= prev.ns) {
                        entries_[s] = first;  // no elapsed time, no rate
                    } else if (isBackwards(l->value, prev.value)) {
                        entries_[s] = first;  // a total that went backwards: rebaseline, never a negative rate
                    } else {
                        // Unsigned subtraction: a total crossing INT64_MAX is still one positive delta.
                        const int64_t delta = static_cast<int64_t>(static_cast<uint64_t>(l->value) -
                                                                   static_cast<uint64_t>(prev.value));
                        entries_[s] = detail::RateRated{l->regId, l->value, nowNs,
                                                        static_cast<double>(delta) * 1e9 / static_cast<double>(nowNs - prev.ns)};
                    }
                },
                entries_[s]);
        }
    }

    // Wrap-aware: a forward step across INT64_MAX is a small positive delta; a step of more than 2^63 back is not.
    static bool isBackwards(int64_t now, int64_t before) noexcept {
        return static_cast<int64_t>(static_cast<uint64_t>(now) - static_cast<uint64_t>(before)) < 0;
    }

    [[nodiscard]] std::optional<double> perSecond(int32_t slot) const noexcept {
        if (slot < 0 || slot >= Capacity) return std::nullopt;
        if (const auto* r = std::get_if<detail::RateRated>(&entries_[slot])) return r->perSecond;
        return std::nullopt;
    }

private:
    std::variant<detail::RateAbsent, detail::RateFirst, detail::RateRated> entries_[Capacity]{};
};

// Levels sampled at the caller's period; max and average over the last Window samples of each slot.
// The average is per sample, not time-weighted; nowNs does not change the result.
template <int32_t Capacity, int32_t Window>
class WindowSampler {
    static_assert(Capacity > 0 && Window > 0);

public:
    template <class Reader>
    void sample(const Reader& reader, [[maybe_unused]] int64_t nowNs) noexcept {
        const int32_t n = std::min(Capacity, reader.maxSlots());
        for (int32_t s = 0; s < n; ++s) {
            const SlotView v = reader.read(s);
            const Live* l = std::get_if<Live>(&v);
            Entry& e = entries_[s];
            if (l == nullptr) {
                e = Entry{};
                continue;
            }
            if (e.regId != l->regId) {
                e = Entry{};
                e.regId = l->regId;
            }
            e.ring[e.head] = l->value;
            e.head = (e.head + 1) % Window;
            if (e.count < Window) ++e.count;
        }
    }

    [[nodiscard]] std::optional<int64_t> max(int32_t slot) const noexcept {
        const Entry* e = entry(slot);
        if (e == nullptr) return std::nullopt;
        int64_t m = e->ring[0];
        for (int32_t i = 1; i < e->count; ++i) m = std::max(m, e->ring[i]);
        return m;
    }

    [[nodiscard]] std::optional<double> avg(int32_t slot) const noexcept {
        const Entry* e = entry(slot);
        if (e == nullptr) return std::nullopt;
        __int128 sum = 0;
        for (int32_t i = 0; i < e->count; ++i) sum += e->ring[i];
        return static_cast<double>(sum) / static_cast<double>(e->count);
    }

private:
    struct Entry {
        int64_t regId = -1;  // -1 is never a regId: no entry yet
        int32_t head = 0;
        int32_t count = 0;
        int64_t ring[Window]{};
    };
    const Entry* entry(int32_t slot) const noexcept {
        if (slot < 0 || slot >= Capacity || entries_[slot].count == 0) return nullptr;
        return &entries_[slot];
    }
    Entry entries_[Capacity]{};
};

}  // namespace metrics
