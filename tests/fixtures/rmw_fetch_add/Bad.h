inline void bad(std::atomic<long>& a) { a.fetch_add(1); }
