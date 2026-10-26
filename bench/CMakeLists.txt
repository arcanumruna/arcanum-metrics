add_executable(MetricsBench MetricsBench.cpp)
target_link_libraries(MetricsBench PRIVATE metrics benchmark::benchmark_main)
