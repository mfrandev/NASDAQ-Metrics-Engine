set(ENGINE_SRC ${CMAKE_SOURCE_DIR}/src)

add_executable(metrics_tests
    metrics_tests.cpp
    ${ENGINE_SRC}/ledger/PerStockLedger.cpp
    ${ENGINE_SRC}/orderstate/PerStockOrderState.cpp
)

target_include_directories(metrics_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/er
    ${ENGINE_SRC}
    ${ENGINE_SRC}/messages
    ${ENGINE_SRC}/orderstate
    ${ENGINE_SRC}/utils
    ${ENGINE_SRC}/shard
    ${ENGINE_SRC}/events
    ${ENGINE_SRC}/ledger
    ${ENGINE_SRC}/vwap
    ${ENGINE_SRC}/twap
    ${ENGINE_SRC}/rv
)

target_link_libraries(metrics_tests PRIVATE unordered_dense::unordered_dense spdlog::spdlog)

# Same options as the engine target so the tests compile the headers identically
target_compile_options(metrics_tests PRIVATE -march=native --param=destructive-interference-size=64)

add_test(NAME metrics_unit_tests COMMAND metrics_tests unit)

# Oracle-driven scenarios: a Python oracle produces expected.csv, then the
# engine replays the same stream and compares.
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python3 not found: oracle-driven metric tests will not be registered")
    return()
endif()

set(ORACLE ${CMAKE_CURRENT_SOURCE_DIR}/oracle/metrics_oracle.py)

foreach(scenario IN ITEMS no_breaks_dense_queries no_breaks_sparse_queries broken_trades)
    set(scenario_dir ${CMAKE_CURRENT_SOURCE_DIR}/data/${scenario})
    set(expected ${CMAKE_CURRENT_BINARY_DIR}/expected_${scenario}.csv)

    add_test(NAME oracle_generate_${scenario}
        COMMAND ${Python3_EXECUTABLE} -I ${ORACLE} ${scenario_dir}/events.csv ${scenario_dir}/queries.csv ${expected})
    set_tests_properties(oracle_generate_${scenario} PROPERTIES FIXTURES_SETUP oracle_${scenario})

    add_test(NAME metrics_oracle_${scenario}
        COMMAND metrics_tests oracle ${scenario_dir}/events.csv ${scenario_dir}/queries.csv ${expected})
    set_tests_properties(metrics_oracle_${scenario} PROPERTIES FIXTURES_REQUIRED oracle_${scenario})
endforeach()
