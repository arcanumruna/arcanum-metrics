// Allowed: plain loads and stores through atomic_ref, and fences.
#include <atomic>
inline void ok(long& x) {
    std::atomic_ref<long> r(x);
    r.store(r.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    (void)r.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int i = 0; i < 3; ++i) {}
}
