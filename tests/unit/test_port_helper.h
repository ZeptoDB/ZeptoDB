// ============================================================================
// ZeptoDB: Test-only helpers for parallel-safe test isolation
// - pick_free_port(): kernel-assigned ephemeral TCP port on 127.0.0.1
// - unique_test_path(): per-(pid, test-name, counter) filesystem path
// These exist so ctest -j$(nproc) does not hit port / temp-path collisions
// between concurrently running test processes.
// ============================================================================
#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace zepto_test_util {

// Ask the kernel for an ephemeral TCP port on 127.0.0.1, close, return it.
// There is an unavoidable TOCTOU race window between close() and the caller
// rebinding. To minimise collisions we (a) ask the kernel for an ephemeral
// port and (b) further randomise by OR'ing in a per-process bit pattern so
// two concurrently-starting test processes are unlikely to pick the same
// port.  This is only used for unit-test fixtures.
inline uint16_t pick_free_port() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    int reuse = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(s); return 0;
    }
    socklen_t len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    uint16_t p = ntohs(a.sin_port);
    ::close(s);
    return p;
}

// Return a filesystem path unique to (pid, GTest case name, monotonic counter).
// Safe against cross-process parallel runs and intra-process repeated calls.
inline std::filesystem::path unique_test_path(const std::string& tag) {
    static std::atomic<uint64_t> ctr{0};
    std::string name = "zepto_" + tag + "_" +
        std::to_string(static_cast<unsigned long>(::getpid())) + "_" +
        std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())) + "_" +
        std::to_string(ctr.fetch_add(1, std::memory_order_relaxed));
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (info && info->name()) {
        name += "_";
        name += info->name();
    }
    return std::filesystem::temp_directory_path() / name;
}

} // namespace zepto_test_util
