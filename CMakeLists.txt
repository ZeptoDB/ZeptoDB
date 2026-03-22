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
# Python .so 링크를 위해 모든 정적 라이브러리에 -fPIC 적용
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
option(APEX_USE_LZ4        "Enable LZ4 compression"     ON)
option(APEX_USE_PARQUET    "Enable Apache Parquet output" ON)
option(APEX_USE_S3         "Enable AWS S3 upload"        ON)

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

# --- Apache Arrow + Parquet (optional) ---
if(APEX_USE_PARQUET)
    find_package(Arrow CONFIG QUIET)
    find_package(Parquet CONFIG QUIET)
    if(Arrow_FOUND AND Parquet_FOUND)
        message(STATUS "Arrow: ${Arrow_VERSION}, Parquet: ${Parquet_VERSION} (Parquet 활성화)")
    else()
        pkg_check_modules(ARROW IMPORTED_TARGET arrow QUIET)
        pkg_check_modules(PARQUET IMPORTED_TARGET parquet QUIET)
        if(ARROW_FOUND AND PARQUET_FOUND)
            message(STATUS "Arrow (pkg-config): ${ARROW_VERSION} (Parquet 활성화)")
        else()
            message(STATUS "Arrow/Parquet: 없음 — Parquet 저장 비활성화")
            message(STATUS "  설치: sudo dnf install -y arrow-devel parquet-devel")
            set(APEX_USE_PARQUET OFF)
        endif()
    endif()
endif()

# --- AWS SDK C++ S3 (optional) ---
if(APEX_USE_S3)
    find_package(AWSSDK CONFIG COMPONENTS s3 QUIET)
    if(AWSSDK_FOUND)
        message(STATUS "AWS SDK C++: ${AWSSDK_VERSION} (S3 업로드 활성화)")
    else()
        message(STATUS "AWS SDK C++: 없음 — S3 업로드 비활성화")
        message(STATUS "  설치: sudo dnf install -y aws-sdk-cpp-s3")
        set(APEX_USE_S3 OFF)
    endif()
endif()

# --- LZ4 (optional) ---
if(APEX_USE_LZ4)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LZ4 IMPORTED_TARGET liblz4)
    if(LZ4_FOUND)
        message(STATUS "LZ4: ${LZ4_VERSION} (압축 활성화)")
    else()
        message(STATUS "LZ4: 없음 — 압축 비활성화 (패스스루 모드)")
        set(APEX_USE_LZ4 OFF)
    endif()
endif()

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
    src/storage/hdb_writer.cpp
    src/storage/hdb_reader.cpp
    src/storage/flush_manager.cpp
    src/storage/parquet_writer.cpp
    src/storage/s3_sink.cpp
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
if(APEX_USE_LZ4)
    target_link_libraries(apex_storage PUBLIC PkgConfig::LZ4)
    target_compile_definitions(apex_storage PUBLIC APEX_HDB_FORCE_LZ4=1)
endif()
if(APEX_USE_PARQUET)
    if(Arrow_FOUND AND Parquet_FOUND)
        target_link_libraries(apex_storage PUBLIC Arrow::arrow_shared Parquet::parquet_shared)
    elseif(ARROW_FOUND AND PARQUET_FOUND)
        target_link_libraries(apex_storage PUBLIC PkgConfig::ARROW PkgConfig::PARQUET)
    endif()
    target_compile_definitions(apex_storage PUBLIC APEX_PARQUET_ENABLED=1)
endif()
if(APEX_USE_S3)
    target_link_libraries(apex_storage PUBLIC ${AWSSDK_LINK_LIBRARIES})
    target_compile_definitions(apex_storage PUBLIC APEX_S3_ENABLED=1)
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
    src/execution/join_operator.cpp
    src/execution/worker_pool.cpp
    src/execution/parallel_scan.cpp
    src/execution/local_scheduler.cpp
    src/execution/distributed_scheduler.cpp
)
if(APEX_USE_JIT)
    target_sources(apex_execution PRIVATE src/execution/jit_engine.cpp)
endif()
target_include_directories(apex_execution PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
find_package(Threads REQUIRED)
target_link_libraries(apex_execution PUBLIC
    apex_common
    apex_storage
    Threads::Threads
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

# --- Layer 5: SQL Parser + Executor ---
add_library(apex_sql STATIC
    src/sql/tokenizer.cpp
    src/sql/parser.cpp
    src/sql/executor.cpp
    src/execution/join_operator.cpp
)
target_include_directories(apex_sql PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_sql PUBLIC
    apex_core
)

# --- Layer 6: HTTP API Server ---
add_library(apex_server STATIC
    src/server/http_server.cpp
)
target_include_directories(apex_server PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}  # third_party/httplib.h 접근용
)
target_link_libraries(apex_server PUBLIC
    apex_sql
)
# httplib은 POSIX threads 필요
find_package(Threads REQUIRED)
target_link_libraries(apex_server PUBLIC Threads::Threads)

# --- Phase C: Cluster (분산 메모리 & 트랜스포트) ---
add_library(apex_cluster STATIC
    src/cluster/shm_backend.cpp
    src/cluster/ucx_backend.cpp
)
target_include_directories(apex_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cluster   # shm_backend.h, ucx_backend.h (internal)
)
target_link_libraries(apex_cluster PUBLIC
    apex_core
)
# UCX 발견 시 APEX_HAS_UCX 정의 + 링크
if(APEX_USE_UCX AND UCX_FOUND)
    target_compile_definitions(apex_cluster PUBLIC APEX_HAS_UCX=1)
    target_link_libraries(apex_cluster PUBLIC PkgConfig::UCX)
endif()

# --- Migration Toolkit (kdb+, ClickHouse, DuckDB, TimescaleDB) ---
add_library(apex_migration STATIC
    src/migration/q_lexer.cpp
    src/migration/q_parser.cpp
    src/migration/q_to_sql.cpp
    src/migration/hdb_loader.cpp
    src/migration/clickhouse_migrator.cpp
    src/migration/duckdb_interop.cpp
    src/migration/timescaledb_migrator.cpp
)
target_include_directories(apex_migration PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_migration PUBLIC
    apex_common
)

# --- Feed Handler Adapters ---
add_library(apex_feeds STATIC
    src/feeds/fix_parser.cpp
    src/feeds/nasdaq_itch.cpp
)
target_include_directories(apex_feeds PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex_feeds PUBLIC
    apex_common
)

# ============================================================================
# Tools
# ============================================================================
add_executable(apex-migrate
    tools/apex-migrate.cpp
)
target_include_directories(apex-migrate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex-migrate PRIVATE
    apex_migration
    apex_common
)
target_compile_options(apex-migrate PRIVATE -O2)

# Interactive SQL CLI
add_executable(apex-cli
    tools/apex-cli.cpp
)
target_include_directories(apex-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(apex-cli PRIVATE
    apex_common
)
# Link readline if available
find_library(READLINE_LIB readline)
if(READLINE_LIB)
    target_link_libraries(apex-cli PRIVATE ${READLINE_LIB})
    target_compile_definitions(apex-cli PRIVATE HAVE_READLINE=1)
endif()
target_compile_options(apex-cli PRIVATE -O2)

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
        apex_common
    )
    target_compile_options(bench_simd_jit PRIVATE
        -O3 -march=native
    )

    add_executable(bench_hdb
        tests/bench/bench_hdb.cpp
    )
    target_link_libraries(bench_hdb PRIVATE
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_hdb PRIVATE
        -O3 -march=native
    )

    # Phase C: Cluster 벤치마크
    add_executable(bench_cluster
        tests/bench/bench_cluster.cpp
    )
    target_include_directories(bench_cluster PRIVATE
        ${CMAKE_SOURCE_DIR}/src/cluster   # shm_backend.h 접근
    )
    target_link_libraries(bench_cluster PRIVATE
        apex_cluster
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_cluster PRIVATE
        -O3 -march=native
    )

    # Phase C: CXL Transport 벤치마크
    add_executable(bench_cxl
        tests/bench/bench_cxl.cpp
    )
    target_include_directories(bench_cxl PRIVATE
        ${CMAKE_SOURCE_DIR}/src/cluster
    )
    target_link_libraries(bench_cxl PRIVATE
        apex_cluster
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_cxl PRIVATE
        -O3 -march=native
    )
    # SQL 벤치마크
    add_executable(bench_sql
        tests/bench/bench_sql.cpp
    )
    target_link_libraries(bench_sql PRIVATE
        apex_sql
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_sql PRIVATE
        -O3 -march=native
    )

    # 병렬 쿼리 벤치마크
    add_executable(bench_parallel
        tests/bench/bench_parallel.cpp
    )
    target_link_libraries(bench_parallel PRIVATE
        apex_sql
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(bench_parallel PRIVATE
        -O3 -march=native
    )
endif()

# ============================================================================
# Phase D: Python Binding (pybind11)
# ============================================================================
option(APEX_BUILD_PYTHON "Build Python binding module" ON)

if(APEX_BUILD_PYTHON)
    # Python 개발 헤더
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
    message(STATUS "Python3: ${Python3_VERSION} (${Python3_EXECUTABLE})")

    # pybind11 — pip 설치 경로에서 CMake config 가져오기
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(PYBIND11_CMAKE_DIR)
        list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
        message(STATUS "pybind11 CMake dir: ${PYBIND11_CMAKE_DIR}")
    endif()

    find_package(pybind11 CONFIG REQUIRED)
    message(STATUS "pybind11: ${pybind11_VERSION}")

    # NumPy include 경로
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import numpy; print(numpy.get_include())"
        OUTPUT_VARIABLE NUMPY_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    message(STATUS "NumPy include: ${NUMPY_INCLUDE_DIR}")

    # Python 모듈 빌드
    pybind11_add_module(apex
        src/transpiler/python_binding.cpp
        src/python/arrow_bridge.cpp
        src/python/dataframe_bridge.cpp
    )
    target_include_directories(apex PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${NUMPY_INCLUDE_DIR}
    )
    target_link_libraries(apex PRIVATE
        apex_core
        apex_storage
        apex_ingestion
        apex_execution
    )
    target_compile_options(apex PRIVATE
        -O3 -march=native
        # pybind11이 -Wpedantic에서 일부 경고 발생 → 억제
        -Wno-pedantic
        -Wno-unused-parameter
    )

    # 빌드 디렉토리에 apex_py 패키지 복사 (테스트 시 import 용이)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/apex_py")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/transpiler/apex_py/dsl.py"
        "${CMAKE_BINARY_DIR}/apex_py/dsl.py"
        COPYONLY
    )

    # apex_py Python 패키지 파일 복사
    foreach(_py_file
        __init__.py
        connection.py
        dataframe.py
        streaming.py
        arrow.py
        utils.py
    )
        configure_file(
            "${CMAKE_SOURCE_DIR}/apex_py/${_py_file}"
            "${CMAKE_BINARY_DIR}/apex_py/${_py_file}"
            COPYONLY
        )
    endforeach()

    message(STATUS "Python module: apex (+ apex_py pandas/polars/arrow integration)")
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
message(STATUS "  LZ4:           ${APEX_USE_LZ4}")
message(STATUS "  Parquet:       ${APEX_USE_PARQUET}")
message(STATUS "  S3 Upload:     ${APEX_USE_S3}")
message(STATUS "  Tests:         ${APEX_BUILD_TESTS}")
message(STATUS "=================================")
