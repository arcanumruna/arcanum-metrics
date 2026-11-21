#pragma once
// Prometheus text rendering as a pure function into a caller buffer: it opens no sockets and allocates nothing.
// The caller samples the RateTracker once per scrape (`rates.sample(reader, nowNs)`); render only reads.
//
// Series (the label text is emitted as one quoted, escaped label value; reg_id is the slot's unique regId):
//   metrics_counter{type="<typeId>",reg_id="<regId>",label="<label>"} <total>
//   metrics_gauge{...} <level>
//   metrics_rate_per_second{...} <rate>        counters that have a rate for that regId only
// Each family is preceded by its # HELP and # TYPE lines.
#include "metrics/Families.h"
#include "metrics/Histogram.h"
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsReader.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace metrics {

struct RenderResult {
    size_t length;  // bytes written; never more than the buffer holds
    bool overflow;  // the text did not fit: the buffer content is unspecified, retry with a bigger buffer
    uint32_t skipped = 0;       // renderNamed: series left out because their label holds a newline or a brace
    bool scratchShort = false;  // renderNamed: more live slots than scratch; overflow is set too
};

// Caller-owned working space of renderNamed (equal sizes): one Live per live slot and the sort order over them.
struct NamedScratch {
    std::span<Live> live;
    std::span<int32_t> order;
};

class PrometheusText {
public:
    // One write pass per family straight into `out` (a family must be one contiguous group, so the reader is
    // scanned once per family). There is no count pass, so the length can never disagree with what was written.
    template <class Reader, int32_t Capacity, int32_t Window>
    [[nodiscard]] static RenderResult render(const Reader& reader, const RateTracker<Capacity, Window>& rates,
                                             std::span<char> out) noexcept {
        Sink s{out.data(), out.size(), 0, false};
        s.put("# HELP metrics_counter Monotonic total of a slot, by type id, registration id and label.\n"
              "# TYPE metrics_counter counter\n");
        reader.forEachLive([&](const Live& l) {
            if (l.kind != MetricKind::Counter) return;
            series(s, "metrics_counter", l);
            s.putNumber(l.value);
            s.putChar('\n');
        });
        s.put("# HELP metrics_gauge Level of a slot, by type id, registration id and label.\n"
              "# TYPE metrics_gauge gauge\n");
        reader.forEachLive([&](const Live& l) {
            if (l.kind != MetricKind::Gauge) return;
            series(s, "metrics_gauge", l);
            s.putNumber(l.value);
            s.putChar('\n');
        });
        s.put("# HELP metrics_rate_per_second Per-second rate of a counter between the reader's last two samples.\n"
              "# TYPE metrics_rate_per_second gauge\n");
        reader.forEachLive([&](const Live& l) {
            if (l.kind != MetricKind::Counter) return;
            const auto rate = rates.perSecond(l.slot, l.regId);  // keyed by regId: never a previous owner's rate
            if (!rate) return;
            series(s, "metrics_rate_per_second", l);
            s.putNumber(*rate);
            s.putChar('\n');
        });
        return RenderResult{s.len, s.overflow};
    }

    // Named rendering (plan 07g). The live slots are copied into scratch.live and sorted by (typeId, label, key).
    // Slots of one family with the same label are one series: counters are summed, gauges summed or maxed per the
    // family. Families print in typeId order, each with # HELP (when not empty) and # TYPE once. Descriptor slots
    // are never printed. Slots of no family print afterwards in the generic form of render(). A series whose
    // label holds '\n', '{' or '}', or a non-empty label that does not end with '"', is skipped and counted. A family
    // name published under several typeIds prints only the lowest typeId's family; the others' series are counted
    // as skipped. A histogram family groups its bucket slots and its sum slots by (base label, key), where the base
    // label is the bucket label without its trailing ,le="...": buckets print in key (bound) order, then _sum
    // (scaled) and _count (the merged +Inf bucket) per base label.
    // Allocates nothing and never throws; std::sort works in place on the caller's spans.
    template <class Reader>
    [[nodiscard]] static RenderResult renderNamed(const Reader& reader, NamedScratch scratch,
                                                  std::span<char> out) noexcept {
        Sink s{out.data(), out.size(), 0, false};
        const size_t cap = std::min(scratch.live.size(), scratch.order.size());
        size_t n = 0;
        bool tooMany = false;
        reader.forEachLive([&](const Live& l) {
            if (n >= cap) { tooMany = true; return; }
            scratch.live[n] = l;
            scratch.order[n] = static_cast<int32_t>(n);
            ++n;
        });
        if (tooMany) return RenderResult{0, true, 0, true};

        const std::span<Live> live = scratch.live;
        const std::span<int32_t> order = scratch.order.first(n);
        std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) noexcept {
            const Live& x = live[static_cast<size_t>(a)];
            const Live& y = live[static_cast<size_t>(b)];
            if (x.typeId != y.typeId) return x.typeId < y.typeId;
            if (const int c = x.label().compare(y.label()); c != 0) return c < 0;
            const auto kx = x.key();
            const auto ky = y.key();
            return std::lexicographical_compare(kx.begin(), kx.end(), ky.begin(), ky.end());
        });
        const auto at = [&](size_t i) -> const Live& { return live[static_cast<size_t>(order[i])]; };

        // The descriptors are contiguous after the sort.
        size_t dBegin = 0;
        while (dBegin < n && at(dBegin).typeId < kFamilyDescriptorType) ++dBegin;
        size_t dEnd = dBegin;
        while (dEnd < n && at(dEnd).typeId == kFamilyDescriptorType) ++dEnd;

        enum class Role { Named, HistogramSum, None };
        const auto describe = [&](int32_t typeId, FamilyView& f) noexcept {
            for (size_t i = dBegin; i < dEnd; ++i) {
                const Live& d = at(i);
                int32_t id;
                std::memcpy(&id, d.keyBytes.data(), 4);
                if (d.keyLength == 4 && id == typeId && decodeFamily(d, f)) return Role::Named;
            }
            for (size_t i = dBegin; i < dEnd; ++i) {
                FamilyView g;
                if (decodeFamily(at(i), g) && g.type == FamilyType::Histogram && g.sumTypeId == typeId) {
                    return Role::HistogramSum;
                }
            }
            return Role::None;
        };

        uint32_t skipped = 0;
        for (size_t g = 0; g < n;) {
            const int32_t typeId = at(g).typeId;
            size_t gEnd = g;
            while (gEnd < n && at(gEnd).typeId == typeId) ++gEnd;
            FamilyView f;
            const Role role = typeId != kFamilyDescriptorType ? describe(typeId, f) : Role::None;
            if (role == Role::Named) {
                // Another descriptor with the same name and a lower typeId owns the name.
                bool emit = true;
                for (size_t i = dBegin; i < dEnd && emit; ++i) {
                    FamilyView o;
                    if (decodeFamily(at(i), o) && o.typeId < f.typeId && o.name == f.name) emit = false;
                }
                if (emit) {
                    if (!f.help.empty()) {
                        s.put("# HELP ");
                        s.put(f.name);
                        s.putChar(' ');
                        s.putHelp(f.help);
                        s.putChar('\n');
                    }
                    s.put("# TYPE ");
                    s.put(f.name);
                    s.put(f.type == FamilyType::Counter ? " counter\n"
                          : f.type == FamilyType::Gauge ? " gauge\n"
                                                        : " histogram\n");
                }
                if (f.type == FamilyType::Histogram) {
                    renderHistogram(s, f, order, live, g, gEnd, emit, skipped);
                    g = gEnd;
                    continue;
                }
                const bool byLabel = f.labels == LabelMode::Set;
                for (size_t i = g; i < gEnd;) {
                    size_t j = i + 1;
                    while (j < gEnd && (!byLabel || at(j).label() == at(i).label())) ++j;
                    int64_t v = at(i).value;
                    for (size_t k = i + 1; k < j; ++k) {
                        const int64_t x = at(k).value;
                        if (f.type == FamilyType::Gauge && f.merge == Merge::Max) {
                            v = std::max(v, x);
                        } else {
                            v = static_cast<int64_t>(static_cast<uint64_t>(v) + static_cast<uint64_t>(x));
                        }
                    }
                    const std::string_view label = byLabel ? at(i).label() : std::string_view{};
                    if (!emit || !labelPrintable(label)) {
                        ++skipped;
                    } else {
                        s.put(f.name);
                        if (!label.empty()) {
                            s.putChar('{');
                            s.put(label);
                            s.putChar('}');
                        }
                        s.putChar(' ');
                        s.putNumber(v);
                        s.putChar('\n');
                    }
                    i = j;
                }
            }
            g = gEnd;
        }

        // Slots of no family, in the 2.0 generic form: counters, then gauges.
        for (const MetricKind kind : {MetricKind::Counter, MetricKind::Gauge}) {
            bool head = false;
            for (size_t g = 0; g < n;) {
                const int32_t typeId = at(g).typeId;
                size_t gEnd = g;
                while (gEnd < n && at(gEnd).typeId == typeId) ++gEnd;
                FamilyView f;
                if (typeId != kFamilyDescriptorType && describe(typeId, f) == Role::None) {
                    for (size_t i = g; i < gEnd; ++i) {
                        const Live& l = at(i);
                        if (l.kind != kind) continue;
                        const bool counter = kind == MetricKind::Counter;
                        if (!head) {
                            head = true;
                            s.put(counter ? "# HELP metrics_counter Monotonic total of a slot, by type id, registration id and label.\n"
                                            "# TYPE metrics_counter counter\n"
                                          : "# HELP metrics_gauge Level of a slot, by type id, registration id and label.\n"
                                            "# TYPE metrics_gauge gauge\n");
                        }
                        series(s, counter ? "metrics_counter" : "metrics_gauge", l);
                        s.putNumber(l.value);
                        s.putChar('\n');
                    }
                }
                g = gEnd;
            }
        }
        return RenderResult{s.len, s.overflow, skipped, false};
    }

private:
    struct Sink {
        char* dst;
        size_t cap;
        size_t len;
        bool overflow;
        void put(std::string_view v) noexcept {
            if (overflow) return;
            if (len + v.size() > cap) { overflow = true; return; }  // nothing of v is written
            if (!v.empty()) std::memcpy(dst + len, v.data(), v.size());
            len += v.size();
        }
        void putChar(char c) noexcept { put(std::string_view(&c, 1)); }
        template <class T>
        void putNumber(T v) noexcept {
            char tmp[40];
            const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
            put(std::string_view(tmp, static_cast<size_t>(r.ptr - tmp)));
        }
        // # HELP text: backslash and newline are escaped (the newline cannot occur in a published help).
        void putHelp(std::string_view v) noexcept {
            size_t run = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const char c = v[i];
                const char* esc = c == '\\' ? "\\\\" : c == '\n' ? "\\n" : nullptr;
                if (esc == nullptr) continue;
                put(v.substr(run, i - run));
                put(esc);
                run = i + 1;
            }
            put(v.substr(run));
        }
        // Runs of characters that need no escape are written with one put each, not one per character.
        void putEscaped(std::string_view v) noexcept {
            size_t run = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                const char c = v[i];
                const char* esc = c == '\\' ? "\\\\" : c == '"' ? "\\\"" : c == '\n' ? "\\n" : nullptr;
                if (esc == nullptr) continue;
                put(v.substr(run, i - run));
                put(esc);
                run = i + 1;
            }
            put(v.substr(run));
        }
    };

    // A label printed verbatim: no newline or braces, and a non-empty label ends with a closing quote.
    [[nodiscard]] static bool labelPrintable(std::string_view l) noexcept {
        return l.find_first_of("\n{}") == std::string_view::npos && (l.empty() || l.back() == '"');
    }
    // The label of a bucket slot without its trailing ,le="..." (or le="..." alone). False if it has none.
    [[nodiscard]] static bool bucketBase(std::string_view l, std::string_view& base) noexcept {
        const size_t p = l.rfind("le=\"");
        if (p == std::string_view::npos || l.back() != '"') return false;
        if (p == 0) { base = {}; return true; }
        if (l[p - 1] != ',') return false;
        base = l.substr(0, p - 1);
        return true;
    }
    static bool sameKey(const Live& a, const Live& b) noexcept {
        const auto x = a.key();
        const auto y = b.key();
        return std::equal(x.begin(), x.end(), y.begin(), y.end());
    }

    // Prints the histogram family `f`: its bucket slots are order[g, gEnd), its sum slots those of sumTypeId.
    // With emit false (a duplicate family name) nothing is printed and each merged series is counted as skipped.
    static void renderHistogram(Sink& s, const FamilyView& f, std::span<int32_t> order, std::span<const Live> live,
                                size_t g, size_t gEnd, bool emit, uint32_t& skipped) noexcept {
        const auto at = [&](size_t i) -> const Live& { return live[static_cast<size_t>(order[i])]; };
        const auto baseOf = [&](const Live& l) noexcept {
            std::string_view b;
            return bucketBase(l.label(), b) ? b : l.label();
        };
        std::sort(order.begin() + static_cast<std::ptrdiff_t>(g), order.begin() + static_cast<std::ptrdiff_t>(gEnd),
                  [&](int32_t a, int32_t b) noexcept {
                      const Live& x = live[static_cast<size_t>(a)];
                      const Live& y = live[static_cast<size_t>(b)];
                      if (const int c = baseOf(x).compare(baseOf(y)); c != 0) return c < 0;
                      const auto kx = x.key();
                      const auto ky = y.key();
                      return std::lexicographical_compare(kx.begin(), kx.end(), ky.begin(), ky.end());
                  });
        // The sum slots, sorted by (label, key) like everything else.
        size_t sBegin = 0, sEnd = 0;
        while (sBegin < order.size() && at(sBegin).typeId != f.sumTypeId) ++sBegin;
        for (sEnd = sBegin; sEnd < order.size() && at(sEnd).typeId == f.sumTypeId; ++sEnd) {
        }
        size_t sPos = sBegin;
        // Passes over the sum slots while `orphan` holds, counting each merged series (by key) as skipped.
        const auto skipSums = [&](auto orphan) noexcept {
            while (sPos < sEnd && orphan(at(sPos))) {
                size_t e = sPos + 1;
                while (e < sEnd && at(e).label() == at(sPos).label() && sameKey(at(sPos), at(e))) ++e;
                ++skipped;
                sPos = e;
            }
        };

        for (size_t i = g; i < gEnd;) {
            const std::string_view base = baseOf(at(i));
            size_t j = i;
            while (j < gEnd && baseOf(at(j)) == base) ++j;
            // `inf` ends as the merged value of the last bucket run of this base: the +Inf bucket, i.e. the count.
            bool wellFormed = labelPrintable(base);
            bool anyBucket = false;  // some bucket run of this base was printed
            int64_t inf = 0;
            for (size_t a = i; a < j;) {
                size_t b = a + 1;
                int64_t v = at(a).value;
                while (b < j && sameKey(at(a), at(b))) {
                    v = static_cast<int64_t>(static_cast<uint64_t>(v) + static_cast<uint64_t>(at(b).value));
                    ++b;
                }
                std::string_view tmp;
                if (!emit || !wellFormed || !bucketBase(at(a).label(), tmp) || !labelPrintable(at(a).label())) {
                    ++skipped;
                } else {
                    s.put(f.name);
                    s.put("_bucket{");
                    s.put(at(a).label());
                    s.put("} ");
                    s.putNumber(v);
                    s.putChar('\n');
                    inf = v;
                    anyBucket = true;
                }
                a = b;
            }
            int64_t sum = 0;
            bool haveSum = false;
            skipSums([&](const Live& l) noexcept { return l.label().compare(base) < 0; });  // orphan sums
            while (sPos < sEnd && at(sPos).label() == base) {
                sum = static_cast<int64_t>(static_cast<uint64_t>(sum) + static_cast<uint64_t>(at(sPos).value));
                haveSum = true;
                ++sPos;
            }
            if (haveSum && (!emit || !wellFormed || !anyBucket)) ++skipped;
            if (emit && wellFormed && anyBucket) {
                for (const bool isSum : {true, false}) {
                    s.put(f.name);
                    s.put(isSum ? "_sum" : "_count");
                    if (!base.empty()) {
                        s.putChar('{');
                        s.put(base);
                        s.putChar('}');
                    }
                    s.putChar(' ');
                    if (isSum) {
                        char num[48];
                        s.put(std::string_view(num, formatScaled(sum, f.scaleExp, std::span<char>(num))));
                    } else {
                        s.putNumber(inf);
                    }
                    s.putChar('\n');
                }
            }
            i = j;
        }
        skipSums([](const Live&) noexcept { return true; });  // sums sorting after the last base are orphans too
    }

    static void series(Sink& s, std::string_view family, const Live& l) noexcept {
        s.put(family);
        s.put("{type=\"");
        s.putNumber(l.typeId);
        s.put("\",reg_id=\"");
        s.putNumber(l.regId);
        s.put("\",label=\"");
        s.putEscaped(l.label());
        s.put("\"} ");
    }
};

}  // namespace metrics
