#pragma once
// Prometheus text rendering as a pure function into a caller buffer: it opens no sockets and allocates nothing.
// Series (the label text is emitted as one quoted, escaped label value):
//   metrics_counter{type="<typeId>",slot="<slot>",label="<label>"} <total>
//   metrics_gauge{...} <level>
//   metrics_rate_per_second{...} <rate>        counters that have a rate only
#include "metrics/MetricsLayout.h"
#include "metrics/MetricsReader.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <variant>

namespace metrics {

class PrometheusText {
public:
    // Samples the tracker at nowNs, then renders. Returns the length the text needs. When that exceeds
    // out.size() nothing is written (a first pass only counts), so the caller can retry with a larger buffer.
    template <class Reader, int32_t Capacity, int32_t Window>
    static size_t render(const Reader& reader, RateTracker<Capacity, Window>& rates, int64_t nowNs,
                         std::span<char> out) noexcept {
        rates.sample(reader, nowNs);
        Sink count{nullptr, 0, 0};
        emit(reader, rates, count);
        if (count.len > out.size()) return count.len;
        Sink write{out.data(), out.size(), 0};
        emit(reader, rates, write);
        return write.len;
    }

private:
    struct Sink {
        char* dst;
        size_t cap;
        size_t len;
        void put(std::string_view s) noexcept {
            if (dst != nullptr && len + s.size() <= cap) std::memcpy(dst + len, s.data(), s.size());
            len += s.size();
        }
        void putChar(char c) noexcept { put(std::string_view(&c, 1)); }
        template <class T>
        void putNumber(T v) noexcept {
            char tmp[40];
            const auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
            put(std::string_view(tmp, static_cast<size_t>(r.ptr - tmp)));
        }
        void putEscaped(std::string_view s) noexcept {
            for (const char c : s) {
                if (c == '\\') put("\\\\");
                else if (c == '"') put("\\\"");
                else if (c == '\n') put("\\n");
                else putChar(c);
            }
        }
    };

    template <class Reader>
    static void series(Sink& s, std::string_view family, const Live& l) noexcept {
        s.put(family);
        s.put("{type=\"");
        s.putNumber(l.typeId);
        s.put("\",slot=\"");
        s.putNumber(l.slot);
        s.put("\",label=\"");
        s.putEscaped(l.label);
        s.put("\"} ");
    }

    template <class Reader, int32_t Capacity, int32_t Window>
    static void emit(const Reader& reader, const RateTracker<Capacity, Window>& rates, Sink& s) noexcept {
        s.put("# TYPE metrics_counter counter\n");
        reader.forEachLive([&](const Live& l) {
            if (l.kind != MetricKind::Counter) return;
            series<Reader>(s, "metrics_counter", l);
            s.putNumber(l.value);
            s.putChar('\n');
        });
        s.put("# TYPE metrics_gauge gauge\n");
        reader.forEachLive([&](const Live& l) {
            if (l.kind != MetricKind::Gauge) return;
            series<Reader>(s, "metrics_gauge", l);
            s.putNumber(l.value);
            s.putChar('\n');
        });
        s.put("# TYPE metrics_rate_per_second gauge\n");
        reader.forEachLive([&](const Live& l) {
            if (l.kind != MetricKind::Counter) return;
            const auto rate = rates.perSecond(l.slot);
            if (!rate) return;
            series<Reader>(s, "metrics_rate_per_second", l);
            s.putNumber(*rate);
            s.putChar('\n');
        });
    }
};

}  // namespace metrics
