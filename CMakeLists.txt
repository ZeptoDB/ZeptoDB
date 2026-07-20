# ============================================================================
# ZeptoDB: Ultra-Low Latency In-Memory Database for HFT
# Root CMakeLists.txt
# ============================================================================
cmake_minimum_required(VERSION 3.22)

# ============================================================================
# Compiler soft-default: prefer clang-19 when user didn't pick a compiler.
# Respects env vars CC/CXX and explicit -DCMAKE_*_COMPILER. Keeps gcc builds
# possible (pass -DCMAKE_CXX_COMPILER=g++) but makes the common case match the
# documented clang-19 toolchain (see docs/getting-started/build.md).
# ============================================================================
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(ZEPTO_CLANGXX19 NAMES clang++-19 clang++ PATHS /usr/bin /usr/local/bin)
    if(ZEPTO_CLANGXX19)
        set(CMAKE_CXX_COMPILER "${ZEPTO_CLANGXX19}" CACHE FILEPATH "C++ compiler" FORCE)
        message(STATUS "Auto-selected C++ compiler: ${CMAKE_CXX_COMPILER}")
    endif()
endif()
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(ZEPTO_CLANG19 NAMES clang-19 clang PATHS /usr/bin /usr/local/bin)
    if(ZEPTO_CLANG19)
        set(CMAKE_C_COMPILER "${ZEPTO_CLANG19}" CACHE FILEPATH "C compiler" FORCE)
    endif()
endif()

project(zeptodb
    VERSION 0.1.8
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
option(ZEPTO_BUILD_TESTS    "Build unit tests"           ON)
option(ZEPTO_BUILD_BENCH    "Build benchmarks"           ON)
option(ZEPTO_USE_HUGEPAGES  "Enable HugePages support"   ON)
option(ZEPTO_USE_UCX        "Enable UCX (RDMA/CXL)"      ON)
option(ZEPTO_USE_JIT        "Enable LLVM JIT engine"      ON)
option(ZEPTO_USE_HIGHWAY    "Enable Highway SIMD"         ON)
option(ZEPTO_USE_LZ4        "Enable LZ4 compression"     ON)
option(ZEPTO_USE_PARQUET    "Enable Apache Parquet output" ON)
option(ZEPTO_USE_S3         "Enable AWS S3 upload"        ON)
option(ZEPTO_USE_TCMALLOC   "Link tcmalloc_minimal for HFT allocation performance" OFF)
option(ZEPTO_USE_LTO        "Enable Link-Time Optimization (GCC -flto)" OFF)
option(ZEPTO_USE_KAFKA      "Enable Apache Kafka consumer (requires librdkafka++)" OFF)
option(ZEPTO_USE_MQTT       "Enable MQTT consumer (requires paho-mqttpp3)" OFF)
option(ZEPTO_USE_KINESIS    "Enable AWS Kinesis consumer (requires AWS SDK C++ kinesis)" OFF)
option(ZEPTO_USE_PULSAR     "Enable Apache Pulsar consumer (requires pulsar-client-cpp)" OFF)
option(ZEPTO_USE_OPCUA      "Enable OPC-UA consumer (requires open62541)" OFF)
option(ZEPTO_USE_ROS2       "Enable ROS 2 scalar subscriber and rosbag2 import/replay (requires ROS 2 packages)" OFF)
option(ZEPTO_USE_FLIGHT    "Enable Arrow Flight RPC server"  ON)
option(ZEPTO_ENABLE_DUCKDB "Enable embedded DuckDB engine for Parquet offload" ON)
option(ZEPTO_ENABLE_HNSWLIB "Enable optional hnswlib backend for Agent Memory ANN" OFF)

# ============================================================================
# Platform-specific prefix paths for find_package()
# ============================================================================
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(HOMEBREW_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH
            "${HOMEBREW_PREFIX}/opt/llvm"
            "${HOMEBREW_PREFIX}/opt/apache-arrow"
            "${HOMEBREW_PREFIX}/opt/aws-sdk-cpp"
            "${HOMEBREW_PREFIX}/opt/lz4"
            "${HOMEBREW_PREFIX}/opt/openssl@3"
        )
    endif()
elseif(UNIX)
    # AWS SDK, librdkafka etc. install to /usr/local on Linux
    list(APPEND CMAKE_PREFIX_PATH "/usr/local")
endif()

# ============================================================================
# Compiler Flags
# ============================================================================
add_compile_options(-Wall -Wextra -Wpedantic -fno-omit-frame-pointer)

# Prevent Clang from selecting an older GCC 11 libstdc++ (6.0.29) before the
# GCC 14 libstdc++ (6.0.33, GLIBCXX_3.4.30+) on Linux hosts.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    link_directories(BEFORE /usr/lib64)
endif()

# Keep the default release profile portable. Deployment/release jobs may add a
# deliberate architecture baseline through CMAKE_CXX_FLAGS(_RELEASE), while
# local benchmark targets opt into native tuning independently.
add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>)
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

# LTO: applied globally when enabled. Uses -flto=thin on Clang for faster
# link times, falls back to -flto on GCC.
if(ZEPTO_USE_LTO)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-flto=thin" HAS_LTO_THIN)
    if(HAS_LTO_THIN)
        set(ZEPTO_LTO_FLAG "-flto=thin")
    else()
        set(ZEPTO_LTO_FLAG "-flto")
    endif()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${ZEPTO_LTO_FLAG}")
    set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${ZEPTO_LTO_FLAG}")
    set(CMAKE_AR      "${CMAKE_CXX_COMPILER_AR}")
    set(CMAKE_RANLIB  "${CMAKE_CXX_COMPILER_RANLIB}")
    message(STATUS "LTO: enabled (${ZEPTO_LTO_FLAG})")
endif()

# tcmalloc: link tcmalloc_minimal for HFT workloads with many small allocations
if(ZEPTO_USE_TCMALLOC)
    find_library(TCMALLOC_LIB tcmalloc_minimal REQUIRED)
    message(STATUS "tcmalloc_minimal: ${TCMALLOC_LIB}")
endif()

# --- librdkafka++ (Kafka consumer, optional) ---
if(ZEPTO_USE_KAFKA)
    find_library(RDKAFKA_CPP_LIB rdkafka++ QUIET)
    find_library(RDKAFKA_LIB     rdkafka   QUIET)
    if(RDKAFKA_CPP_LIB AND RDKAFKA_LIB)
        message(STATUS "librdkafka++: ${RDKAFKA_CPP_LIB} (Kafka consumer enabled)")
        set(ZEPTO_HAS_KAFKA ON)
    else()
        message(STATUS "librdkafka++: not found — Kafka consumer disabled")
        message(STATUS "  Install: sudo dnf install -y librdkafka-devel")
        set(ZEPTO_USE_KAFKA OFF)
        set(ZEPTO_HAS_KAFKA OFF)
    endif()
else()
    set(ZEPTO_HAS_KAFKA OFF)
endif()

# --- paho-mqttpp3 (MQTT consumer, optional) ---
if(ZEPTO_USE_MQTT)
    find_library(PAHO_MQTTPP_LIB paho-mqttpp3 QUIET)
    find_library(PAHO_MQTT_LIB   paho-mqtt3a  QUIET)
    if(PAHO_MQTTPP_LIB AND PAHO_MQTT_LIB)
        message(STATUS "paho-mqttpp3: ${PAHO_MQTTPP_LIB} (MQTT consumer enabled)")
        set(ZEPTO_HAS_MQTT ON)
    else()
        message(STATUS "paho-mqttpp3: not found — MQTT consumer disabled")
        message(STATUS "  Install: sudo dnf install -y paho-c-devel paho-cpp-devel")
        set(ZEPTO_USE_MQTT OFF)
        set(ZEPTO_HAS_MQTT OFF)
    endif()
else()
    set(ZEPTO_HAS_MQTT OFF)
endif()

# --- Apache Pulsar C++ client (optional) ---
if(ZEPTO_USE_PULSAR)
    find_path(PULSAR_CPP_INCLUDE_DIR pulsar/Client.h QUIET)
    find_library(PULSAR_CPP_LIB NAMES pulsar pulsarwithdeps QUIET)
    if(PULSAR_CPP_INCLUDE_DIR AND PULSAR_CPP_LIB)
        message(STATUS "pulsar-client-cpp: ${PULSAR_CPP_LIB} (Pulsar consumer enabled)")
        set(ZEPTO_HAS_PULSAR ON)
    else()
        message(STATUS "pulsar-client-cpp: not found - Pulsar consumer disabled")
        message(STATUS "  Install: sudo dnf install -y pulsar-client-devel")
        set(ZEPTO_USE_PULSAR OFF)
        set(ZEPTO_HAS_PULSAR OFF)
    endif()
else()
    set(ZEPTO_HAS_PULSAR OFF)
endif()

# --- open62541 (OPC-UA consumer, optional) ---
if(ZEPTO_USE_OPCUA)
    set(ZEPTO_OPCUA_TARGET "")
    set(ZEPTO_OPCUA_DETECTION_METHOD "")
    set(ZEPTO_OPCUA_INCLUDE_DIRS "")
    set(ZEPTO_OPCUA_VERSION "")
    set(ZEPTO_OPCUA_HAS_HISTORIZING OFF)

    find_package(open62541 CONFIG QUIET)
    if(open62541_FOUND)
        if(TARGET open62541::open62541)
            set(ZEPTO_OPCUA_TARGET open62541::open62541)
        elseif(TARGET open62541)
            set(ZEPTO_OPCUA_TARGET open62541)
        endif()
        set(ZEPTO_OPCUA_DETECTION_METHOD "CMake package")
        if(DEFINED open62541_VERSION)
            set(ZEPTO_OPCUA_VERSION "${open62541_VERSION}")
        endif()
    endif()

    if(NOT ZEPTO_OPCUA_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(OPEN62541_PC QUIET IMPORTED_TARGET open62541)
            if(OPEN62541_PC_FOUND AND TARGET PkgConfig::OPEN62541_PC)
                set(ZEPTO_OPCUA_TARGET PkgConfig::OPEN62541_PC)
                set(ZEPTO_OPCUA_DETECTION_METHOD "pkg-config")
                set(ZEPTO_OPCUA_INCLUDE_DIRS ${OPEN62541_PC_INCLUDE_DIRS})
                set(ZEPTO_OPCUA_VERSION "${OPEN62541_PC_VERSION}")
            endif()
        endif()
    endif()

    if(NOT ZEPTO_OPCUA_TARGET)
        find_path(OPEN62541_INCLUDE_DIR NAMES open62541/client.h)
        find_library(OPEN62541_LIBRARY NAMES open62541)
        if(OPEN62541_INCLUDE_DIR AND OPEN62541_LIBRARY)
            add_library(zepto_open62541 UNKNOWN IMPORTED)
            set_target_properties(zepto_open62541 PROPERTIES
                IMPORTED_LOCATION "${OPEN62541_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${OPEN62541_INCLUDE_DIR}"
            )
            set(ZEPTO_OPCUA_TARGET zepto_open62541)
            set(ZEPTO_OPCUA_DETECTION_METHOD "find_path/find_library")
            set(ZEPTO_OPCUA_INCLUDE_DIRS "${OPEN62541_INCLUDE_DIR}")

            set(_open62541_config "${OPEN62541_INCLUDE_DIR}/open62541/config.h")
            if(EXISTS "${_open62541_config}")
                foreach(_ver_part MAJOR MINOR PATCH)
                    file(STRINGS "${_open62541_config}" _open62541_${_ver_part}
                         REGEX "^#define[ \t]+UA_OPEN62541_VER_${_ver_part}[ \t]+[0-9]+")
                    if(_open62541_${_ver_part})
                        string(REGEX REPLACE ".*UA_OPEN62541_VER_${_ver_part}[ \t]+([0-9]+).*" "\\1"
                               _open62541_${_ver_part} "${_open62541_${_ver_part}}")
                    endif()
                endforeach()
                if(_open62541_MAJOR AND _open62541_MINOR AND _open62541_PATCH)
                    set(ZEPTO_OPCUA_VERSION "${_open62541_MAJOR}.${_open62541_MINOR}.${_open62541_PATCH}")
                endif()
            endif()
        endif()
    endif()

    if(ZEPTO_OPCUA_TARGET)
        if(NOT ZEPTO_OPCUA_INCLUDE_DIRS)
            get_target_property(_open62541_target_includes
                ${ZEPTO_OPCUA_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
            if(_open62541_target_includes)
                set(ZEPTO_OPCUA_INCLUDE_DIRS ${_open62541_target_includes})
            endif()
        endif()

        include(CheckCXXSourceCompiles)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        if(ZEPTO_OPCUA_INCLUDE_DIRS)
            set(CMAKE_REQUIRED_INCLUDES ${ZEPTO_OPCUA_INCLUDE_DIRS})
        endif()
        set(CMAKE_REQUIRED_LIBRARIES ${ZEPTO_OPCUA_TARGET})
        set(CMAKE_REQUIRED_QUIET ON)
        check_cxx_source_compiles("
            #include <open62541/client.h>
            #ifndef UA_ENABLE_HISTORIZING
            #error UA_ENABLE_HISTORIZING missing
            #endif
            int main() { return 0; }
        " ZEPTO_OPCUA_HAS_HISTORIZING)
        cmake_pop_check_state()

        set(ZEPTO_HAS_OPCUA ON)
        set(_open62541_label "open62541")
        if(ZEPTO_OPCUA_VERSION)
            set(_open62541_label "${_open62541_label} ${ZEPTO_OPCUA_VERSION}")
        endif()
        message(STATUS "${_open62541_label}: found via ${ZEPTO_OPCUA_DETECTION_METHOD} (OPC-UA client/server enabled)")
        if(ZEPTO_OPCUA_HAS_HISTORIZING)
            message(STATUS "open62541: UA_ENABLE_HISTORIZING enabled (live Historical Access enabled)")
        else()
            message(STATUS "open62541: UA_ENABLE_HISTORIZING not detected — live Historical Access disabled")
            message(STATUS "  Rebuild open62541 with historizing support for OpcUaConsumer::read_history()")
        endif()
    else()
        message(STATUS "open62541: not found — OPC-UA client/server disabled")
        message(STATUS "  Install libopen62541-dev/open62541-devel or provide open62541Config.cmake/pkg-config metadata")
        set(ZEPTO_USE_OPCUA OFF)
        set(ZEPTO_HAS_OPCUA OFF)
    endif()
else()
    set(ZEPTO_HAS_OPCUA OFF)
    set(ZEPTO_OPCUA_HAS_HISTORIZING OFF)
endif()

# --- ROS 2 / rclcpp + standard message profiles (Physical AI bridge, optional) ---
if(ZEPTO_USE_ROS2)
    find_package(rclcpp QUIET)
    find_package(std_msgs QUIET)
    find_package(sensor_msgs QUIET)
    find_package(nav_msgs QUIET)
    find_package(tf2_msgs QUIET)
    find_package(geometry_msgs QUIET)
    if(rclcpp_FOUND AND std_msgs_FOUND)
        message(STATUS "rclcpp/std_msgs: found (ROS 2 scalar subscriber enabled)")
        set(ZEPTO_HAS_ROS2 ON)
        if(sensor_msgs_FOUND AND nav_msgs_FOUND AND tf2_msgs_FOUND AND geometry_msgs_FOUND)
            message(STATUS "ROS 2 standard messages: found (IMU/JointState/Odometry/TF/LaserScan profiles enabled)")
            set(ZEPTO_HAS_ROS2_STANDARD ON)
        else()
            message(STATUS "ROS 2 standard messages: not fully found — standard profiles disabled")
            set(ZEPTO_HAS_ROS2_STANDARD OFF)
        endif()
        find_package(rosbag2_cpp QUIET)
        find_package(rosbag2_storage QUIET)
        if(rosbag2_cpp_FOUND AND rosbag2_storage_FOUND)
            message(STATUS "rosbag2_cpp/storage: found (ROS 2 bag import/replay enabled)")
            set(ZEPTO_HAS_ROS2_BAG ON)
        else()
            message(STATUS "rosbag2_cpp/storage: not found — ROS 2 bag import/replay disabled")
            set(ZEPTO_HAS_ROS2_BAG OFF)
        endif()
    else()
        message(STATUS "rclcpp/std_msgs: not found — ROS 2 live bridge disabled")
        message(STATUS "  Install a ROS 2 distro and source its setup.bash before configuring")
        set(ZEPTO_USE_ROS2 OFF)
        set(ZEPTO_HAS_ROS2 OFF)
        set(ZEPTO_HAS_ROS2_STANDARD OFF)
        set(ZEPTO_HAS_ROS2_BAG OFF)
    endif()
else()
    set(ZEPTO_HAS_ROS2 OFF)
    set(ZEPTO_HAS_ROS2_STANDARD OFF)
    set(ZEPTO_HAS_ROS2_BAG OFF)
endif()

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

# DuckDB amalgamation for embedded analytical engine (optional)
if(ZEPTO_ENABLE_DUCKDB)
    FetchContent_Declare(
        duckdb
        GIT_REPOSITORY https://github.com/duckdb/duckdb.git
        GIT_TAG        v1.2.1
        GIT_SHALLOW    TRUE
    )
endif()

if(ZEPTO_ENABLE_HNSWLIB)
    FetchContent_Declare(
        hnswlib
        GIT_REPOSITORY https://github.com/nmslib/hnswlib.git
        GIT_TAG        v0.8.0
        GIT_SHALLOW    TRUE
    )
endif()

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt spdlog)

if(ZEPTO_BUILD_TESTS)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

if(ZEPTO_ENABLE_HNSWLIB)
    FetchContent_MakeAvailable(hnswlib)
endif()

# --- NUMA (Linux only) ---
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(NUMA_LIB numa REQUIRED)
else()
    set(NUMA_LIB "")
endif()

# --- Apache Arrow + Parquet (optional) ---
find_package(PkgConfig QUIET)
if(ZEPTO_USE_PARQUET)
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
            # Fallback: pyarrow 번들 C++ 라이브러리
            execute_process(
                COMMAND python3 -c "import pyarrow; print(pyarrow.get_library_dirs()[0])"
                OUTPUT_VARIABLE PYARROW_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE PYARROW_RESULT ERROR_QUIET)
            execute_process(
                COMMAND python3 -c "import pyarrow; print(pyarrow.get_include())"
                OUTPUT_VARIABLE PYARROW_INC_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            find_library(PYARROW_ARROW_LIB NAMES arrow PATHS "${PYARROW_LIB_DIR}" NO_DEFAULT_PATH)
            find_library(PYARROW_PARQUET_LIB NAMES parquet PATHS "${PYARROW_LIB_DIR}" NO_DEFAULT_PATH)
            if(PYARROW_ARROW_LIB AND PYARROW_PARQUET_LIB)
                message(STATUS "Arrow/Parquet: pyarrow 번들 사용 (${PYARROW_LIB_DIR})")
                set(ZEPTO_PARQUET_VIA_PYARROW ON)
                add_library(PyArrow::arrow SHARED IMPORTED)
                set_target_properties(PyArrow::arrow PROPERTIES
                    IMPORTED_LOCATION "${PYARROW_ARROW_LIB}"
                    INTERFACE_INCLUDE_DIRECTORIES "${PYARROW_INC_DIR}")
                add_library(PyArrow::parquet SHARED IMPORTED)
                set_target_properties(PyArrow::parquet PROPERTIES
                    IMPORTED_LOCATION "${PYARROW_PARQUET_LIB}"
                    INTERFACE_INCLUDE_DIRECTORIES "${PYARROW_INC_DIR}")
            else()
                message(STATUS "Arrow/Parquet: 없음 — Parquet 저장 비활성화")
                message(STATUS "  설치: pip3 install pyarrow")
                set(ZEPTO_USE_PARQUET OFF)
            endif()
        endif()
    endif()
endif()

# --- Arrow Flight (optional, requires Arrow + gRPC) ---
set(ZEPTO_HAS_FLIGHT OFF)
if(ZEPTO_USE_FLIGHT)
    # Try system-installed ArrowFlight first
    find_package(ArrowFlight CONFIG QUIET)
    if(ArrowFlight_FOUND)
        message(STATUS "ArrowFlight: ${ArrowFlight_VERSION} (system)")
        set(ZEPTO_HAS_FLIGHT ON)
    else()
        pkg_check_modules(ARROW_FLIGHT IMPORTED_TARGET arrow-flight QUIET)
        if(ARROW_FLIGHT_FOUND)
            message(STATUS "ArrowFlight (pkg-config): ${ARROW_FLIGHT_VERSION}")
            set(ZEPTO_HAS_FLIGHT ON)
        else()
        # Fallback: pyarrow bundled libarrow_flight
        execute_process(
            COMMAND python3 -c "import pyarrow; print(pyarrow.get_library_dirs()[0])"
            OUTPUT_VARIABLE _PYARROW_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _PYARROW_RES ERROR_QUIET)
        execute_process(
            COMMAND python3 -c "import pyarrow; print(pyarrow.get_include())"
            OUTPUT_VARIABLE _PYARROW_INC_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        # pyarrow ships libarrow_flight.so.XXXX (no bare .so symlink)
        file(GLOB _FLIGHT_SO "${_PYARROW_LIB_DIR}/libarrow_flight.so*")
        if(_FLIGHT_SO)
            list(GET _FLIGHT_SO 0 _FLIGHT_SO_PATH)
            message(STATUS "ArrowFlight: pyarrow bundle (${_FLIGHT_SO_PATH})")
            add_library(PyArrow::arrow_flight SHARED IMPORTED)
            set_target_properties(PyArrow::arrow_flight PROPERTIES
                IMPORTED_LOCATION "${_FLIGHT_SO_PATH}"
                IMPORTED_NO_SONAME TRUE
                INTERFACE_INCLUDE_DIRECTORIES "${_PYARROW_INC_DIR}")
            set(ZEPTO_HAS_FLIGHT ON)
            set(ZEPTO_FLIGHT_VIA_PYARROW ON)
        else()
            message(STATUS "ArrowFlight: not found — Flight server disabled")
            message(STATUS "  Install: pip3 install pyarrow")
            set(ZEPTO_USE_FLIGHT OFF)
        endif()
        endif()
    endif()
endif()

# --- AWS SDK C++ S3 (optional) ---
if(ZEPTO_USE_S3)
    find_package(AWSSDK CONFIG COMPONENTS s3 QUIET)
    if(AWSSDK_FOUND)
        message(STATUS "AWS SDK C++: ${AWSSDK_VERSION} (S3 업로드 활성화)")
        set(ZEPTO_S3_AWSSDK_LIBS ${AWSSDK_LINK_LIBRARIES})
    else()
        message(STATUS "AWS SDK C++: 없음 — S3 업로드 비활성화")
        message(STATUS "  설치: sudo dnf install -y aws-sdk-cpp-s3")
        set(ZEPTO_USE_S3 OFF)
    endif()
endif()

# --- AWS SDK C++ Kinesis (optional) ---
if(ZEPTO_USE_KINESIS)
    find_file(ZEPTO_AWS_KINESIS_CONFIG
        NAMES aws-cpp-sdk-kinesisConfig.cmake aws-cpp-sdk-kinesis-config.cmake
        PATH_SUFFIXES
            lib/cmake/aws-cpp-sdk-kinesis
            lib64/cmake/aws-cpp-sdk-kinesis
    )
    if(ZEPTO_AWS_KINESIS_CONFIG)
        find_package(AWSSDK CONFIG COMPONENTS kinesis QUIET)
    endif()
    if(AWSSDK_FOUND AND ZEPTO_AWS_KINESIS_CONFIG)
        message(STATUS "AWS SDK C++: ${AWSSDK_VERSION} (Kinesis consumer enabled)")
        set(ZEPTO_HAS_KINESIS ON)
        set(ZEPTO_KINESIS_AWSSDK_LIBS ${AWSSDK_LINK_LIBRARIES})
    else()
        message(STATUS "AWS SDK C++: not found — Kinesis consumer disabled")
        message(STATUS "  Install: sudo dnf install -y aws-sdk-cpp-kinesis")
        set(ZEPTO_USE_KINESIS OFF)
        set(ZEPTO_HAS_KINESIS OFF)
    endif()
else()
    set(ZEPTO_HAS_KINESIS OFF)
endif()

# --- OpenSSL (TLS + crypto for auth) ---
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND OR OPENSSL_FOUND)
    message(STATUS "OpenSSL: ${OPENSSL_VERSION} (TLS + JWT/auth enabled)")
    set(ZEPTO_HAS_OPENSSL ON)
else()
    message(STATUS "OpenSSL: not found — TLS and JWT auth disabled")
    set(ZEPTO_HAS_OPENSSL OFF)
endif()

# --- DuckDB embedded engine (optional) ---
if(ZEPTO_ENABLE_DUCKDB)
    set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(duckdb)
    message(STATUS "DuckDB: embedded engine enabled")
endif()

# --- LZ4 (optional) ---
if(ZEPTO_USE_LZ4)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LZ4 IMPORTED_TARGET liblz4)
    if(LZ4_FOUND)
        message(STATUS "LZ4: ${LZ4_VERSION} (압축 활성화)")
    else()
        message(STATUS "LZ4: 없음 — 압축 비활성화 (패스스루 모드)")
        set(ZEPTO_USE_LZ4 OFF)
    endif()
endif()

# --- Highway (SIMD) ---
if(ZEPTO_USE_HIGHWAY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HWY REQUIRED IMPORTED_TARGET libhwy)
    message(STATUS "Highway SIMD: ${HWY_VERSION}")
endif()

# --- UCX (RDMA/CXL communication, Linux only) ---
if(ZEPTO_USE_UCX)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(STATUS "UCX: disabled on ${CMAKE_SYSTEM_NAME} (Linux only)")
        set(ZEPTO_USE_UCX OFF)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx)
        message(STATUS "UCX: ${UCX_VERSION}")
    endif()
endif()

# --- LLVM (JIT) ---
if(ZEPTO_USE_JIT)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(LLVM REQUIRED CONFIG
            PATHS
                /usr/lib64/llvm19/lib/cmake/llvm          # Amazon Linux / RHEL
                /usr/lib/llvm-19/lib/cmake/llvm            # Ubuntu amd64
                /usr/lib/aarch64-linux-gnu/cmake/llvm-19   # Ubuntu arm64
            NO_DEFAULT_PATH
        )
    else()
        find_package(LLVM CONFIG QUIET)
        if(NOT LLVM_FOUND AND HOMEBREW_PREFIX)
            find_package(LLVM CONFIG QUIET
                PATHS "${HOMEBREW_PREFIX}/opt/llvm/lib/cmake/llvm"
                NO_DEFAULT_PATH)
        endif()
        if(NOT LLVM_FOUND)
            message(STATUS "LLVM: not found — JIT disabled on ${CMAKE_SYSTEM_NAME}")
            set(ZEPTO_USE_JIT OFF)
        endif()
    endif()
    if(ZEPTO_USE_JIT)
        message(STATUS "LLVM ${LLVM_VERSION}: ${LLVM_DIR}")
        include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
        separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
        add_definitions(${LLVM_DEFINITIONS_LIST})
    endif()
endif()

# ============================================================================
# ZeptoDB Libraries
# ============================================================================

# --- Common utilities ---
add_library(zepto_common STATIC
    src/common/logger.cpp
    src/util/logger.cpp
)
target_include_directories(zepto_common PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(zepto_common PUBLIC
    ZEPTO_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(zepto_common PUBLIC
    spdlog::spdlog
    fmt::fmt
)

# --- Layer 1: Storage Engine ---
add_library(zepto_storage STATIC
    src/storage/arena_allocator.cpp
    src/storage/column_store.cpp
    src/storage/partition_manager.cpp
    src/storage/hdb_writer.cpp
    src/storage/hdb_reader.cpp
    src/storage/flush_manager.cpp
    src/storage/parquet_writer.cpp
    src/storage/parquet_reader.cpp
    src/storage/s3_sink.cpp
    src/storage/materialized_view.cpp
)
target_include_directories(zepto_storage PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_storage PUBLIC
    zepto_common
)
if(NUMA_LIB)
    target_link_libraries(zepto_storage PUBLIC ${NUMA_LIB})
endif()
if(ZEPTO_USE_HIGHWAY)
    target_link_libraries(zepto_storage PUBLIC PkgConfig::HWY)
endif()
if(ZEPTO_USE_LZ4)
    target_link_libraries(zepto_storage PUBLIC PkgConfig::LZ4)
    target_compile_definitions(zepto_storage PUBLIC ZEPTO_HDB_FORCE_LZ4=1)
endif()
if(ZEPTO_USE_PARQUET)
    if(Arrow_FOUND AND Parquet_FOUND)
        target_link_libraries(zepto_storage PUBLIC Arrow::arrow_shared Parquet::parquet_shared)
    elseif(ARROW_FOUND AND PARQUET_FOUND)
        target_link_libraries(zepto_storage PUBLIC PkgConfig::ARROW PkgConfig::PARQUET)
    elseif(ZEPTO_PARQUET_VIA_PYARROW)
        target_link_libraries(zepto_storage PUBLIC PyArrow::arrow PyArrow::parquet)
    endif()
    target_compile_definitions(zepto_storage PUBLIC ZEPTO_PARQUET_ENABLED=1)
endif()
if(ZEPTO_USE_S3)
    target_link_libraries(zepto_storage PUBLIC ${ZEPTO_S3_AWSSDK_LIBS})
    target_compile_definitions(zepto_storage PUBLIC ZEPTO_S3_ENABLED=1)
endif()

# --- Layer 2: Ingestion (Tick Plant) ---
add_library(zepto_ingestion STATIC
    src/ingestion/ring_buffer.cpp
    src/ingestion/tick_plant.cpp
    src/ingestion/wal_writer.cpp
)
target_include_directories(zepto_ingestion PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_ingestion PUBLIC
    zepto_common
    zepto_storage
)
if(ZEPTO_USE_UCX)
    target_link_libraries(zepto_ingestion PUBLIC PkgConfig::UCX)
endif()

# --- Layer 3: Execution Engine ---
add_library(zepto_execution STATIC
    src/execution/vectorized_engine.cpp
    src/execution/query_planner.cpp
    src/execution/join_operator.cpp
    src/execution/worker_pool.cpp
    src/execution/parallel_scan.cpp
    src/execution/local_scheduler.cpp
    src/execution/distributed_scheduler.cpp
    src/execution/resource_isolation.cpp
    src/execution/table_statistics.cpp
    src/execution/cost_model.cpp
)
if(ZEPTO_USE_JIT)
    target_sources(zepto_execution PRIVATE src/execution/jit_engine.cpp)
endif()
if(ZEPTO_ENABLE_DUCKDB)
    target_sources(zepto_execution PRIVATE
        src/execution/duckdb_engine.cpp
        src/execution/arrow_bridge.cpp
    )
endif()
target_include_directories(zepto_execution PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src   # Highway foreach_target.h needs src/ in path
)
find_package(Threads REQUIRED)
target_link_libraries(zepto_execution PUBLIC
    zepto_common
    zepto_storage
    Threads::Threads
)
if(ZEPTO_USE_HIGHWAY)
    target_link_libraries(zepto_execution PUBLIC PkgConfig::HWY)
endif()
if(ZEPTO_ENABLE_DUCKDB)
    target_link_libraries(zepto_execution PUBLIC duckdb)
    target_compile_definitions(zepto_execution PUBLIC ZEPTO_ENABLE_DUCKDB=1)
endif()
if(ZEPTO_USE_JIT)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # LLVM shared lib (auto-detect path) + GCC libstdc++
        find_library(LLVM_SHARED_LIB NAMES LLVM-19 LLVM
            PATHS ${LLVM_LIBRARY_DIRS} /usr/lib64/llvm19/lib /usr/lib/llvm-19/lib
            NO_DEFAULT_PATH
        )
        if(NOT LLVM_SHARED_LIB)
            find_library(LLVM_SHARED_LIB NAMES LLVM-19 LLVM)
        endif()
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.so
            OUTPUT_VARIABLE STDCXX_PATH OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        target_link_libraries(zepto_execution PUBLIC
            ${LLVM_SHARED_LIB}
            ${STDCXX_PATH}
        )
    else()
        llvm_map_components_to_libnames(LLVM_LIBS core orcjit native)
        target_link_libraries(zepto_execution PUBLIC ${LLVM_LIBS})
    endif()
endif()

# --- Core Pipeline (Layer 1+2+3 통합) ---
add_library(zepto_core STATIC
    src/core/pipeline.cpp
)
target_include_directories(zepto_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_core PUBLIC
    zepto_common
    zepto_storage
    zepto_ingestion
    zepto_execution
)

# --- Layer 5: SQL Parser + Executor ---
add_library(zepto_sql STATIC
    src/sql/tokenizer.cpp
    src/sql/parser.cpp
    src/sql/executor.cpp
    src/execution/join_operator.cpp
)
target_include_directories(zepto_sql PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_sql PUBLIC
    zepto_core
)

# --- AI Agent Memory Layer ---
add_library(zepto_ai STATIC
    src/ai/ann_index.cpp
    src/ai/agent_memory.cpp
    src/ai/agent_memory_router.cpp
    src/ai/agent_memory_wire.cpp
)
target_include_directories(zepto_ai PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_ai PUBLIC
    zepto_common
)
if(ZEPTO_ENABLE_HNSWLIB)
    target_include_directories(zepto_ai SYSTEM PUBLIC ${hnswlib_SOURCE_DIR})
    target_compile_definitions(zepto_ai PUBLIC ZEPTO_ENABLE_HNSWLIB=1)
endif()

# --- Auth Library (RBAC + API Key + JWT + AuthManager + Rate Limiting + Secrets) ---
add_library(zepto_auth STATIC
    src/auth/api_key_store.cpp
    src/auth/vault_key_backend.cpp
    src/auth/jwt_validator.cpp
    src/auth/jwks_provider.cpp
    src/auth/auth_manager.cpp
    src/auth/sso_identity_provider.cpp
    src/auth/rate_limiter.cpp
    src/auth/secrets_provider.cpp
    src/auth/query_tracker.cpp
    src/auth/tenant_manager.cpp
    src/auth/oidc_discovery.cpp
    src/auth/session_store.cpp
    src/auth/oauth2_token_exchange.cpp
    src/auth/license_validator.cpp
)
target_include_directories(zepto_auth PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}   # third_party/httplib.h (used by secrets_provider)
)
target_link_libraries(zepto_auth PUBLIC
    zepto_common
    spdlog::spdlog
)
if(ZEPTO_HAS_OPENSSL)
    target_link_libraries(zepto_auth PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(zepto_auth PUBLIC ZEPTO_AUTH_OPENSSL=1)
endif()

# --- Layer 6: HTTP API Server ---

# Shared Arrow IPC encoder (used by both HTTP `POST /` content negotiation
# and the Arrow Flight server's `DoGet` / `GetFlightInfo`). Always built;
# when ZEPTO_HAS_FLIGHT is OFF the TU compiles to a stub that reports
# `arrow_ipc_available() == false`. devlog 119.
add_library(zepto_arrow_ipc STATIC
    src/server/arrow_ipc.cpp
)
target_include_directories(zepto_arrow_ipc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_arrow_ipc PUBLIC
    zepto_sql
    zepto_common
)
if(ZEPTO_HAS_FLIGHT)
    if(ArrowFlight_FOUND)
        target_link_libraries(zepto_arrow_ipc PUBLIC ArrowFlight::arrow_flight_shared)
    elseif(ARROW_FLIGHT_FOUND)
        target_link_libraries(zepto_arrow_ipc PUBLIC PkgConfig::ARROW_FLIGHT)
    elseif(ZEPTO_FLIGHT_VIA_PYARROW)
        if(TARGET PyArrow::arrow)
            target_link_libraries(zepto_arrow_ipc PUBLIC PyArrow::arrow)
        elseif(Arrow_FOUND)
            target_link_libraries(zepto_arrow_ipc PUBLIC Arrow::arrow_shared)
        elseif(ARROW_FOUND)
            target_link_libraries(zepto_arrow_ipc PUBLIC PkgConfig::ARROW)
        endif()
    endif()
    target_compile_definitions(zepto_arrow_ipc PUBLIC ZEPTO_FLIGHT_ENABLED=1)
endif()

add_library(zepto_server STATIC
    src/server/action_outcome_sql_adapter.cpp
    src/server/edge_fleet_sql_http_adapter.cpp
    src/server/http_server.cpp
    src/server/msgpack_ingest.cpp
)
target_include_directories(zepto_server PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}  # third_party/httplib.h access
)
target_link_libraries(zepto_server PUBLIC
    zepto_sql
    zepto_ai
    zepto_auth
    zepto_cluster
    zepto_arrow_ipc
)
# httplib requires POSIX threads
find_package(Threads REQUIRED)
target_link_libraries(zepto_server PUBLIC Threads::Threads)
if(ZEPTO_HAS_OPENSSL)
    target_link_libraries(zepto_server PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(zepto_server PUBLIC ZEPTO_TLS_ENABLED=1)
endif()
if(ZEPTO_USE_TCMALLOC AND TCMALLOC_LIB)
    target_link_libraries(zepto_server PUBLIC ${TCMALLOC_LIB})
endif()

# --- Phase C: Cluster (분산 메모리 & 트랜스포트) ---

# --- Arrow Flight Server (optional) ---
add_library(zepto_flight STATIC
    src/server/flight_server.cpp
)
target_include_directories(zepto_flight PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_flight PUBLIC
    zepto_sql
    zepto_common
    zepto_auth
    zepto_arrow_ipc
)
if(ZEPTO_HAS_FLIGHT)
    if(ArrowFlight_FOUND)
        target_link_libraries(zepto_flight PUBLIC ArrowFlight::arrow_flight_shared)
    elseif(ARROW_FLIGHT_FOUND)
        target_link_libraries(zepto_flight PUBLIC PkgConfig::ARROW_FLIGHT)
    elseif(ZEPTO_FLIGHT_VIA_PYARROW)
        target_link_libraries(zepto_flight PUBLIC PyArrow::arrow_flight)
        # Also need libarrow.so for Arrow core types
        if(TARGET PyArrow::arrow)
            target_link_libraries(zepto_flight PUBLIC PyArrow::arrow)
        elseif(Arrow_FOUND)
            target_link_libraries(zepto_flight PUBLIC Arrow::arrow_shared)
        elseif(ARROW_FOUND)
            target_link_libraries(zepto_flight PUBLIC PkgConfig::ARROW)
        endif()
    endif()
    target_compile_definitions(zepto_flight PUBLIC ZEPTO_FLIGHT_ENABLED=1)
endif()
add_library(zepto_cluster STATIC
    src/cluster/shm_backend.cpp
    src/cluster/ucx_backend.cpp
    src/cluster/rpc_security.cpp
    src/cluster/tcp_rpc.cpp
    src/cluster/query_coordinator.cpp
    src/cluster/wal_replicator.cpp
    src/cluster/partition_migrator.cpp
    src/cluster/rebalance_manager.cpp
    src/cluster/failover_manager.cpp
    src/cluster/coordinator_ha.cpp
    src/cluster/snapshot_coordinator.cpp
    src/cluster/compute_node.cpp
    src/cluster/ptp_clock_detector.cpp
)
target_include_directories(zepto_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cluster   # shm_backend.h, ucx_backend.h (internal)
)
target_link_libraries(zepto_cluster PUBLIC
    zepto_core
    zepto_sql
)
if(ZEPTO_HAS_OPENSSL)
    target_link_libraries(zepto_cluster PUBLIC OpenSSL::Crypto)
    target_compile_definitions(zepto_cluster PRIVATE ZEPTO_RPC_OPENSSL=1)
endif()
# UCX 발견 시 ZEPTO_HAS_UCX 정의 + 링크
if(ZEPTO_USE_UCX AND UCX_FOUND)
    target_compile_definitions(zepto_cluster PUBLIC ZEPTO_HAS_UCX=1)
    target_link_libraries(zepto_cluster PUBLIC PkgConfig::UCX)
endif()

# --- Migration Toolkit (kdb+, ClickHouse, DuckDB, TimescaleDB) ---
add_library(zepto_migration STATIC
    src/migration/q_lexer.cpp
    src/migration/q_parser.cpp
    src/migration/q_to_sql.cpp
    src/migration/q_to_python.cpp
    src/migration/hdb_loader.cpp
    src/migration/clickhouse_migrator.cpp
    src/migration/duckdb_interop.cpp
    src/migration/timescaledb_migrator.cpp
)
target_include_directories(zepto_migration PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_migration PUBLIC
    zepto_common
    zepto_auth
)

# --- Scheduler (.z.ts equivalent) ---
add_library(zepto_scheduler STATIC
    src/scheduler/scheduler.cpp
)
target_include_directories(zepto_scheduler PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_scheduler PUBLIC
    Threads::Threads
)

# --- Feed Handler Adapters ---
add_library(zepto_feeds STATIC
    src/feeds/action_outcome_supervisor_runtime.cpp
    src/feeds/edge_fleet_connector_runtime.cpp
    src/feeds/edge_fleet_feed_connector.cpp
    src/feeds/fix_parser.cpp
    src/feeds/nasdaq_itch.cpp
    src/feeds/binance_feed.cpp
)
target_include_directories(zepto_feeds PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_feeds PUBLIC
    zepto_common
)

# --- Kafka Consumer (depends on zepto_core + zepto_cluster for routing) ---
add_library(zepto_kafka STATIC
    src/feeds/kafka_consumer.cpp
)
target_include_directories(zepto_kafka PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_kafka PUBLIC
    zepto_core
    zepto_cluster
)
if(ZEPTO_HAS_KAFKA)
    target_compile_definitions(zepto_kafka PUBLIC ZEPTO_KAFKA_AVAILABLE=1)
    target_link_libraries(zepto_kafka PUBLIC ${RDKAFKA_CPP_LIB} ${RDKAFKA_LIB})
    message(STATUS "zepto_kafka: Kafka polling enabled")
else()
    message(STATUS "zepto_kafka: decode/routing only (no librdkafka)")
endif()

# --- MQTT Consumer (depends on zepto_core + zepto_cluster + zepto_kafka for shared decoders) ---
add_library(zepto_mqtt STATIC
    src/feeds/mqtt_consumer.cpp
)
target_include_directories(zepto_mqtt PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_mqtt PUBLIC
    zepto_core
    zepto_cluster
    zepto_kafka
)
if(ZEPTO_HAS_MQTT)
    target_compile_definitions(zepto_mqtt PUBLIC ZEPTO_MQTT_AVAILABLE=1)
    target_link_libraries(zepto_mqtt PUBLIC ${PAHO_MQTTPP_LIB} ${PAHO_MQTT_LIB})
    message(STATUS "zepto_mqtt: MQTT broker connectivity enabled")
else()
    message(STATUS "zepto_mqtt: decode/routing only (no paho-mqttpp3)")
endif()

# --- AWS Kinesis Consumer (depends on zepto_core + zepto_cluster + zepto_kafka for shared decoders) ---
add_library(zepto_kinesis STATIC
    src/feeds/kinesis_consumer.cpp
)
target_include_directories(zepto_kinesis PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_kinesis PUBLIC
    zepto_core
    zepto_cluster
    zepto_kafka
)
if(ZEPTO_HAS_KINESIS)
    target_compile_definitions(zepto_kinesis PUBLIC ZEPTO_KINESIS_AVAILABLE=1)
    target_link_libraries(zepto_kinesis PUBLIC ${ZEPTO_KINESIS_AWSSDK_LIBS})
    message(STATUS "zepto_kinesis: AWS polling enabled")
else()
    message(STATUS "zepto_kinesis: decode/routing only (no AWS SDK Kinesis)")
endif()

# --- Apache Pulsar Consumer (depends on zepto_core + zepto_cluster + zepto_kafka for shared decoders) ---
add_library(zepto_pulsar STATIC
    src/feeds/pulsar_consumer.cpp
)
target_include_directories(zepto_pulsar PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_pulsar PUBLIC
    zepto_core
    zepto_cluster
    zepto_kafka
)
if(ZEPTO_HAS_PULSAR)
    target_compile_definitions(zepto_pulsar PUBLIC ZEPTO_PULSAR_AVAILABLE=1)
    target_include_directories(zepto_pulsar PUBLIC ${PULSAR_CPP_INCLUDE_DIR})
    target_link_libraries(zepto_pulsar PUBLIC ${PULSAR_CPP_LIB})
    message(STATUS "zepto_pulsar: Pulsar broker connectivity enabled")
else()
    message(STATUS "zepto_pulsar: decode/routing only (no pulsar-client-cpp)")
endif()

# --- Telegraf external output (outputs.execd line protocol writer) ---
add_library(zepto_telegraf_output STATIC
    src/feeds/telegraf_output.cpp
)
target_include_directories(zepto_telegraf_output PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# --- OPC-UA Consumer (depends on zepto_core + zepto_cluster) ---
add_library(zepto_opcua STATIC
    src/feeds/opcua_consumer.cpp
)
target_include_directories(zepto_opcua PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_opcua PUBLIC
    zepto_core
    zepto_cluster
)
if(ZEPTO_HAS_OPCUA)
    target_compile_definitions(zepto_opcua PUBLIC ZEPTO_OPCUA_AVAILABLE=1)
    if(ZEPTO_OPCUA_HAS_HISTORIZING)
        target_compile_definitions(zepto_opcua PUBLIC ZEPTO_OPCUA_HISTORIZING_AVAILABLE=1)
    endif()
    target_link_libraries(zepto_opcua PUBLIC ${ZEPTO_OPCUA_TARGET})
    message(STATUS "zepto_opcua: OPC-UA client/server connectivity enabled")
else()
    message(STATUS "zepto_opcua: NodeId-map / routing only (no open62541)")
endif()

# --- ROS 2 Consumer (depends on zepto_core + zepto_cluster) ---
add_library(zepto_ros2 STATIC
    src/feeds/ros2_consumer.cpp
)
target_include_directories(zepto_ros2 PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto_ros2 PUBLIC
    zepto_core
    zepto_cluster
)
if(ZEPTO_HAS_ROS2)
    target_compile_definitions(zepto_ros2 PUBLIC ZEPTO_ROS2_AVAILABLE=1)
    set(ZEPTO_ROS2_TARGETS)
    foreach(_ros2_target
            rclcpp::rclcpp
            std_msgs::std_msgs__rosidl_typesupport_cpp
            std_msgs::std_msgs__rosidl_generator_cpp)
        if(TARGET ${_ros2_target})
            list(APPEND ZEPTO_ROS2_TARGETS ${_ros2_target})
        endif()
    endforeach()
    if(ZEPTO_ROS2_TARGETS)
        target_link_libraries(zepto_ros2 PUBLIC ${ZEPTO_ROS2_TARGETS})
    endif()
    if(ZEPTO_HAS_ROS2_STANDARD)
        target_compile_definitions(zepto_ros2 PUBLIC ZEPTO_ROS2_STANDARD_AVAILABLE=1)
        foreach(_ros2_standard_target
                sensor_msgs::sensor_msgs__rosidl_typesupport_cpp
                sensor_msgs::sensor_msgs__rosidl_generator_cpp
                nav_msgs::nav_msgs__rosidl_typesupport_cpp
                nav_msgs::nav_msgs__rosidl_generator_cpp
                tf2_msgs::tf2_msgs__rosidl_typesupport_cpp
                tf2_msgs::tf2_msgs__rosidl_generator_cpp
                geometry_msgs::geometry_msgs__rosidl_typesupport_cpp
                geometry_msgs::geometry_msgs__rosidl_generator_cpp)
            if(TARGET ${_ros2_standard_target})
                target_link_libraries(zepto_ros2 PUBLIC ${_ros2_standard_target})
            endif()
        endforeach()
    endif()
    if(ZEPTO_HAS_ROS2_BAG)
        target_compile_definitions(zepto_ros2 PUBLIC ZEPTO_ROS2_BAG_AVAILABLE=1)
        set(ZEPTO_ROS2_BAG_TARGETS)
        foreach(_ros2_bag_target
                rosbag2_cpp::rosbag2_cpp
                rosbag2_storage::rosbag2_storage)
            if(TARGET ${_ros2_bag_target})
                list(APPEND ZEPTO_ROS2_BAG_TARGETS ${_ros2_bag_target})
            endif()
        endforeach()
        if(ZEPTO_ROS2_BAG_TARGETS)
            target_link_libraries(zepto_ros2 PUBLIC ${ZEPTO_ROS2_BAG_TARGETS})
        endif()
        if(DEFINED rosbag2_cpp_INCLUDE_DIRS)
            target_include_directories(zepto_ros2 PUBLIC ${rosbag2_cpp_INCLUDE_DIRS})
        endif()
        if(DEFINED rosbag2_storage_INCLUDE_DIRS)
            target_include_directories(zepto_ros2 PUBLIC ${rosbag2_storage_INCLUDE_DIRS})
        endif()
        if(DEFINED rosbag2_cpp_LIBRARIES)
            target_link_libraries(zepto_ros2 PUBLIC ${rosbag2_cpp_LIBRARIES})
        endif()
        if(DEFINED rosbag2_storage_LIBRARIES)
            target_link_libraries(zepto_ros2 PUBLIC ${rosbag2_storage_LIBRARIES})
        endif()
    endif()
    if(DEFINED rclcpp_DIR)
        get_filename_component(ZEPTO_ROS2_PREFIX "${rclcpp_DIR}/../../.." ABSOLUTE)
        if(EXISTS "${ZEPTO_ROS2_PREFIX}/lib")
            target_link_directories(zepto_ros2 PUBLIC "${ZEPTO_ROS2_PREFIX}/lib")
        endif()
    endif()
    if(DEFINED rclcpp_INCLUDE_DIRS)
        target_include_directories(zepto_ros2 PUBLIC ${rclcpp_INCLUDE_DIRS})
    endif()
    if(DEFINED std_msgs_INCLUDE_DIRS)
        target_include_directories(zepto_ros2 PUBLIC ${std_msgs_INCLUDE_DIRS})
    endif()
    if(DEFINED rclcpp_LIBRARIES)
        target_link_libraries(zepto_ros2 PUBLIC ${rclcpp_LIBRARIES})
    endif()
    if(DEFINED std_msgs_LIBRARIES)
        target_link_libraries(zepto_ros2 PUBLIC ${std_msgs_LIBRARIES})
    endif()
    foreach(_ros2_standard_pkg sensor_msgs nav_msgs tf2_msgs geometry_msgs)
        if(DEFINED ${_ros2_standard_pkg}_INCLUDE_DIRS)
            target_include_directories(zepto_ros2 PUBLIC ${${_ros2_standard_pkg}_INCLUDE_DIRS})
        endif()
        if(DEFINED ${_ros2_standard_pkg}_LIBRARIES)
            target_link_libraries(zepto_ros2 PUBLIC ${${_ros2_standard_pkg}_LIBRARIES})
        endif()
    endforeach()
    if(ZEPTO_HAS_ROS2_BAG)
        if(ZEPTO_HAS_ROS2_STANDARD)
            message(STATUS "zepto_ros2: ROS 2 scalar + standard profiles and rosbag2 import/replay enabled")
        else()
            message(STATUS "zepto_ros2: ROS 2 std_msgs scalar subscriber and rosbag2 import/replay enabled")
        endif()
    else()
        if(ZEPTO_HAS_ROS2_STANDARD)
            message(STATUS "zepto_ros2: ROS 2 scalar + standard profiles enabled")
        else()
            message(STATUS "zepto_ros2: ROS 2 std_msgs scalar subscriber enabled")
        endif()
    endif()
else()
    message(STATUS "zepto_ros2: config/mapping/routing only (no rclcpp)")
endif()

# ============================================================================
# Tools
# ============================================================================
function(zepto_link_system_openssl target)
    if(ZEPTO_HAS_OPENSSL)
        # Make OpenSSL a direct executable dependency. Pyarrow wheels export
        # private OpenSSL symbols from Arrow/Parquet DSOs; leaving OpenSSL only
        # as a transitive dependency can put those DSOs first in ELF lookup
        # order and interpose incompatible EVP/X509 implementations.
        target_link_libraries(${target} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
endfunction()

add_executable(zepto-migrate
    tools/zepto-migrate.cpp
)
target_include_directories(zepto-migrate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto-migrate PRIVATE
    zepto_migration
    zepto_common
)
target_compile_options(zepto-migrate PRIVATE -O2)

# OPC-UA address-space discovery CLI. In default builds it compiles as a
# diagnostic stub; with ZEPTO_USE_OPCUA=ON it connects through open62541.
add_executable(zepto-opcua-browse
    tools/zepto-opcua-browse.cpp
)
target_include_directories(zepto-opcua-browse PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto-opcua-browse PRIVATE
    zepto_opcua
)
zepto_link_system_openssl(zepto-opcua-browse)
target_compile_options(zepto-opcua-browse PRIVATE -O2)

# Telegraf external output writer. This is used from Telegraf's
# `outputs.execd` and sends parsed line protocol to ZeptoDB over HTTP SQL.
add_executable(zepto-telegraf-output
    tools/zepto-telegraf-output.cpp
)
target_include_directories(zepto-telegraf-output PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(zepto-telegraf-output PRIVATE
    zepto_telegraf_output
)
target_compile_options(zepto-telegraf-output PRIVATE -O2)

# Experimental Physical AI edge/fleet replay. It wires the C++ feed connector
# to two live HTTP SQL nodes and materializes fleet ACK/audit tables.
add_executable(zepto_edge_fleet_replay
    tools/zepto_edge_fleet_replay.cpp
)
target_include_directories(zepto_edge_fleet_replay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(zepto_edge_fleet_replay PRIVATE
    zepto_feeds
)
target_compile_options(zepto_edge_fleet_replay PRIVATE -O2)

# Interactive SQL CLI
add_executable(zepto-cli
    tools/zepto-cli.cpp
)
target_include_directories(zepto-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(zepto-cli PRIVATE
    zepto_common
)
# Link readline if available
find_library(READLINE_LIB readline)
if(READLINE_LIB)
    target_link_libraries(zepto-cli PRIVATE ${READLINE_LIB})
    target_compile_definitions(zepto-cli PRIVATE HAVE_READLINE=1)
endif()
target_compile_options(zepto-cli PRIVATE -O2)

# Standalone data node (for multi-process integration testing)
add_executable(zepto_data_node tools/zepto_data_node.cpp)

# HTTP server (Web UI + API)
add_executable(zepto_http_server tools/zepto_http_server.cpp)
target_include_directories(zepto_http_server PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zepto_http_server PRIVATE
    zepto_server zepto_sql zepto_execution zepto_core zepto_storage
    zepto_ingestion zepto_cluster zepto_feeds)
zepto_link_system_openssl(zepto_http_server)

# SQL-backed Action-Outcome supervisor soak/fault-injection harness.
add_executable(zepto_action_outcome_soak tools/zepto_action_outcome_soak.cpp)
target_include_directories(zepto_action_outcome_soak PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zepto_action_outcome_soak PRIVATE
    zepto_server zepto_sql zepto_execution zepto_core zepto_storage zepto_ingestion zepto_cluster zepto_feeds)
zepto_link_system_openssl(zepto_action_outcome_soak)
target_compile_options(zepto_action_outcome_soak PRIVATE -O2)

# Stateless ingest node (P8-I3, devlog 113) — HTTP ingest-only pod that
# forwards every tick to the right storage pod via CoordinatorRoutingAdapter.
add_executable(zepto_ingest_node tools/zepto_ingest_node.cpp)
target_include_directories(zepto_ingest_node PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zepto_ingest_node PRIVATE
    zepto_server zepto_sql zepto_execution zepto_core zepto_storage zepto_ingestion zepto_cluster zepto_feeds)
zepto_link_system_openssl(zepto_ingest_node)

# Arrow Flight + HTTP server
add_executable(zepto_flight_server tools/zepto_flight_server.cpp)
target_include_directories(zepto_flight_server PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zepto_flight_server PRIVATE
    zepto_flight zepto_server zepto_sql zepto_execution zepto_core
    zepto_storage zepto_ingestion zepto_cluster zepto_feeds)
zepto_link_system_openssl(zepto_flight_server)
target_include_directories(zepto_data_node PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zepto_data_node PRIVATE
    zepto_cluster zepto_sql zepto_execution zepto_core zepto_storage zepto_ingestion)
zepto_link_system_openssl(zepto_data_node)

# License key generator
add_executable(zepto-license-gen tools/zepto-license-gen.cpp)
target_include_directories(zepto-license-gen PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(ZEPTO_HAS_OPENSSL)
    zepto_link_system_openssl(zepto-license-gen)
    target_compile_definitions(zepto-license-gen PRIVATE ZEPTO_AUTH_OPENSSL=1)
endif()
target_compile_options(zepto-license-gen PRIVATE -O2)

# ============================================================================
# Tests
# ============================================================================
if(ZEPTO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ============================================================================
# Benchmarks
# ============================================================================
if(ZEPTO_BUILD_BENCH)
    add_executable(bench_pipeline
        tests/bench/bench_pipeline.cpp
    )
    target_link_libraries(bench_pipeline PRIVATE
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_pipeline PRIVATE
        -O3 -march=native
    )

    add_executable(bench_simd_jit
        tests/bench/bench_simd_jit.cpp
    )
    target_link_libraries(bench_simd_jit PRIVATE
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
        zepto_common
    )
    target_compile_options(bench_simd_jit PRIVATE
        -O3 -march=native
    )
    if(NOT ZEPTO_USE_JIT)
        set_target_properties(bench_simd_jit PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    add_executable(bench_hdb
        tests/bench/bench_hdb.cpp
    )
    target_link_libraries(bench_hdb PRIVATE
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_hdb PRIVATE
        -O3 -march=native
    )

    add_executable(bench_agent_memory
        tests/bench/bench_agent_memory.cpp
    )
    target_link_libraries(bench_agent_memory PRIVATE
        zepto_ai
        zepto_common
    )
    target_compile_options(bench_agent_memory PRIVATE
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
        zepto_cluster
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
        zepto_auth
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
        zepto_cluster
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_cxl PRIVATE
        -O3 -march=native
    )
    # SQL 벤치마크
    add_executable(bench_sql
        tests/bench/bench_sql.cpp
    )
    target_link_libraries(bench_sql PRIVATE
        zepto_sql
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_sql PRIVATE
        -O3 -march=native
    )

    # Cost-based planner overhead benchmark
    add_executable(bench_planner
        tests/bench/bench_planner.cpp
    )
    target_link_libraries(bench_planner PRIVATE
        zepto_sql
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_planner PRIVATE
        -O3 -march=native
    )

    # Cost-based planner benefit benchmark
    add_executable(bench_planner_benefit
        tests/bench/bench_planner_benefit.cpp
    )
    target_link_libraries(bench_planner_benefit PRIVATE
        zepto_sql
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_planner_benefit PRIVATE
        -O3 -march=native
    )

    # 병렬 쿼리 벤치마크
    add_executable(bench_parallel
        tests/bench/bench_parallel.cpp
    )
    target_link_libraries(bench_parallel PRIVATE
        zepto_sql
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
    )
    target_compile_options(bench_parallel PRIVATE
        -O3 -march=native
    )

    # Live rebalancing load test (HTTP client, runs on EKS loadgen pod)
    add_executable(bench_rebalance
        tests/bench/bench_rebalance.cpp
    )
    target_include_directories(bench_rebalance PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(bench_rebalance PRIVATE Threads::Threads)
    if(OPENSSL_FOUND)
        target_link_libraries(bench_rebalance PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    target_compile_options(bench_rebalance PRIVATE
        -O3 -march=native
    )

    # Multi-pod ingest scaling benchmark (HTTP client, runs on EKS loadgen pod)
    add_executable(bench_ingest_scale
        tests/bench/bench_ingest_scale.cpp
    )
    target_include_directories(bench_ingest_scale PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(bench_ingest_scale PRIVATE Threads::Threads)
    if(OPENSSL_FOUND)
        target_link_libraries(bench_ingest_scale PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    target_compile_options(bench_ingest_scale PRIVATE
        -O3 -march=native
    )
endif()

# ============================================================================
# Phase D: Python Binding (pybind11)
# ============================================================================
option(ZEPTO_BUILD_PYTHON "Build Python binding module" ON)

if(ZEPTO_BUILD_PYTHON)
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
    pybind11_add_module(zeptodb
        src/transpiler/python_binding.cpp
    )
    target_include_directories(zeptodb PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${NUMPY_INCLUDE_DIR}
    )
    target_link_libraries(zeptodb PRIVATE
        zepto_core
        zepto_storage
        zepto_ingestion
        zepto_execution
        zepto_sql
        zepto_cluster
    )
    target_compile_options(zeptodb PRIVATE
        -O3
        # pybind11이 -Wpedantic에서 일부 경고 발생 → 억제
        -Wno-pedantic
        -Wno-unused-parameter
    )

    # 빌드 디렉토리에 zepto_py 패키지 복사 (테스트 시 import 용이)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/zepto_py")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/transpiler/zepto_py/dsl.py"
        "${CMAKE_BINARY_DIR}/zepto_py/dsl.py"
        COPYONLY
    )

    # zepto_py Python 패키지 파일 복사
    foreach(_py_file
        __init__.py
        connection.py
        dataframe.py
        streaming.py
        arrow.py
        utils.py
    )
        configure_file(
            "${CMAKE_SOURCE_DIR}/zepto_py/${_py_file}"
            "${CMAKE_BINARY_DIR}/zepto_py/${_py_file}"
            COPYONLY
        )
    endforeach()

    message(STATUS "Python module: zeptodb (+ zepto_py pandas/polars/arrow integration)")
endif()

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== ZeptoDB v${PROJECT_VERSION} Build Config ===")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  HugePages:     ${ZEPTO_USE_HUGEPAGES}")
message(STATUS "  UCX (RDMA):    ${ZEPTO_USE_UCX}")
message(STATUS "  Highway SIMD:  ${ZEPTO_USE_HIGHWAY}")
message(STATUS "  LLVM JIT:      ${ZEPTO_USE_JIT}")
message(STATUS "  LZ4:           ${ZEPTO_USE_LZ4}")
message(STATUS "  Parquet:       ${ZEPTO_USE_PARQUET}")
message(STATUS "  S3 Upload:     ${ZEPTO_USE_S3}")
message(STATUS "  Arrow Flight:  ${ZEPTO_HAS_FLIGHT}")
message(STATUS "  Kinesis:       ${ZEPTO_HAS_KINESIS}")
message(STATUS "  Pulsar:        ${ZEPTO_HAS_PULSAR}")
message(STATUS "  OPC-UA:        ${ZEPTO_HAS_OPCUA}")
message(STATUS "  OPC-UA HA:     ${ZEPTO_OPCUA_HAS_HISTORIZING}")
message(STATUS "  DuckDB:        ${ZEPTO_ENABLE_DUCKDB}")
message(STATUS "  hnswlib ANN:   ${ZEPTO_ENABLE_HNSWLIB}")
message(STATUS "  Tests:         ${ZEPTO_BUILD_TESTS}")
message(STATUS "=================================")
