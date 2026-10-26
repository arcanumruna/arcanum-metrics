inline bool bad(std::atomic<long>& a, long& e) { return a.compare_exchange_strong(e, 1); }
