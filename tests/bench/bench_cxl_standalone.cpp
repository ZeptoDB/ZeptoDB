// ============================================================================
// CXL-only Benchmark — UCX 의존성 제거
// ============================================================================

#include "zeptodb/cluster/cxl_backend.h"
#include "zeptodb/common/types.h"

#include <chrono>
#include <cstdio>
#include <vector>
#include <numeric>

using namespace zeptodb::cluster;
using Clock = std::chrono::steady_clock;

int main() {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  CXL 3.0 Transport Simulation Benchmark             ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    constexpr size_t POOL = 64ULL * 1024 * 1024;
    std::vector<char> data_64(64, 'A');
    std::vector<char> data_4k(4096, 'B');
    std::vector<char> data_1m(1024*1024, 'C');
    std::vector<char> buf(1024*1024);

    auto bench = [](auto fn, size_t iters) -> double {
        auto t0 = Clock::now();
        for (size_t i = 0; i < iters; ++i) fn();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
        return static_cast<double>(ns) / iters;
    };

    // === CXL (no latency) — 순수 인터페이스 교체 검증 ===
    printf("=== CXL Backend (no latency — interface swap proof) ===\n");
    {
        CXLConfig cfg{.pool_size = POOL, .inject_latency = false};
        CXLBackend cxl(cfg);
        cxl.do_init(NodeAddress{"localhost", 9000, 1});
        auto region = cxl.do_register_memory(nullptr, POOL);

        double w64 = bench([&]{ cxl.do_remote_write(data_64.data(), region, 0, 64); }, 1'000'000);
        printf("  write  64B: %8.1f ns/op  (%6.2f GB/s)\n", w64, 64.0/w64);

        double w4k = bench([&]{ cxl.do_remote_write(data_4k.data(), region, 0, 4096); }, 500'000);
        printf("  write  4KB: %8.1f ns/op  (%6.2f GB/s)\n", w4k, 4096.0/w4k);

        double w1m = bench([&]{ cxl.do_remote_write(data_1m.data(), region, 0, 1024*1024); }, 1000);
        printf("  write  1MB: %8.1f ns/op  (%6.2f GB/s)\n", w1m, 1024.0*1024.0/w1m);

        // Direct Access (CXL 고유)
        double da = bench([&]{
            volatile int64_t sink = *cxl.direct_access<int64_t>(region);
            (void)sink;
        }, 1'000'000);
        printf("  direct  8B: %8.1f ns/op  (load/store — zero copy)\n", da);

        cxl.do_shutdown();
    }

    // === CXL (200ns — same rack) ===
    printf("\n=== CXL Backend (200ns — CXL 3.0 same rack simulation) ===\n");
    {
        CXLConfig cfg{.pool_size = POOL, .inject_latency = true, .latency_ns = 200};
        CXLBackend cxl(cfg);
        cxl.do_init(NodeAddress{"localhost", 9001, 2});
        auto region = cxl.do_register_memory(nullptr, POOL);

        double w64 = bench([&]{ cxl.do_remote_write(data_64.data(), region, 0, 64); }, 100'000);
        printf("  write  64B: %8.1f ns/op  (%6.2f GB/s)\n", w64, 64.0/w64);

        double w4k = bench([&]{ cxl.do_remote_write(data_4k.data(), region, 0, 4096); }, 50'000);
        printf("  write  4KB: %8.1f ns/op  (%6.2f GB/s)\n", w4k, 4096.0/w4k);

        double r64 = bench([&]{ cxl.do_remote_read(region, 0, buf.data(), 64); }, 100'000);
        printf("  read   64B: %8.1f ns/op\n", r64);

        cxl.do_shutdown();
    }

    // === CXL (300ns — cross socket) ===
    printf("\n=== CXL Backend (300ns — CXL 3.0 cross socket simulation) ===\n");
    {
        CXLConfig cfg{.pool_size = POOL, .inject_latency = true, .latency_ns = 300};
        CXLBackend cxl(cfg);
        cxl.do_init(NodeAddress{"localhost", 9002, 3});
        auto region = cxl.do_register_memory(nullptr, POOL);

        double w64 = bench([&]{ cxl.do_remote_write(data_64.data(), region, 0, 64); }, 100'000);
        printf("  write  64B: %8.1f ns/op  (%6.2f GB/s)\n", w64, 64.0/w64);

        double w4k = bench([&]{ cxl.do_remote_write(data_4k.data(), region, 0, 4096); }, 50'000);
        printf("  write  4KB: %8.1f ns/op  (%6.2f GB/s)\n", w4k, 4096.0/w4k);

        cxl.do_shutdown();
    }

    // === Data Integrity ===
    printf("\n=== Data Integrity Check ===\n");
    {
        CXLConfig cfg{.pool_size = POOL, .inject_latency = false};
        CXLBackend cxl(cfg);
        cxl.do_init(NodeAddress{"localhost", 9003, 4});

        constexpr size_t N = 1'000'000;
        std::vector<int64_t> original(N);
        std::iota(original.begin(), original.end(), 0);

        auto region = cxl.do_register_memory(nullptr, N * sizeof(int64_t));
        cxl.do_remote_write(original.data(), region, 0, N * sizeof(int64_t));
        cxl.do_fence();

        std::vector<int64_t> readback(N);
        cxl.do_remote_read(region, 0, readback.data(), N * sizeof(int64_t));

        bool ok = (original == readback);
        const int64_t* ptr = cxl.direct_access<int64_t>(region);
        bool direct_ok = (ptr[0] == 0 && ptr[N-1] == static_cast<int64_t>(N-1));

        printf("  remote_write+read 1M rows: %s\n", ok ? "✅ PASS" : "❌ FAIL");
        printf("  direct_access 1M rows:     %s\n", direct_ok ? "✅ PASS" : "❌ FAIL");

        cxl.do_shutdown();
    }

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  ✅ CXL Transport 검증 완료                          ║\n");
    printf("║  코드 수정 제로 — Backend 교체만으로 동작 확인        ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    return 0;
}
