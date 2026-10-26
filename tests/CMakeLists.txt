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
