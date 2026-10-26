include(GoogleTest)
find_package(Threads REQUIRED)

# One executable per test file: AllocCounter.h replaces global operator new/delete,
# so it may be included in exactly one translation unit per executable.
function(metrics_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE metrics GTest::gtest_main Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gtest_discover_tests(${name})
endfunction()

metrics_add_test(MetricsLayoutRegressionTest)
metrics_add_test(AllocCounterRegressionTest)
metrics_add_test(MetricsFileRegressionTest)
metrics_add_test(MetricsWriterRegressionTest)
metrics_add_test(MetricsReaderRegressionTest)
metrics_add_test(SystemMetricsRegressionTest)
metrics_add_test(PrometheusTextRegressionTest)
metrics_add_test(MetricsConcurrencyRegressionTest)
target_link_libraries(MetricsConcurrencyRegressionTest PRIVATE Threads::Threads)


# Source guard: no read-modify-write atomics, locks or thread_local in the library headers.
set(_guard ${PROJECT_SOURCE_DIR}/cmake/CheckNoAtomicRmw.cmake)
add_test(NAME MetricsWriterRegression.NoAtomicRmwSourceRegression
         COMMAND ${CMAKE_COMMAND} -DHEADER_DIR=${PROJECT_SOURCE_DIR}/include -P ${_guard})
add_test(NAME MetricsWriterRegression.NoAtomicRmwAllowsLoadStoreRegression
         COMMAND ${CMAKE_COMMAND} -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rmw_ok -P ${_guard})
# Brackets, semicolons and a trailing backslash in the source must not merge lines into a false report.
add_test(NAME MetricsWriterRegression.NoAtomicRmwBracketCommentRegression
         COMMAND ${CMAKE_COMMAND} -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rmw_bracket_comment -P ${_guard})
# The guard must fire: each fixture holds one forbidden construct and the script must report it.
foreach(_case rmw_fetch_add rmw_operator rmw_mutex rmw_cas rmw_thread_local rmw_multiline rmw_inc_ref
              rmw_semicolon)
    add_test(NAME MetricsWriterRegression.NoAtomicRmwFiresRegression.${_case}
             COMMAND ${CMAKE_COMMAND} -DHEADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${_case} -P ${_guard})
    set_tests_properties(MetricsWriterRegression.NoAtomicRmwFiresRegression.${_case} PROPERTIES
            PASS_REGULAR_EXPRESSION "forbidden")
endforeach()
