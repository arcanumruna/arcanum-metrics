include(GoogleTest)

# One executable per test file: AllocCounter.h replaces global operator new/delete,
# so it may be included in exactly one translation unit per executable.
function(metrics_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE metrics GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gtest_discover_tests(${name})
endfunction()

metrics_add_test(MetricsLayoutRegressionTest)
metrics_add_test(AllocCounterRegressionTest)
metrics_add_test(MetricsFileRegressionTest)
metrics_add_test(MetricsWriterRegressionTest)

# Source guard: no read-modify-write atomics or locks in the library headers.
add_test(NAME MetricsWriterRegression.NoAtomicRmwSourceRegression
         COMMAND ${CMAKE_COMMAND} -DHEADER_DIR=${PROJECT_SOURCE_DIR}/include/metrics
                 -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoAtomicRmw.cmake)
