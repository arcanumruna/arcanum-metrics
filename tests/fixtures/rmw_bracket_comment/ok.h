// Allowed. An unbalanced bracket in a comment [see the notes and a trailing backslash \
// must not merge the atomic store below with the loop counter's ++ on later lines.
#include <atomic>
inline void ok(long& x) {
    std::atomic_ref<long> r(x);
    r.store(1, std::memory_order_release);
    long n = 0;
    for (int i = 0; i < 3; ++i) n += i;  // a plain long, not an atomic
    (void)n;
}
// ]
