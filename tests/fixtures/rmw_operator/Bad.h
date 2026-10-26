inline void bad(long& x) { std::atomic_ref<long>(x) += 1; }
