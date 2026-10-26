// The declaration and the increment share one line, separated by a semicolon.
#include <atomic>
inline void f() { std::atomic<int> a{0}; a++; }
