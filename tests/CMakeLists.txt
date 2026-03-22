# ============================================================================
# APEX-DB Tests
# ============================================================================

# --- Unit Tests ---
add_executable(apex_tests
    unit/test_arena_allocator.cpp
    unit/test_column_store.cpp
    unit/test_ring_buffer.cpp
    unit/test_tick_plant.cpp
    unit/test_vectorized_engine.cpp
    unit/test_hdb.cpp
    unit/test_cluster.cpp
    unit/test_cxl_backend.cpp
)
target_include_directories(apex_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cluster   # shm_backend.h 접근
)
target_link_libraries(apex_tests PRIVATE
    apex_storage
    apex_ingestion
    apex_execution
    apex_core
    apex_cluster
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(apex_tests)
