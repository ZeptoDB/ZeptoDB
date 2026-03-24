// ============================================================================
// Test: CXL Backend + Transport 교체 검증
// ============================================================================

#include "zeptodb/cluster/cxl_backend.h"
#include "shm_backend.h"
#include "zeptodb/cluster/transport.h"
#include <gtest/gtest.h>
#include <vector>
#include <numeric>

using namespace zeptodb::cluster;

// ============================================================================
// CXL Backend 기본 기능 테스트
// ============================================================================

TEST(CXLBackend, InitAndShutdown) {
    CXLConfig config{.pool_size = 4 * 1024 * 1024, .inject_latency = false};
    CXLBackend cxl(config);
    cxl.init(NodeAddress{"localhost", 9000, 1});
    EXPECT_EQ(cxl.pool_used(), 0);
    EXPECT_EQ(cxl.pool_capacity(), 4 * 1024 * 1024);
    cxl.shutdown();
}

TEST(CXLBackend, RegisterAndAccess) {
    CXLConfig config{.pool_size = 4 * 1024 * 1024, .inject_latency = false};
    CXLBackend cxl(config);
    cxl.init(NodeAddress{"localhost", 9000, 1});

    // 데이터 준비
    std::vector<int64_t> prices = {100, 200, 300, 400, 500};

    // CXL 메모리에 등록
    auto region = cxl.register_memory(prices.data(), prices.size() * sizeof(int64_t));
    EXPECT_GT(region.remote_addr, 0);
    EXPECT_EQ(region.size, prices.size() * sizeof(int64_t));

    // Direct Access (CXL 고유 기능) — 포인터로 직접 읽기
    const int64_t* cxl_prices = cxl.direct_access<int64_t>(region);
    EXPECT_EQ(cxl_prices[0], 100);
    EXPECT_EQ(cxl_prices[4], 500);

    cxl.shutdown();
}

TEST(CXLBackend, WriteAndRead) {
    CXLConfig config{.pool_size = 4 * 1024 * 1024, .inject_latency = false};
    CXLBackend cxl(config);
    cxl.init(NodeAddress{"localhost", 9000, 1});

    // 빈 영역 등록
    auto region = cxl.register_memory(nullptr, 1024);

    // remote_write로 데이터 쓰기
    int64_t value = 42;
    cxl.remote_write(&value, region, 0, sizeof(int64_t));

    // remote_read로 데이터 읽기
    int64_t read_back = 0;
    cxl.remote_read(region, 0, &read_back, sizeof(int64_t));

    EXPECT_EQ(read_back, 42);

    // Direct Access로도 같은 값
    EXPECT_EQ(*cxl.direct_access<int64_t>(region), 42);

    cxl.shutdown();
}

TEST(CXLBackend, BulkDataIntegrity) {
    CXLConfig config{.pool_size = 64 * 1024 * 1024, .inject_latency = false};
    CXLBackend cxl(config);
    cxl.init(NodeAddress{"localhost", 9000, 1});

    // 100만 행 데이터
    constexpr size_t N = 1'000'000;
    std::vector<int64_t> original(N);
    std::iota(original.begin(), original.end(), 0);

    auto region = cxl.register_memory(nullptr, N * sizeof(int64_t));

    // CXL 메모리에 쓰기
    cxl.remote_write(original.data(), region, 0, N * sizeof(int64_t));
    cxl.fence();

    // 읽기 검증
    std::vector<int64_t> readback(N);
    cxl.remote_read(region, 0, readback.data(), N * sizeof(int64_t));

    EXPECT_EQ(original, readback);

    // Direct Access 검증
    const int64_t* ptr = cxl.direct_access<int64_t>(region);
    EXPECT_EQ(ptr[0], 0);
    EXPECT_EQ(ptr[N-1], static_cast<int64_t>(N-1));

    cxl.shutdown();
}

TEST(CXLBackend, LatencyInjection) {
    // 레이턴시 주입 ON: ~200ns per operation
    CXLConfig config{
        .pool_size = 1024 * 1024,
        .inject_latency = true,
        .latency_ns = 200
    };
    CXLBackend cxl(config);
    cxl.init(NodeAddress{"localhost", 9000, 1});

    auto region = cxl.register_memory(nullptr, 1024);
    int64_t val = 99;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        cxl.remote_write(&val, region, 0, sizeof(int64_t));
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    auto ns_per_op = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 1000;

    // 200ns 주입 → 실제로 200ns 이상이어야 함
    EXPECT_GE(ns_per_op, 150);  // 약간의 오차 허용
    EXPECT_LE(ns_per_op, 500);  // 너무 느리면 안 됨

    cxl.shutdown();
}

// ============================================================================
// 핵심 검증: Transport 교체 — 동일 코드, 다른 백엔드
// ============================================================================

// 템플릿 함수: 어떤 Transport든 동일한 로직으로 동작
template <typename Transport>
int64_t run_write_read_test(Transport& transport, size_t n) {
    auto region = transport.register_memory(nullptr, n * sizeof(int64_t));

    // 데이터 쓰기
    std::vector<int64_t> data(n);
    std::iota(data.begin(), data.end(), 1);
    transport.remote_write(data.data(), region, 0, n * sizeof(int64_t));
    transport.fence();

    // 합계 읽어서 반환
    std::vector<int64_t> result(n);
    transport.remote_read(region, 0, result.data(), n * sizeof(int64_t));

    int64_t sum = 0;
    for (auto v : result) sum += v;
    return sum;
}

TEST(TransportSwap, SameCodeDifferentBackend) {
    // 동일한 run_write_read_test 함수를 두 가지 백엔드로 실행

    constexpr size_t N = 10000;
    int64_t expected = static_cast<int64_t>(N) * (N + 1) / 2;

    // Backend 1: SharedMem
    SharedMemBackend shm;
    shm.init(NodeAddress{"localhost", 9001, 1});
    int64_t result_shm = run_write_read_test(shm, N);
    EXPECT_EQ(result_shm, expected);
    shm.shutdown();

    // Backend 2: CXL (시뮬레이션, 레이턴시 없이)
    CXLConfig config{.pool_size = 4 * 1024 * 1024, .inject_latency = false};
    CXLBackend cxl(config);
    cxl.init(NodeAddress{"localhost", 9002, 2});
    int64_t result_cxl = run_write_read_test(cxl, N);
    EXPECT_EQ(result_cxl, expected);
    cxl.shutdown();

    // 두 백엔드 결과 동일!
    EXPECT_EQ(result_shm, result_cxl);
}
