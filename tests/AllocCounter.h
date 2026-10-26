#pragma once
// Counts global operator new calls inside an AllocScope. Replaces the global allocation
// functions, so include it from exactly one translation unit per test executable.
#include <cstdint>
#include <cstdlib>
#include <new>

namespace metrics::testing {
inline int64_t g_allocCount = 0;
inline bool g_allocCounting = false;

class AllocScope {
public:
    AllocScope() noexcept { g_allocCount = 0; g_allocCounting = true; }
    ~AllocScope() { g_allocCounting = false; }
    AllocScope(const AllocScope&) = delete;
    AllocScope& operator=(const AllocScope&) = delete;
    [[nodiscard]] int64_t count() const noexcept { return g_allocCount; }
};
}  // namespace metrics::testing

// Every global form is replaced so each allocation pairs with its own release (ASan checks this).
void* operator new(std::size_t n) {
    if (metrics::testing::g_allocCounting) ++metrics::testing::g_allocCount;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (metrics::testing::g_allocCounting) ++metrics::testing::g_allocCount;
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return ::operator new(n, t); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (metrics::testing::g_allocCounting) ++metrics::testing::g_allocCount;
    if (void* p = std::aligned_alloc(static_cast<std::size_t>(a), (n + static_cast<std::size_t>(a) - 1) &
                                                                   ~(static_cast<std::size_t>(a) - 1)))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
