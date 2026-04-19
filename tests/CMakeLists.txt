# ============================================================================
# ZeptoDB Tests
# ============================================================================

# --- Unit Tests ---
add_executable(zepto_tests
    unit/test_arena_allocator.cpp
    unit/test_column_store.cpp
    unit/test_ring_buffer.cpp
    unit/test_tick_plant.cpp
    unit/test_vectorized_engine.cpp
    unit/test_hdb.cpp
    unit/test_cluster.cpp
    unit/test_cxl_backend.cpp
    unit/test_sql.cpp
    unit/test_join_window.cpp
    unit/test_financial_functions.cpp
    unit/test_parallel.cpp
    unit/test_auth.cpp
    unit/test_coordinator.cpp
    unit/test_scheduler.cpp
    unit/test_features.cpp
    unit/test_kafka.cpp
    unit/test_mqtt.cpp
    unit/test_metrics_collector.cpp
    unit/test_flight_server.cpp
    unit/test_flat_hash_map.cpp
    unit/test_rebalance.cpp
    unit/test_bandwidth_throttler.cpp
    unit/test_ptp_clock_detector.cpp
    unit/test_mv_rewrite.cpp
    unit/test_license.cpp
    unit/test_composite_index.cpp
    unit/test_table_statistics.cpp
    unit/test_cost_model.cpp
    unit/test_query_planner.cpp
    unit/test_duckdb_engine.cpp
    unit/test_window_join_simd.cpp
    unit/test_table_scoped_partitioning.cpp
    unit/test_feed_table_id.cpp
    unit/test_table_acl_ddl.cpp
)
if(ZEPTO_USE_JIT)
    target_sources(zepto_tests PRIVATE unit/test_jit_simd.cpp)
endif()
target_include_directories(zepto_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cluster   # shm_backend.h access
)
target_link_libraries(zepto_tests PRIVATE
    zepto_storage
    zepto_ingestion
    zepto_execution
    zepto_core
    zepto_cluster
    zepto_sql
    zepto_auth
    zepto_scheduler
    zepto_server
    zepto_kafka
    zepto_mqtt
    zepto_flight
    zepto_feeds
    GTest::gtest_main
)
if(ZEPTO_HAS_OPENSSL)
    target_link_libraries(zepto_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(ZEPTO_USE_TCMALLOC AND TCMALLOC_LIB)
    target_link_libraries(zepto_tests PRIVATE ${TCMALLOC_LIB})
endif()

include(GoogleTest)
# Use PRE_TEST mode so we can apply per-test properties (e.g. RESOURCE_LOCK)
# via a TEST_INCLUDE_FILES hook evaluated at ctest-run time, after test
# names are materialised.
gtest_discover_tests(zepto_tests DISCOVERY_MODE PRE_TEST)

# --- Feed Handler Tests ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/feeds/test_fix_parser.cpp")
    add_executable(test_feeds
        feeds/test_fix_parser.cpp
        feeds/test_nasdaq_itch.cpp
    )
    target_include_directories(test_feeds PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(test_feeds PRIVATE
        zepto_feeds
        zepto_common
        GTest::gtest
    )
    gtest_discover_tests(test_feeds DISCOVERY_MODE PRE_TEST)
endif()

# Append AFTER all gtest_discover_tests() calls so the serial-tests script
# runs after each discovered suite's _include.cmake (which registers the
# test names). Otherwise set_tests_properties() for tests from the later
# suites silently no-ops because the names aren't known yet.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/serial_tests.cmake")

# --- Feed Handler Benchmarks ---
find_package(benchmark QUIET)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/feeds/benchmark_feed_handlers.cpp" AND benchmark_FOUND)
    add_executable(bench_feeds
        feeds/benchmark_feed_handlers.cpp
    )
    target_include_directories(bench_feeds PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(bench_feeds PRIVATE
        zepto_feeds
        zepto_common
        benchmark::benchmark
    )
    target_compile_options(bench_feeds PRIVATE -O3 -march=native)
endif()

# --- Migration Tests ---
add_executable(test_migration
    migration/test_q_to_sql.cpp
    migration/test_q_to_python.cpp
    migration/test_hdb_loader.cpp
    migration/test_clickhouse.cpp
    migration/test_duckdb.cpp
    migration/test_timescaledb.cpp
)
target_include_directories(test_migration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_migration PRIVATE
    zepto_migration
    zepto_auth
    zepto_common
    GTest::gtest
)
gtest_discover_tests(test_migration)


