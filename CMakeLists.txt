cmake_minimum_required(VERSION 3.31)
project(metrics)

set(CMAKE_CXX_STANDARD 23)
add_library(metrics INTERFACE
        include/metrics/Metrics.h
        include/metrics/SystemMetrics.h
)

target_include_directories(metrics INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)


add_library(metrics_dummy STATIC
        src/dummy.cpp
        include/metrics/Metrics.h
)

target_include_directories(metrics_dummy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

install(TARGETS metrics
        EXPORT metricsTargets)

install(EXPORT metricsTargets
        FILE metricsTargets.cmake
        NAMESPACE ""
        DESTINATION lib/cmake/metrics)

install(DIRECTORY include/ DESTINATION include)