cmake_minimum_required(VERSION 3.31)
project(metrics)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(METRICS_BUILD_TESTS "Build the library tests" OFF)
option(METRICS_BUILD_BENCH "Build the library benchmarks" OFF)
set(METRICS_SANITIZE "" CACHE STRING "Sanitizers for tests and bench, e.g. address,undefined or thread")

add_library(metrics INTERFACE
        include/metrics/Metrics.h
        include/metrics/MetricsLayout.h
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

if(METRICS_SANITIZE)
    add_compile_options(-fsanitize=${METRICS_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${METRICS_SANITIZE})
endif()

if(METRICS_BUILD_TESTS OR METRICS_BUILD_BENCH)
    include(FetchContent)
endif()

if(METRICS_BUILD_TESTS)
    FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.16.0
            GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(METRICS_BUILD_BENCH)
    FetchContent_Declare(benchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.8.2
            GIT_SHALLOW TRUE)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    add_subdirectory(bench)
endif()

install(TARGETS metrics
        EXPORT metricsTargets)

install(EXPORT metricsTargets
        FILE metricsTargets.cmake
        NAMESPACE ""
        DESTINATION lib/cmake/metrics)

install(DIRECTORY include/ DESTINATION include)
