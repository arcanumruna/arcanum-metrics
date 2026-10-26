#pragma once
// Prometheus text rendering as a pure function into a caller buffer: it opens no sockets and allocates nothing.
// The caller samples the RateTracker once per scrape (`rates.sample(reader, nowNs)`); render only reads.
//
// Series (the label text is emitted as one quoted, escaped label value; reg_id is the slot's unique regId):
//   metrics_counter{type="<typeId>",reg_id="<regId>",label="<label>"} <total>
//   metrics_gauge{...} <level>
//   metrics_rate_per_second{...} <rate>        counters that have a rate for that regId only
// Each family is preceded by its # HELP and # TYPE lines.
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsReader.h"

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
