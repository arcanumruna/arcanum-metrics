#pragma once
// Reader side: acquire loads only, never writes the file. A slot is read as state, regId, value, state, regId;
// a changed regId means the slot was reused mid-read, a state that is no longer Allocated means it was freed. Rates and windows are computed here from sampled totals
// and levels with the caller's nowNs. No allocation, no exceptions, no clock.
#include "metrics/MetricsFile.h"
#include "metrics/MetricsLayout.h"

#include <algorithm>
#include <array>
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
// One owner's view of a slot, copied out of the mapping: nothing here points into the file, so it stays valid and
// consistent whatever the writer does next. key() and label() view the copy inside this object (do not use them
// after the Live is destroyed).
struct Live {
    // Leaves every field uninitialized (no zero fill of the byte arrays on each read): read() sets each field and
    // the first keyLength / labelLength bytes. The arrays are std::byte so copying the unset tail is well defined.
    Live() noexcept {}

    int32_t slot;
    int32_t typeId;
    MetricKind kind;
    int64_t regId;
    int64_t ownerTag;
    int64_t value;
    int32_t keyLength;
    int32_t labelLength;
    std::array<std::byte, kMaxKeyLength> keyBytes;
    std::array<std::byte, kMaxLabelLength> labelBytes;

    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return std::span<const std::byte>(keyBytes.data(), static_cast<size_t>(keyLength));
    }
    [[nodiscard]] std::string_view label() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(labelBytes.data()), static_cast<size_t>(labelLength));
    }
};
struct Reused {
    int32_t slot;
    int64_t oldRegId;
};
using SlotView = std::variant<Empty, Live, Reused>;

// Result of read(slot, Live&). Live: `out` is one owner's consistent copy. Reused: the slot changed owner while it
// was read; out.slot and out.regId name the old owner, the other fields are unspecified. Empty: `out` is unspecified.
enum class ReadStatus : int32_t { Empty, Live, Reused };

struct SlotRef {
    int32_t slot;   // -1: none
    int64_t regId;  // -1: none
};

// Steps of a slot read, for deterministic tests (AfterStateRecheck sits between the state and regId re-checks).
// The default hook is empty.
enum class ReadStep : int32_t { AfterFirstRegId, AfterValue, AfterStateRecheck };
struct NoReadHook {
    void operator()(ReadStep, int32_t) const noexcept {}
};

template <class Hook = NoReadHook>
class BasicMetricsReader {
public:
    explicit BasicMetricsReader(const MetricsFile& file, Hook hook = Hook{}) noexcept
        : base_(const_cast<std::byte*>(file.base())), layout_(file.layout()), hook_(hook) {}

    [[nodiscard]] int32_t maxSlots() const noexcept { return layout_.maxSlots; }

    // The variant form. One SlotView is built in place and returned (NRVO): no copy of the Live inside it.
    [[nodiscard]] SlotView read(int32_t slot) const noexcept {
        SlotView view;
        Live& l = view.template emplace<Live>();
        switch (read(slot, l)) {
            case ReadStatus::Live:
                break;
            case ReadStatus::Reused: {
                const Reused r{slot, l.regId};  // copied out before emplace ends the Live
                view.template emplace<Reused>(r);
                break;
            }
            case ReadStatus::Empty:
                view.template emplace<Empty>();
                break;
        }
        return view;
    }

    // Reads `slot` into a caller-owned Live (reused across a scan: no variant, no copy). See ReadStatus.
    [[nodiscard]] ReadStatus read(int32_t slot, Live& l) const noexcept {
        if (slot < 0 || slot >= layout_.maxSlots) return ReadStatus::Empty;
        const std::byte* m = meta(slot);
        if (ref32(m, kMetaStateOffset).load(std::memory_order_acquire) != static_cast<int32_t>(SlotState::Allocated)) {
            return ReadStatus::Empty;
        }
        const int64_t regId = ref64(m, kMetaRegIdOffset).load(std::memory_order_acquire);
        hook_(ReadStep::AfterFirstRegId, slot);

        // Copy everything out with relaxed loads, then validate the copy with the checks below.
        l.slot = slot;
        l.regId = regId;
        l.typeId = ref32(m, kMetaTypeIdOffset).load(std::memory_order_relaxed);
        l.kind = static_cast<MetricKind>(ref32(m, kMetaKindOffset).load(std::memory_order_relaxed));
        l.ownerTag = ref64(m, kMetaOwnerTagOffset).load(std::memory_order_relaxed);
        l.keyLength = static_cast<int32_t>(clamp(ref32(m, kMetaKeyLengthOffset).load(std::memory_order_relaxed), kMaxKeyLength));
        l.labelLength = static_cast<int32_t>(clamp(ref32(m, kMetaLabelLengthOffset).load(std::memory_order_relaxed), kMaxLabelLength));
        loadWords(m + kMetaKeyOffset, l.keyBytes.data(), static_cast<size_t>(l.keyLength));
        loadWords(m + kMetaLabelOffset, l.labelBytes.data(), static_cast<size_t>(l.labelLength));
        l.value = std::atomic_ref<int64_t>(*valuePtr(slot)).load(std::memory_order_acquire);
        hook_(ReadStep::AfterValue, slot);

        // Seqlock validation. The fence pairs with the allocator's release fence: if any byte copied above came
        // from a later owner, the state and regId read now show that owner. State before regId: a value of 0
        // seen after a free proves the free is visible here, and a state of Allocated then proves the new regId
        // is visible too. (regId first would allow Live{oldRegId, 0}.)
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ref32(m, kMetaStateOffset).load(std::memory_order_acquire) != static_cast<int32_t>(SlotState::Allocated)) {
            return ReadStatus::Empty;  // freed while reading
        }
        hook_(ReadStep::AfterStateRecheck, slot);
        if (ref64(m, kMetaRegIdOffset).load(std::memory_order_acquire) != regId) return ReadStatus::Reused;
        return ReadStatus::Live;
    }

    // Calls f(const Live&) for every live slot; the Live is one stack object reused for the whole scan, valid
    // only during the call. A slot found Reused is read once more. Slots are handed out in order, so the first
    // Unused slot ends the scan.
    template <class F>
    void forEachLive(F&& f) const noexcept {
        Live l;
        for (int32_t s = 0; s < layout_.maxSlots; ++s) {
            if (ref32(meta(s), kMetaStateOffset).load(std::memory_order_acquire) == static_cast<int32_t>(SlotState::Unused)) {
                return;
            }
            ReadStatus st = read(s, l);
            if (st == ReadStatus::Reused) st = read(s, l);
            if (st == ReadStatus::Live) f(static_cast<const Live&>(l));
        }
    }

    // First live slot with this typeId and key, with the regId it had when matched ({-1, -1} if none). A slot
    // found Reused is read once more, as in forEachLive.
    [[nodiscard]] SlotRef findFirst(int32_t typeId, std::span<const std::byte> key) const noexcept {
        SlotRef found{-1, -1};
        Live l;
        for (int32_t s = 0; s < layout_.maxSlots && found.slot < 0; ++s) {
            if (ref32(meta(s), kMetaStateOffset).load(std::memory_order_acquire) == static_cast<int32_t>(SlotState::Unused)) {
                break;
            }
            ReadStatus st = read(s, l);
            if (st == ReadStatus::Reused) st = read(s, l);
            if (st == ReadStatus::Live && l.typeId == typeId && l.key().size() == key.size() &&
                (key.empty() || std::memcmp(l.key().data(), key.data(), key.size()) == 0)) {
                found = SlotRef{s, l.regId};
            }
        }
        return found;
    }

private:
    static size_t clamp(int32_t n, int32_t max) noexcept { return static_cast<size_t>(std::clamp(n, 0, max)); }
    // Copies `n` bytes starting at a 4-aligned region with relaxed 32-bit word loads.
    static void loadWords(const std::byte* region, void* dst, size_t n) noexcept {
        for (size_t off = 0; off < n; off += 4) {
            const uint32_t w = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(const_cast<std::byte*>(region) + off))
                                   .load(std::memory_order_relaxed);
            std::memcpy(static_cast<std::byte*>(dst) + off, &w, std::min<size_t>(4, n - off));
        }
    }
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

enum class TrackerStatus : int32_t { Ok, CapacityTooSmall };

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
    // Binds the tracker to a reader's file. A Capacity below the file's slot count is refused (the tracker stays
    // unbound and samples nothing) rather than silently covering only the first Capacity slots.
    template <class Reader>
    [[nodiscard]] TrackerStatus bind(const Reader& reader) noexcept {
        bound_ = 0;
        if (reader.maxSlots() > Capacity) return TrackerStatus::CapacityTooSmall;
        bound_ = reader.maxSlots();
        return TrackerStatus::Ok;
    }

    template <class Reader>
    void sample(const Reader& reader, int64_t nowNs) noexcept {
        const int32_t n = std::min(bound_, reader.maxSlots());
        Live live;  // one stack Live for the whole sample
        for (int32_t s = 0; s < n; ++s) {
            const Live* l = reader.read(s, live) == ReadStatus::Live ? &live : nullptr;
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

    // The rate of the owner `regId`; none if the slot's tracked owner is another one (reused since the last sample).
    [[nodiscard]] std::optional<double> perSecond(int32_t slot, int64_t regId) const noexcept {
        if (slot < 0 || slot >= Capacity) return std::nullopt;
        if (const auto* r = std::get_if<detail::RateRated>(&entries_[slot]); r != nullptr && r->regId == regId) {
            return r->perSecond;
        }
        return std::nullopt;
    }

private:
    int32_t bound_ = 0;
    std::variant<detail::RateAbsent, detail::RateFirst, detail::RateRated> entries_[Capacity]{};
};

// Levels sampled at the caller's period; max and average over the last Window samples of each slot.
// The average is per sample, not time-weighted; nowNs does not change the result.
template <int32_t Capacity, int32_t Window>
class WindowSampler {
    static_assert(Capacity > 0 && Window > 0);

public:
    template <class Reader>
    [[nodiscard]] TrackerStatus bind(const Reader& reader) noexcept {
        bound_ = 0;
        if (reader.maxSlots() > Capacity) return TrackerStatus::CapacityTooSmall;
        bound_ = reader.maxSlots();
        return TrackerStatus::Ok;
    }

    template <class Reader>
    void sample(const Reader& reader, [[maybe_unused]] int64_t nowNs) noexcept {
        const int32_t n = std::min(bound_, reader.maxSlots());
        Live live;  // one stack Live for the whole sample
        for (int32_t s = 0; s < n; ++s) {
            const Live* l = reader.read(s, live) == ReadStatus::Live ? &live : nullptr;
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
    int32_t bound_ = 0;
    Entry entries_[Capacity]{};
};

}  // namespace metrics
