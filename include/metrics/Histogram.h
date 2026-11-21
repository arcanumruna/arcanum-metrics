#pragma once
// Single-writer histogram (plan 07g, D5): cumulative bucket slots plus one sum slot, allocated all or none.
// observe() is up to bounds+2 single-writer stores (SlotWriter::add: plain load, add, release store); there is no
// atomic read-modify-write. Several writer threads of one series each own a histogram with the same labels; the
// renderer merges them. allocateHistogram / freeHistogram run on the allocator thread. Nothing here allocates on
// the heap or throws.
#include "metrics/Families.h"
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsWriter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace metrics {

inline constexpr size_t kMaxHistogramBounds = 16;
inline constexpr std::byte kHistogramSumKey{0xFF};

enum class HistogramStatus : uint8_t { Ok, Full, BadBounds, BadScale, LabelTooLong, InvalidType };

// raw / 10^scaleExp as an exact decimal: no exponent, trailing zeros cut. (500,6)->"0.0005", (2500,3)->"2.5".
// Returns the length, or 0 if `out` is too small (or scaleExp > 18).
[[nodiscard]] inline size_t formatScaled(int64_t raw, uint8_t scaleExp, std::span<char> out) noexcept {
    if (scaleExp > 18) return 0;
    char digits[24];
    size_t nd = 0;
    uint64_t mag = raw < 0 ? 0 - static_cast<uint64_t>(raw) : static_cast<uint64_t>(raw);
    do {
        digits[nd++] = static_cast<char>('0' + mag % 10);  // least significant first
        mag /= 10;
    } while (mag != 0);
    while (nd <= scaleExp) digits[nd++] = '0';  // at least one integer digit
    size_t fracEnd = 0;                         // number of fraction digits kept (trailing zeros cut)
    while (fracEnd < scaleExp && digits[fracEnd] == '0') ++fracEnd;
    const size_t fracDigits = scaleExp - fracEnd;
    const size_t intDigits = nd - scaleExp;
    const bool neg = raw < 0;
    const size_t total = (neg ? 1 : 0) + intDigits + (fracDigits != 0 ? 1 + fracDigits : 0);
    if (total > out.size()) return 0;
    size_t p = 0;
    if (neg) out[p++] = '-';
    for (size_t i = 0; i < intDigits; ++i) out[p++] = digits[nd - 1 - i];
    if (fracDigits != 0) {
        out[p++] = '.';
        for (size_t i = 0; i < fracDigits; ++i) out[p++] = digits[scaleExp - 1 - i];
    }
    return p;
}

class HistogramWriter {  // value type; one writer thread
public:
    HistogramWriter() noexcept = default;

    // Unbound writers must not observe (the caller's wrapper checks bound()).
    void observe(int64_t v) noexcept {
        for (size_t i = 0; i < n_; ++i) {
            if (bounds_[i] >= v) buckets_[i].add(1);
        }
        buckets_[n_].add(1);  // +Inf
        sum_.add(v);
    }
    [[nodiscard]] int64_t bucket(size_t i) const noexcept { return buckets_[i].get(); }  // i == bounds(): +Inf
    [[nodiscard]] int64_t sum() const noexcept { return sum_.get(); }
    [[nodiscard]] size_t bounds() const noexcept { return n_; }
    [[nodiscard]] bool bound() const noexcept { return n_ != 0; }

private:
    friend HistogramStatus allocateHistogram(MetricsWriter&, int32_t, int32_t, std::string_view,
                                             std::span<const int64_t>, uint8_t, HistogramWriter&) noexcept;
    friend void freeHistogram(MetricsWriter&, HistogramWriter&) noexcept;
    std::array<SlotWriter, kMaxHistogramBounds + 1> buckets_{};
    std::array<int64_t, kMaxHistogramBounds> bounds_{};
    SlotWriter sum_{};
    size_t n_ = 0;
};

// Allocator thread. All bounds+2 slots or none. Bucket i: typeId, key {uint8 i}, label
// "<base>,le=\"<formatScaled(bound,scale)>\"" (no leading comma when base is empty; +Inf last); sum: sumTypeId,
// key {0xFF}, label "<base>". Bounds must be 1..16, strictly increasing and > 0; scaleExp <= 9. `out` is left
// unbound on any refusal.
[[nodiscard]] inline HistogramStatus allocateHistogram(MetricsWriter& w, int32_t typeId, int32_t sumTypeId,
                                                       std::string_view baseLabel, std::span<const int64_t> bounds,
                                                       uint8_t scaleExp, HistogramWriter& out) noexcept {
    out = HistogramWriter{};
    if (bounds.empty() || bounds.size() > kMaxHistogramBounds) return HistogramStatus::BadBounds;
    for (size_t i = 0; i < bounds.size(); ++i) {
        if (bounds[i] <= 0 || (i > 0 && bounds[i] <= bounds[i - 1])) return HistogramStatus::BadBounds;
    }
    if (scaleExp > 9) return HistogramStatus::BadScale;
    if (typeId <= 0 || sumTypeId <= 0 || typeId == sumTypeId || typeId == kFamilyDescriptorType ||
        sumTypeId == kFamilyDescriptorType) {
        return HistogramStatus::InvalidType;
    }

    HistogramWriter h;
    h.n_ = bounds.size();
    int32_t slots[kMaxHistogramBounds + 2];
    size_t taken = 0;
    const auto rollback = [&](HistogramStatus st) noexcept {
        for (size_t k = taken; k > 0; --k) (void)w.free(slots[k - 1]);
        return st;
    };
    const auto status = [](AllocStatus s) noexcept {
        return s == AllocStatus::Full           ? HistogramStatus::Full
               : s == AllocStatus::LabelTooLong ? HistogramStatus::LabelTooLong
                                                : HistogramStatus::InvalidType;
    };
    for (size_t i = 0; i <= bounds.size(); ++i) {
        detail::LabelBuilder b;
        b.put(baseLabel);
        if (!baseLabel.empty()) b.put(",");
        b.put("le=\"");
        if (i < bounds.size()) {
            char num[24];
            b.put(std::string_view(num, formatScaled(bounds[i], scaleExp, std::span<char>(num))));
        } else {
            b.put("+Inf");
        }
        b.put("\"");
        if (!b.fits()) return rollback(HistogramStatus::LabelTooLong);
        const std::byte key[1] = {static_cast<std::byte>(i)};
        const AllocResult r = w.allocate(typeId, MetricKind::Counter, -1, key, b.view());
        if (r.status != AllocStatus::Ok) return rollback(status(r.status));
        slots[taken++] = r.writer.slot();
        h.buckets_[i] = r.writer;
        if (i < bounds.size()) h.bounds_[i] = bounds[i];
    }
    const std::byte sumKey[1] = {kHistogramSumKey};
    const AllocResult r = w.allocate(sumTypeId, MetricKind::Counter, -1, sumKey, baseLabel);
    if (r.status != AllocStatus::Ok) return rollback(status(r.status));
    h.sum_ = r.writer;
    out = h;
    return HistogramStatus::Ok;
}

// Allocator thread. Frees every slot of `h` and leaves it unbound; an unbound writer is a no-op. The owning
// writer thread must have stopped observing first.
inline void freeHistogram(MetricsWriter& w, HistogramWriter& h) noexcept {
    if (!h.bound()) return;
    for (size_t i = 0; i <= h.n_; ++i) (void)w.free(h.buckets_[i].slot());
    (void)w.free(h.sum_.slot());
    h = HistogramWriter{};
}

}  // namespace metrics
