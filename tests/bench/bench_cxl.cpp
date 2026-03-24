// ============================================================================
// Benchmark: SharedMem vs CXL(시뮬) vs CXL(레이턴시 주입) 비교
// ============================================================================

#include "shm_backend.h"
#include "zeptodb/cluster/cxl_backend.h"
#include "zeptodb/cluster/transport.h"
#include "zeptodb/common/logger.h"

#include <chrono>
#include <cstdio>
#include <vector>
#include <numeric>

using namespace zeptodb::cluster;
using Clock = std::chrono::steady_clock;

// ============================================================================
// 벤치 유틸
// ============================================================================
struct BenchResult {
    double ns_per_op;
    double throughput_gbps;
};

template <typename Transport>
BenchResult bench_write(Transport& t, RemoteRegion region,
                        const void* data, size_t chunk_size, size_t iterations)
{
    auto t0 = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        t.remote_write(data, region, 0, chunk_size);
    }
    t.fence();
    auto elapsed = Clock::now() - t0;
    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    double ns_op = ns / iterations;
    double bytes_total = static_cast<double>(chunk_size) * iterations;
    double gbps = bytes_total / ns;  // GB/ns = GB/s
    return {ns_op, gbps};
}

template <typename Transport>
BenchResult bench_read(Transport& t, RemoteRegion region,
                       void* buf, size_t chunk_size, size_t iterations)
{
    auto t0 = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        t.remote_read(region, 0, buf, chunk_size);
    }
    auto elapsed = Clock::now() - t0;
    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    double ns_op = ns / iterations;
    double bytes_total = static_cast<double>(chunk_size) * iterations;
    double gbps = bytes_total / ns;
    return {ns_op, gbps};
}

// ============================================================================
// 메인
// ============================================================================
int main() {
    zeptodb::Logger::init("bench-cxl", spdlog::level::warn);

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║   ZeptoDB Transport Backend Comparison                     ║\n");
    printf("║   SharedMem vs CXL(no latency) vs CXL(200ns) vs CXL(300ns)║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    constexpr size_t POOL = 64ULL * 1024 * 1024;
    constexpr size_t CHUNK_64B = 64;           // 1 cache line (= 1 tick)
    constexpr size_t CHUNK_4K = 4096;          // 1 page
    constexpr size_t CHUNK_1M = 1024 * 1024;   // bulk transfer

    // 데이터 준비
    std::vector<char> data_64(CHUNK_64B, 'A');
    std::vector<char> data_4k(CHUNK_4K, 'B');
    std::vector<char> data_1m(CHUNK_1M, 'C');
    std::vector<char> read_buf(CHUNK_1M);

    // ================================================================
    // Backend 1: SharedMem (RDMA 로컬 시뮬레이션 베이스라인)
    // ================================================================
    printf("=== 1. SharedMem Backend (baseline) ===\n");
    {
        SharedMemBackend shm;
        shm.init(NodeAddress{"localhost", 9001, 1});
        auto region = shm.register_memory(nullptr, POOL);

        auto r64 = bench_write(shm, region, data_64.data(), CHUNK_64B, 1'000'000);
        printf("  write  64B: %8.1f ns/op  %6.2f GB/s\n", r64.ns_per_op, r64.throughput_gbps);

        auto r4k = bench_write(shm, region, data_4k.data(), CHUNK_4K, 500'000);
        printf("  write  4KB: %8.1f ns/op  %6.2f GB/s\n", r4k.ns_per_op, r4k.throughput_gbps);

        auto r1m = bench_write(shm, region, data_1m.data(), CHUNK_1M, 1000);
        printf("  write  1MB: %8.1f ns/op  %6.2f GB/s\n", r1m.ns_per_op, r1m.throughput_gbps);

        auto rd = bench_read(shm, region, read_buf.data(), CHUNK_64B, 1'000'000);
        printf("  read   64B: %8.1f ns/op  %6.2f GB/s\n", rd.ns_per_op, rd.throughput_gbps);

        shm.shutdown();
    }

    // ================================================================
    // Backend 2: CXL (레이턴시 없음 — 순수 인터페이스 교체 검증)
    // ================================================================
    printf("\n=== 2. CXL Backend (no latency injection) ===\n");
    {
        CXLConfig config{.pool_size = POOL, .inject_latency = false};
        CXLBackend cxl(config);
        cxl.init(NodeAddress{"localhost", 9002, 2});
        auto region = cxl.register_memory(nullptr, POOL);

        auto r64 = bench_write(cxl, region, data_64.data(), CHUNK_64B, 1'000'000);
        printf("  write  64B: %8.1f ns/op  %6.2f GB/s\n", r64.ns_per_op, r64.throughput_gbps);

        auto r4k = bench_write(cxl, region, data_4k.data(), CHUNK_4K, 500'000);
        printf("  write  4KB: %8.1f ns/op  %6.2f GB/s\n", r4k.ns_per_op, r4k.throughput_gbps);

        auto r1m = bench_write(cxl, region, data_1m.data(), CHUNK_1M, 1000);
        printf("  write  1MB: %8.1f ns/op  %6.2f GB/s\n", r1m.ns_per_op, r1m.throughput_gbps);

        // Direct Access (CXL 고유) — read 필요 없이 포인터 직접 접근
        auto t0 = Clock::now();
        volatile int64_t sink = 0;
        const int64_t* ptr = cxl.direct_access<int64_t>(region);
        for (size_t i = 0; i < 1'000'000; ++i) {
            sink = ptr[i % (POOL / sizeof(int64_t))];
        }
        auto elapsed = Clock::now() - t0;
        double direct_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 1'000'000.0;
        printf("  direct 8B:  %8.1f ns/op  (CXL load/store — zero copy)\n", direct_ns);

        cxl.shutdown();
    }

    // ================================================================
    // Backend 3: CXL (200ns 레이턴시 — CXL 3.0 같은 랙)
    // ================================================================
    printf("\n=== 3. CXL Backend (200ns latency — same rack) ===\n");
    {
        CXLConfig config{.pool_size = POOL, .inject_latency = true, .latency_ns = 200};
        CXLBackend cxl(config);
        cxl.init(NodeAddress{"localhost", 9003, 3});
        auto region = cxl.register_memory(nullptr, POOL);

        auto r64 = bench_write(cxl, region, data_64.data(), CHUNK_64B, 100'000);
        printf("  write  64B: %8.1f ns/op  %6.2f GB/s\n", r64.ns_per_op, r64.throughput_gbps);

        auto r4k = bench_write(cxl, region, data_4k.data(), CHUNK_4K, 50'000);
        printf("  write  4KB: %8.1f ns/op  %6.2f GB/s\n", r4k.ns_per_op, r4k.throughput_gbps);

        auto r1m = bench_write(cxl, region, data_1m.data(), CHUNK_1M, 500);
        printf("  write  1MB: %8.1f ns/op  %6.2f GB/s\n", r1m.ns_per_op, r1m.throughput_gbps);

        cxl.shutdown();
    }

    // ================================================================
    // Backend 4: CXL (300ns 레이턴시 — CXL 크로스 소켓)
    // ================================================================
    printf("\n=== 4. CXL Backend (300ns latency — cross socket) ===\n");
    {
        CXLConfig config{.pool_size = POOL, .inject_latency = true, .latency_ns = 300};
        CXLBackend cxl(config);
        cxl.init(NodeAddress{"localhost", 9004, 4});
        auto region = cxl.register_memory(nullptr, POOL);

        auto r64 = bench_write(cxl, region, data_64.data(), CHUNK_64B, 100'000);
        printf("  write  64B: %8.1f ns/op  %6.2f GB/s\n", r64.ns_per_op, r64.throughput_gbps);

        auto r4k = bench_write(cxl, region, data_4k.data(), CHUNK_4K, 50'000);
        printf("  write  4KB: %8.1f ns/op  %6.2f GB/s\n", r4k.ns_per_op, r4k.throughput_gbps);

        cxl.shutdown();
    }

    // ================================================================
    // 요약 비교
    // ================================================================
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║   Transport 교체 검증: 동일 코드, 다른 백엔드               ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║   SharedMem (RDMA sim)  → 테스트용 베이스라인              ║\n");
    printf("║   CXL (no inject)       → 인터페이스 교체 검증             ║\n");
    printf("║   CXL (200ns)           → CXL 3.0 same-rack 시뮬레이션    ║\n");
    printf("║   CXL (300ns)           → CXL 3.0 cross-socket 시뮬레이션 ║\n");
    printf("║                                                            ║\n");
    printf("║   ✅ 코드 수정 제로 — using Transport = Backend만 바꿈     ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}
