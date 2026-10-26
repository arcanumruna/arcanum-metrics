// The atomic is declared on one line and incremented on another: the check is per identifier, not per line.
#include <atomic>
struct S { std::atomic<long> c;
 void f() {
  c++;
 }
};
