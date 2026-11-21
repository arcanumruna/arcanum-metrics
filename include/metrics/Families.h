#pragma once
// Named Prometheus families, published into the metrics file as descriptor slots (plan 07g, D2).
//
// One descriptor slot per family: typeId kFamilyDescriptorType, key = the family typeId (4 bytes, little-endian),
// label = name \t type(c|g|h) \t scaleExp \t merge(s|m) \t labelmode(l|n) \t sumTypeId \t help. The reader side
// (PrometheusText::renderNamed) decodes them, so an exporter needs no table of its own.
// publishFamilies runs on the allocator thread. Nothing here allocates on the heap, throws, or does an atomic
// read-modify-write.
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsReader.h"
#include "metrics/MetricsWriter.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace metrics {

enum class FamilyType : uint8_t { Counter = 1, Gauge = 2, Histogram = 3 };
enum class Merge : uint8_t { Sum = 0, Max = 1 };
// None: the slot label is not printed (SystemMetrics labels are names, not Prometheus label sets).
enum class LabelMode : uint8_t { Set = 0, None = 1 };

struct Family {
    int32_t typeId;
    std::string_view name, help;
    FamilyType type;
    Merge merge = Merge::Sum;
    LabelMode labels = LabelMode::Set;
    int32_t sumTypeId = 0;  // histograms only: the typeId of the sum slot
    uint8_t scaleExp = 0;   // histograms only: the sum is stored scaled by 10^scaleExp
};

// What decodeFamily returns: the same fields, the string_views pointing into the Live it was decoded from.
using FamilyView = Family;

inline constexpr int32_t kFamilyDescriptorType = 1'000'306;
// publishFamilies keeps the slot numbers of one call on the stack (16 KiB); more rows than this are refused as Full.
inline constexpr size_t kMaxPublishRows = 4096;

enum class FamilyStatus : uint8_t { Ok, Full, TooLong, BadName, BadHelp, Duplicate, BadHistogram };

namespace detail {

[[nodiscard]] constexpr bool isNameStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}
[[nodiscard]] constexpr bool isValidFamilyName(std::string_view n) noexcept {
    if (n.empty() || !isNameStart(n[0])) return false;
    for (const char c : n) {
        if (!isNameStart(c) && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}
[[nodiscard]] constexpr bool isValidHelp(std::string_view h) noexcept {
    for (const char c : h) {
        if (c == '\t' || c == '\n') return false;
    }
    return true;
}

// Builds a descriptor label into a fixed buffer; `total` counts every byte asked for, so a label that does not
// fit is detected without writing past the buffer.
struct LabelBuilder {
    char buf[kMaxLabelLength];
    size_t total = 0;
    void put(std::string_view v) noexcept {
        if (total + v.size() <= sizeof buf && !v.empty()) std::memcpy(buf + total, v.data(), v.size());
        total += v.size();
    }
    void putNumber(int64_t v) noexcept {
        char tmp[24];
        const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
        put(std::string_view(tmp, static_cast<size_t>(r.ptr - tmp)));
    }
    [[nodiscard]] bool fits() const noexcept { return total <= sizeof buf; }
    [[nodiscard]] std::string_view view() const noexcept { return std::string_view(buf, total); }
};

inline void buildDescriptorLabel(const Family& f, LabelBuilder& b) noexcept {
    b.put(f.name);
    b.put("\t");
    b.put(f.type == FamilyType::Counter ? "c" : f.type == FamilyType::Gauge ? "g" : "h");
    b.put("\t");
    b.putNumber(f.scaleExp);
    b.put("\t");
    b.put(f.merge == Merge::Max ? "m" : "s");
    b.put("\t");
    b.put(f.labels == LabelMode::None ? "n" : "l");
    b.put("\t");
    b.putNumber(f.sumTypeId);
    b.put("\t");
    b.put(f.help);
}

// Splits `rest` at the first tab: field = before, rest = after. False if there is no tab.
[[nodiscard]] inline bool takeField(std::string_view& rest, std::string_view& field) noexcept {
    const size_t p = rest.find('\t');
    if (p == std::string_view::npos) return false;
    field = rest.substr(0, p);
    rest.remove_prefix(p + 1);
    return true;
}
template <class T>
[[nodiscard]] inline bool parseInt(std::string_view v, T& out) noexcept {
    if (v.empty()) return false;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

}  // namespace detail

// Allocator thread. One descriptor per row. Every row is checked before anything is allocated; if the file then
// fills up, the descriptors already allocated are freed. On any refusal *failedRow is the offending row (-1 on
// success) and the file's allocatedCount() is what it was before the call.
[[nodiscard]] inline FamilyStatus publishFamilies(MetricsWriter& w, std::span<const Family> rows,
                                                  int32_t* failedRow = nullptr) noexcept {
    if (rows.size() > kMaxPublishRows) {
        if (failedRow != nullptr) *failedRow = static_cast<int32_t>(kMaxPublishRows);
        return FamilyStatus::Full;
    }
    const auto refuse = [&](size_t row, FamilyStatus st) noexcept {
        if (failedRow != nullptr) *failedRow = static_cast<int32_t>(row);
        return st;
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        const Family& f = rows[i];
        if (!detail::isValidFamilyName(f.name)) return refuse(i, FamilyStatus::BadName);
        if (!detail::isValidHelp(f.help)) return refuse(i, FamilyStatus::BadHelp);
        if (f.type == FamilyType::Histogram &&
            (f.sumTypeId == 0 || f.sumTypeId == f.typeId || f.scaleExp > 9)) {
            return refuse(i, FamilyStatus::BadHistogram);
        }
        detail::LabelBuilder b;
        detail::buildDescriptorLabel(f, b);
        if (!b.fits()) return refuse(i, FamilyStatus::TooLong);
        for (size_t j = 0; j < i; ++j) {
            const Family& g = rows[j];
            const bool sumClash = (f.type == FamilyType::Histogram && f.sumTypeId == g.typeId) ||
                                  (g.type == FamilyType::Histogram && g.sumTypeId == f.typeId) ||
                                  (f.type == FamilyType::Histogram && g.type == FamilyType::Histogram &&
                                   f.sumTypeId == g.sumTypeId);
            if (f.typeId == g.typeId || f.name == g.name || sumClash) return refuse(i, FamilyStatus::Duplicate);
        }
    }
    std::array<std::byte, 4> key{};
    int32_t slots[kMaxPublishRows];  // the slots taken so far, to free them again on a refusal
    for (size_t i = 0; i < rows.size(); ++i) {
        const Family& f = rows[i];
        detail::LabelBuilder b;
        detail::buildDescriptorLabel(f, b);
        std::memcpy(key.data(), &f.typeId, 4);  // the layout is little-endian (static_assert in MetricsLayout.h)
        const AllocResult r = w.allocate(kFamilyDescriptorType, MetricKind::Gauge, -1, key, b.view());
        if (r.status != AllocStatus::Ok) {
            for (size_t k = i; k > 0; --k) (void)w.free(slots[k - 1]);
            return refuse(i, r.status == AllocStatus::LabelTooLong ? FamilyStatus::TooLong : FamilyStatus::Full);
        }
        slots[i] = r.writer.slot();
    }
    if (failedRow != nullptr) *failedRow = -1;
    return FamilyStatus::Ok;
}

// Decodes a descriptor slot. False if it is not one or its label is malformed.
[[nodiscard]] inline bool decodeFamily(const Live& d, FamilyView& out) noexcept {
    if (d.typeId != kFamilyDescriptorType || d.keyLength != 4) return false;
    int32_t id;
    std::memcpy(&id, d.keyBytes.data(), 4);
    std::string_view rest = d.label();
    std::string_view name, type, scale, merge, labels, sum;
    if (!detail::takeField(rest, name) || !detail::takeField(rest, type) || !detail::takeField(rest, scale) ||
        !detail::takeField(rest, merge) || !detail::takeField(rest, labels) || !detail::takeField(rest, sum)) {
        return false;
    }
    if (!detail::isValidFamilyName(name) || type.size() != 1 || merge.size() != 1 || labels.size() != 1) return false;
    FamilyView f{};
    f.typeId = id;
    f.name = name;
    f.help = rest;
    if (type[0] == 'c') f.type = FamilyType::Counter;
    else if (type[0] == 'g') f.type = FamilyType::Gauge;
    else if (type[0] == 'h') f.type = FamilyType::Histogram;
    else return false;
    if (merge[0] != 's' && merge[0] != 'm') return false;
    f.merge = merge[0] == 'm' ? Merge::Max : Merge::Sum;
    if (labels[0] != 'l' && labels[0] != 'n') return false;
    f.labels = labels[0] == 'n' ? LabelMode::None : LabelMode::Set;
    uint32_t sc = 0;
    if (!detail::parseInt(scale, sc) || sc > 255 || !detail::parseInt(sum, f.sumTypeId)) return false;
    f.scaleExp = static_cast<uint8_t>(sc);
    out = f;
    return true;
}

// The SystemMetrics slots (ids as SystemMetrics::kType*), for publishFamilies. Their slot label is the metric
// name, so it is not printed.
inline constexpr std::array<Family, 5> kSystemMetricsFamilies{{
    {1'000'301, "process_open_fds", "Number of open file descriptors.", FamilyType::Gauge, Merge::Sum,
     LabelMode::None},
    {1'000'302, "process_threads", "Number of OS threads in the process.", FamilyType::Gauge, Merge::Sum,
     LabelMode::None},
    {1'000'303, "process_resident_memory_bytes", "Resident memory size in bytes.", FamilyType::Gauge, Merge::Sum,
     LabelMode::None},
    {1'000'304, "process_virtual_memory_bytes", "Virtual memory size in bytes.", FamilyType::Gauge, Merge::Sum,
     LabelMode::None},
    {1'000'305, "process_metrics_read_errors_total", "Failed reads of the process statistics files.",
     FamilyType::Counter, Merge::Sum, LabelMode::None},
}};

}  // namespace metrics
