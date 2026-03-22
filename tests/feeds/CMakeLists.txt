# ============================================================================
# APEX-DB Feed Handler Tests
# ============================================================================

# Google Test
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Google Benchmark (optional)
find_package(benchmark QUIET)

# ============================================================================
# Unit Tests
# ============================================================================

# FIX Parser Tests
add_executable(test_fix_parser
    test_fix_parser.cpp
    ../../src/feeds/fix_parser.cpp
)
target_link_libraries(test_fix_parser
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME test_fix_parser COMMAND test_fix_parser)

# NASDAQ ITCH Parser Tests
add_executable(test_nasdaq_itch
    test_nasdaq_itch.cpp
    ../../src/feeds/nasdaq_itch.cpp
)
target_link_libraries(test_nasdaq_itch
    GTest::gtest
    GTest::gtest_main
    pthread
)
add_test(NAME test_nasdaq_itch COMMAND test_nasdaq_itch)

# ============================================================================
# Benchmark Tests (if benchmark library found)
# ============================================================================

if(benchmark_FOUND)
    add_executable(benchmark_feed_handlers
        benchmark_feed_handlers.cpp
        ../../src/feeds/fix_parser.cpp
        ../../src/feeds/nasdaq_itch.cpp
        ../../src/feeds/optimized/fix_parser_fast.cpp
    )

    target_link_libraries(benchmark_feed_handlers
        benchmark::benchmark
        pthread
    )

    # AVX2 플래그 (최적화)
    if(CMAKE_BUILD_TYPE MATCHES Release)
        target_compile_options(benchmark_feed_handlers PRIVATE
            -march=native
            -mavx2
            -O3
            -flto
        )
    endif()

    message(STATUS "Google Benchmark found - building benchmark tests")
else()
    message(STATUS "Google Benchmark not found - skipping benchmark tests")
    message(STATUS "Install with: sudo apt-get install libbenchmark-dev")
endif()

# ============================================================================
# Optimization Flags
# ============================================================================

if(CMAKE_BUILD_TYPE MATCHES Release)
    # 베어메탈 최적화
    target_compile_options(test_fix_parser PRIVATE -O3 -march=native)
    target_compile_options(test_nasdaq_itch PRIVATE -O3 -march=native)
endif()
