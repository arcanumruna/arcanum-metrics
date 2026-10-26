// A named atomic_ref, incremented on a later line that does not mention atomic.
#include <atomic>
inline long x;
inline void g() { auto r = std::atomic_ref<long>(x);
 ++r; }
