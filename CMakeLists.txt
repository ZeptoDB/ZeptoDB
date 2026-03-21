# ============================================================================
# APEX-DB: Ultra-Low Latency In-Memory Database for HFT
# Root CMakeLists.txt
# ============================================================================
cmake_minimum_required(VERSION 3.22)

project(apex-db
    VERSION 0.1.0
    DESCRIPTION "Ultra-Low Latency In-Memory Database for HFT"
    LANGUAGES CXX C
)

# ============================================================================
# Global Settings
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================================
# Options
# ============================================================================
option(APEX_BUILD_TESTS    "Build unit tests"           ON)
option(APEX_BUILD_BENCH    "Build benchmarks"           ON)
option(APEX_USE_HUGEPAGES  "Enable HugePages support"   ON)
option(APEX_USE_UCX        "Enable UCX (RDMA/CXL)"      ON)
option(APEX_USE_JIT        "Enable LLVM JIT engine"      ON)
option(APEX_USE_HIGHWAY    "Enable Highway SIMD"         ON)

# ============================================================================
# Compiler Flags
# ============================================================================
add_compile_options(-Wall -Wextra -Wpedantic -fno-omit-frame-pointer)

# clang이 GCC 11 sysroot의 오래된 libstdc++ (6.0.29)를 먼저 찾는 문제 회피
# GCC 14의 libstdc++ (6.0.33, GLIBCXX_3.4.30+) 경로를 우선 지정
link_directories(BEFORE /usr/lib64)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

# ============================================================================
# Dependencies
# ============================================================================

# --- spdlog & fmt (FetchContent) ---
include(FetchContent)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.1.4
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# FlatBuffers for AST serialization (Layer 4)
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v24.12.23
    GIT_SHALLOW    TRUE
)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt spdlog)

if(APEX_BUILD_TESTS)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# --- NUMA ---
find_library(NUMA_LIB numa REQUIRED)

# --- Highway (SIMD) ---
if(APEX_USE_HIGHWAY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HWY REQUIRED IMPORTED_TARGET libhwy)
    message(STATUS "Highway SIMD: ${HWY_VERSION}")
endif()

# --- UCX (RDMA/CXL communication) ---
if(APEX_USE_UCX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx)
    message(STATUS "UCX: ${UCX_VERSION}")
endif()

# --- LLVM (JIT) ---
if(APEX_USE_JIT)
    find_package(LLVM REQUIRED CONFIG
        PATHS /usr/lib64/llvm19/lib/cmake/llvm
        NO_DEFAULT_PATH
    )
    message(STATUS "LLVM ${LLVM_VERSION}: ${LLVM_DIR}")
    include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
    separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
    add_definitions(${LLVM_DEFINITIONS_LIST})
endif()

# ============================================================================
# APEX Libraries
# ============================================================================

# --- Common utilities ---
add_library(apex_common STATIC
    src/common/logger.cpp
)
target_include_directories(apex_common PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_common PUBLIC
    spdlog::spdlog
    fmt::fmt
)

# --- Layer 1: Storage Engine ---
add_library(apex_storage STATIC
    src/storage/arena_allocator.cpp
    src/storage/column_store.cpp
    src/storage/partition_manager.cpp
)
target_include_directories(apex_storage PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_storage PUBLIC
    apex_common
    ${NUMA_LIB}
)
if(APEX_USE_HIGHWAY)
    target_link_libraries(apex_storage PUBLIC PkgConfig::HWY)
endif()

# --- Layer 2: Ingestion (Tick Plant) ---
add_library(apex_ingestion STATIC
    src/ingestion/ring_buffer.cpp
    src/ingestion/tick_plant.cpp
    src/ingestion/wal_writer.cpp
)
target_include_directories(apex_ingestion PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_ingestion PUBLIC
    apex_common
    apex_storage
)
if(APEX_USE_UCX)
    target_link_libraries(apex_ingestion PUBLIC PkgConfig::UCX)
endif()

# --- Layer 3: Execution Engine ---
add_library(apex_execution STATIC
    src/execution/vectorized_engine.cpp
    src/execution/query_planner.cpp
    src/execution/jit_engine.cpp
)
target_include_directories(apex_execution PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_execution PUBLIC
    apex_common
    apex_storage
)
if(APEX_USE_HIGHWAY)
    target_link_libraries(apex_execution PUBLIC PkgConfig::HWY)
endif()
if(APEX_USE_JIT)
    # LLVM-19 shared lib + GCC 11의 libstdc++ 경로 명시 (clang 기본 경로 우선순위 문제 해결)
    target_link_libraries(apex_execution PUBLIC
        /usr/lib64/llvm19/lib/libLLVM-19.so
        /usr/lib/gcc/x86_64-amazon-linux/11/libstdc++.so
    )
endif()

# --- Core Pipeline (Layer 1+2+3 통합) ---
add_library(apex_core STATIC
    src/core/pipeline.cpp
)
target_include_directories(apex_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_core PUBLIC
    apex_common
    apex_storage
    apex_ingestion
    apex_execution
)

# ============================================================================
# Tests
# ============================================================================
if(APEX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ============================================================================
# Benchmarks
# ============================================================================
if(APEX_BUILD_BENCH)
    add_executable(bench_pipeline
        tests/bench/bench_pipeline.cpp
    )
    target_link_libraries(bench_pipeline PRIVATE
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_pipeline PRIVATE
        -O3 -march=native
    )

    add_executable(bench_simd_jit
        tests/bench/bench_simd_jit.cpp
    )
    target_link_libraries(bench_simd_jit PRIVATE
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_simd_jit PRIVATE
        -O3 -march=native
    )
endif()

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== APEX-DB v${PROJECT_VERSION} Build Config ===")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  HugePages:     ${APEX_USE_HUGEPAGES}")
message(STATUS "  UCX (RDMA):    ${APEX_USE_UCX}")
message(STATUS "  Highway SIMD:  ${APEX_USE_HIGHWAY}")
message(STATUS "  LLVM JIT:      ${APEX_USE_JIT}")
message(STATUS "  Tests:         ${APEX_BUILD_TESTS}")
message(STATUS "=================================")
