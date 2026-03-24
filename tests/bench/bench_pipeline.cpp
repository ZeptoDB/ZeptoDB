// ============================================================================
// ZeptoDB: End-to-End Benchmark Framework
// ============================================================================
// 측정 항목:
//   1. 인제스션 처리량 (ticks/sec, 다양한 배치 크기)
//   2. 쿼리 레이턴시 p50/p99/p999 (VWAP, filter+sum, full scan)
//   3. 엔드투엔드 레이턴시 (틱 수신 → 쿼리 결과)
//   4. 멀티스레드 동시 성능
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/common/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace zeptodb;
using namespace zeptodb::core;
using namespace zeptodb::ingestion;

// ============================================================================
// 타이머 유틸리티
// ============================================================================
static inline int64_t rdtsc_or_ns() {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<int64_t>(hi) << 32) | lo;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
#endif
}

static inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// 통계 계산
// ============================================================================
struct LatencyStats {
    double p50_us;
    double p99_us;
    double p999_us;
    double mean_us;
    double min_us;
    double max_us;
    size_t count;
};

static LatencyStats compute_stats(std::vector<int64_t>& samples_ns) {
    if (samples_ns.empty()) return {};
    std::sort(samples_ns.begin(), samples_ns.end());

    const size_t n = samples_ns.size();
    auto ns_to_us = [](int64_t ns) { return ns / 1000.0; };

    LatencyStats s;
    s.count  = n;
    s.min_us = ns_to_us(samples_ns.front());
    s.max_us = ns_to_us(samples_ns.back());
    s.p50_us = ns_to_us(samples_ns[n * 50 / 100]);
    s.p99_us = ns_to_us(samples_ns[n * 99 / 100]);
    s.p999_us= ns_to_us(samples_ns[std::min(n - 1, n * 999 / 1000)]);

    const double sum = std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0);
    s.mean_us = ns_to_us(static_cast<int64_t>(sum / n));

    return s;
}

// ============================================================================
// 테스트 데이터 생성
// ============================================================================
static TickMessage make_tick(SymbolId sym, int64_t base_ts, int idx) {
    TickMessage msg{};
    msg.symbol_id = sym;
    msg.recv_ts   = base_ts + static_cast<int64_t>(idx) * 1000; // 1μs 간격
    msg.price     = 100'0000 + (idx % 1000) * 100; // 100.00 ~ 209.90 (x10000)
    msg.volume    = 100 + (idx % 500);
    msg.msg_type  = 0; // Trade
    return msg;
}

// ============================================================================
// BENCH 1: 인제스션 처리량
// ============================================================================
static void bench_ingestion_throughput() {
    printf("\n===== BENCH 1: 인제스션 처리량 =====\n");

    const std::vector<size_t> batch_sizes = {1, 64, 512, 4096, 65535};
    const size_t TOTAL_TICKS = 1'000'000;

    for (size_t batch : batch_sizes) {
        PipelineConfig cfg;
        cfg.arena_size_per_partition = 128ULL * 1024 * 1024; // 128MB
        cfg.drain_batch_size = 1024;

        ZeptoPipeline pipeline(cfg);
        // drain 없이 순수 ingest 처리량 측정 (TickPlant queue 기준)
        // queue 용량은 65536이므로 그 이상은 flush 필요

        const int64_t base_ts = now_ns();
        size_t sent = 0;
        size_t dropped = 0;

        const int64_t t_start = now_ns();

        // 배치 단위로 인제스트 + 중간 drain
        const size_t actual_batch = std::min(batch, (size_t)65535);
        size_t remaining = TOTAL_TICKS;

        while (remaining > 0) {
            const size_t this_batch = std::min(actual_batch, remaining);
            for (size_t i = 0; i < this_batch; ++i) {
                TickMessage msg = make_tick(1, base_ts, static_cast<int>(sent + i));
                if (!pipeline.ingest_tick(msg)) ++dropped;
            }
            // 큐 오버플로우 방지: 동기 드레인
            pipeline.drain_sync(this_batch + 256);
            sent      += this_batch;
            remaining -= this_batch;
        }

        const int64_t t_end = now_ns();
        const double elapsed_s = (t_end - t_start) / 1e9;
        const double tps = (sent - dropped) / elapsed_s;

        printf("[BENCH] 인제스션: %.2fM ticks/sec (batch=%zu, total=%zu, dropped=%zu)\n",
               tps / 1e6, actual_batch, sent, dropped);
    }
}

// ============================================================================
// BENCH 2: 쿼리 레이턴시 (p50/p99/p999)
// ============================================================================
static void bench_query_latency() {
    printf("\n===== BENCH 2: 쿼리 레이턴시 =====\n");

    // 아레나 크기: N rows * 8bytes * 4cols * doubling_waste_factor(2.5)
    const std::vector<std::pair<size_t, size_t>> tests = {
        {100'000,   128ULL * 1024 * 1024},   // 100K rows → 128MB
        {1'000'000, 256ULL * 1024 * 1024},   // 1M rows → 256MB
        {5'000'000, 768ULL * 1024 * 1024},   // 5M rows → 768MB
    };
    const size_t QUERY_ITERS = 1000;

    for (auto [num_rows, arena_mb] : tests) {
        PipelineConfig cfg;
        cfg.arena_size_per_partition = arena_mb;

        ZeptoPipeline pipeline(cfg);

        // 데이터 로드
        const int64_t base_ts = 1'700'000'000'000'000'000LL; // 2023-11-14 기준
        for (size_t i = 0; i < num_rows; ++i) {
            TickMessage msg = make_tick(42, base_ts, static_cast<int>(i));
            pipeline.ingest_tick(msg);
            // 주기적 드레인
            if ((i & 0xFFFF) == 0) {
                pipeline.drain_sync(65536);
            }
        }
        pipeline.drain_sync(SIZE_MAX);

        const size_t stored = pipeline.total_stored_rows();
        if (stored == 0) {
            printf("[BENCH] WARNING: 저장된 데이터 없음 (rows=%zu)\n", num_rows);
            continue;
        }

        // --- VWAP 쿼리 레이턴시 측정 ---
        {
            std::vector<int64_t> latencies;
            latencies.reserve(QUERY_ITERS);

            for (size_t q = 0; q < QUERY_ITERS; ++q) {
                const int64_t t0 = now_ns();
                auto r = pipeline.query_vwap(42);
                const int64_t dt = now_ns() - t0;
                (void)r;
                latencies.push_back(dt);
            }

            auto s = compute_stats(latencies);
            printf("[BENCH] VWAP 쿼리:       p50=%.1fμs p99=%.1fμs p999=%.1fμs"
                   " (rows=%zuK, stored=%zu)\n",
                   s.p50_us, s.p99_us, s.p999_us,
                   num_rows / 1000, stored);
        }

        // --- Filter+Sum 쿼리 레이턴시 측정 ---
        {
            std::vector<int64_t> latencies;
            latencies.reserve(QUERY_ITERS);
            const int64_t threshold = 100'5000; // 중간값

            for (size_t q = 0; q < QUERY_ITERS; ++q) {
                const int64_t t0 = now_ns();
                auto r = pipeline.query_filter_sum(42, "price", threshold);
                const int64_t dt = now_ns() - t0;
                (void)r;
                latencies.push_back(dt);
            }

            auto s = compute_stats(latencies);
            printf("[BENCH] Filter+Sum 쿼리: p50=%.1fμs p99=%.1fμs p999=%.1fμs"
                   " (rows=%zuK)\n",
                   s.p50_us, s.p99_us, s.p999_us, num_rows / 1000);
        }

        // --- Full Count 쿼리 ---
        {
            std::vector<int64_t> latencies;
            latencies.reserve(QUERY_ITERS);

            for (size_t q = 0; q < QUERY_ITERS; ++q) {
                const int64_t t0 = now_ns();
                auto r = pipeline.query_count(42);
                const int64_t dt = now_ns() - t0;
                (void)r;
                latencies.push_back(dt);
            }

            auto s = compute_stats(latencies);
            printf("[BENCH] Full Count 쿼리: p50=%.1fμs p99=%.1fμs p999=%.1fμs"
                   " (rows=%zuK)\n",
                   s.p50_us, s.p99_us, s.p999_us, num_rows / 1000);
        }
    }
}

// ============================================================================
// BENCH 3: 엔드투엔드 레이턴시 (ingest → query)
// ============================================================================
static void bench_e2e_latency() {
    printf("\n===== BENCH 3: 엔드투엔드 레이턴시 =====\n");

    const size_t WARMUP_TICKS  = 100'000;
    const size_t MEASURE_ITERS = 500;

    PipelineConfig cfg;
    cfg.drain_batch_size = 64;
    cfg.drain_sleep_us = 1;

    ZeptoPipeline pipeline(cfg);
    pipeline.start(); // 백그라운드 드레인 시작

    // 워밍업
    const int64_t base_ts = now_ns();
    for (size_t i = 0; i < WARMUP_TICKS; ++i) {
        pipeline.ingest_tick(make_tick(99, base_ts, static_cast<int>(i)));
    }
    // 워밍업 데이터 드레인 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // E2E 레이턴시 측정:
    // 1. 틱 인제스트
    // 2. 동기 드레인 (즉시 저장)
    // 3. VWAP 쿼리
    // 전체 시간 측정
    std::vector<int64_t> e2e_latencies;
    e2e_latencies.reserve(MEASURE_ITERS);

    pipeline.stop(); // 백그라운드 드레인 중지 (동기 측정을 위해)

    for (size_t i = 0; i < MEASURE_ITERS; ++i) {
        const int64_t t_start = now_ns();

        // 1) 인제스트
        pipeline.ingest_tick(make_tick(99, base_ts, static_cast<int>(WARMUP_TICKS + i)));

        // 2) 동기 드레인 (저장)
        pipeline.drain_sync(1);

        // 3) VWAP 쿼리
        auto r = pipeline.query_vwap(99);
        (void)r;

        const int64_t t_end = now_ns();
        e2e_latencies.push_back(t_end - t_start);
    }

    auto s = compute_stats(e2e_latencies);
    printf("[BENCH] E2E 레이턴시 (ingest→store→query):\n");
    printf("  p50=%.1fμs  p99=%.1fμs  p999=%.1fμs\n",
           s.p50_us, s.p99_us, s.p999_us);
    printf("  mean=%.1fμs  min=%.1fμs  max=%.1fμs\n",
           s.mean_us, s.min_us, s.max_us);
    printf("  (iterations=%zu, total_stored=%zu)\n",
           MEASURE_ITERS, pipeline.total_stored_rows());
}

// ============================================================================
// BENCH 4: 멀티 프로듀서 동시 성능
// ============================================================================
static void bench_concurrent() {
    printf("\n===== BENCH 4: 멀티 프로듀서 동시 성능 =====\n");

    const std::vector<int> thread_counts = {1, 2, 4};
    const size_t TICKS_PER_THREAD = 500'000;

    for (int num_threads : thread_counts) {
        PipelineConfig cfg;
        cfg.arena_size_per_partition = 512ULL * 1024 * 1024; // 512MB per partition
        cfg.drain_batch_size = 1024;

        ZeptoPipeline pipeline(cfg);
        pipeline.start();

        std::atomic<uint64_t> total_ingested{0};
        std::vector<std::thread> producers;

        const int64_t t_start = now_ns();

        for (int t = 0; t < num_threads; ++t) {
            producers.emplace_back([&, t]() {
                const int64_t base_ts = now_ns();
                uint64_t local_sent = 0;
                const SymbolId sym = static_cast<SymbolId>(t + 1);

                for (size_t i = 0; i < TICKS_PER_THREAD; ++i) {
                    TickMessage msg = make_tick(sym, base_ts, static_cast<int>(i));
                    if (pipeline.ingest_tick(msg)) ++local_sent;

                    // 큐 오버플로우 방지를 위한 가끔씩 양보
                    if ((i & 0x3FFF) == 0) {
                        std::this_thread::yield();
                    }
                }
                total_ingested.fetch_add(local_sent, std::memory_order_relaxed);
            });
        }

        for (auto& th : producers) th.join();

        pipeline.stop();

        const int64_t t_end = now_ns();
        const double elapsed_s = (t_end - t_start) / 1e9;
        const uint64_t ingested = total_ingested.load();
        const uint64_t stored   = pipeline.total_stored_rows();
        const double tps = static_cast<double>(stored) / elapsed_s;

        printf("[BENCH] 동시 인제스션: %.2fM ticks/sec"
               " (threads=%d, ingested=%lu, stored=%lu, elapsed=%.2fs)\n",
               tps / 1e6, num_threads, ingested, stored, elapsed_s);
    }
}

// ============================================================================
// BENCH 5: 대용량 VWAP (10M rows)
// ============================================================================
static void bench_large_vwap() {
    printf("\n===== BENCH 5: 대용량 VWAP (10M rows) =====\n");

    const size_t TARGET_ROWS = 10'000'000;

    PipelineConfig cfg;
    // 10M rows * 8 bytes * 4 cols * doubling_waste = ~1.2GB
    cfg.arena_size_per_partition = 1536ULL * 1024 * 1024; // 1.5GB

    ZeptoPipeline pipeline(cfg);

    printf("  데이터 로드 중 (%zuM ticks)...\n", TARGET_ROWS / 1'000'000);
    const int64_t base_ts = 1'700'000'000'000'000'000LL;
    const int64_t load_t0 = now_ns();

    for (size_t i = 0; i < TARGET_ROWS; ++i) {
        pipeline.ingest_tick(make_tick(7, base_ts, static_cast<int>(i)));
        if ((i & 0xFFFF) == 0) {
            pipeline.drain_sync(65536);
        }
    }
    pipeline.drain_sync(SIZE_MAX);

    const double load_sec = (now_ns() - load_t0) / 1e9;
    const size_t stored = pipeline.total_stored_rows();
    printf("  로드 완료: stored=%zuM rows (%.1fs, %.2fM/sec)\n",
           stored / 1'000'000, load_sec, stored / 1e6 / load_sec);

    if (stored == 0) {
        printf("  WARNING: 데이터 없음\n");
        return;
    }

    // VWAP 반복 측정
    const size_t ITERS = 200;
    std::vector<int64_t> latencies;
    latencies.reserve(ITERS);

    for (size_t q = 0; q < ITERS; ++q) {
        const int64_t t0 = now_ns();
        const auto r = pipeline.query_vwap(7);
        latencies.push_back(now_ns() - t0);
        (void)r;
    }

    auto s = compute_stats(latencies);
    printf("[BENCH] VWAP 쿼리 (%zuM rows):\n", stored / 1'000'000);
    printf("  p50=%.1fμs  p99=%.1fμs  p999=%.1fμs\n",
           s.p50_us, s.p99_us, s.p999_us);
    printf("  mean=%.1fμs  throughput=%.1fM rows/sec\n",
           s.mean_us, stored / 1e6 / (s.mean_us / 1e6));
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    zeptodb::Logger::init("apex-bench", spdlog::level::warn); // 벤치 중 로그 최소화

    printf("============================================================\n");
    printf("  ZeptoDB Benchmark Suite\n");
    printf("  컴파일: C++20 / clang-19 / -O3 -march=native\n");
    printf("============================================================\n");

    const bool run_all = (argc == 1);
    auto should_run = [&](const char* name) {
        if (run_all) return true;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], name) == 0) return true;
        }
        return false;
    };

    if (should_run("ingest"))     bench_ingestion_throughput();
    if (should_run("query"))      bench_query_latency();
    if (should_run("e2e"))        bench_e2e_latency();
    if (should_run("concurrent")) bench_concurrent();
    if (should_run("large"))      bench_large_vwap();

    printf("\n============================================================\n");
    printf("  벤치마크 완료\n");
    printf("============================================================\n");

    return 0;
}
