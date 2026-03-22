// ============================================================================
// APEX-DB: SQL 벤치마크
// ============================================================================
// 1. SQL 파싱 시간 (< 50μs 목표)
// 2. SQL 실행 vs 직접 C++ API (오버헤드 측정)
// 3. ASOF JOIN 성능 (다양한 데이터 크기)
// ============================================================================

#include "apex/sql/tokenizer.h"
#include "apex/sql/parser.h"
#include "apex/sql/executor.h"
#include "apex/execution/join_operator.h"
#include "apex/execution/window_function.h"
#include "apex/core/pipeline.h"
#include "apex/storage/arena_allocator.h"
#include "apex/storage/column_store.h"

#include <chrono>
#include <cstdio>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>

using namespace apex::sql;
using namespace apex::execution;
using namespace apex::storage;
using namespace apex::core;

// ============================================================================
// 타이머 유틸리티
// ============================================================================
using clock_t2 = std::chrono::high_resolution_clock;

static double elapsed_us(clock_t2::time_point start) {
    return std::chrono::duration<double, std::micro>(
        clock_t2::now() - start
    ).count();
}

// ============================================================================
// 벤치마크 헬퍼
// ============================================================================
struct BenchResult {
    double min_us, max_us, avg_us, p99_us;
    size_t iterations;
};

template <typename Fn>
BenchResult bench(const char* name, size_t iters, Fn&& fn) {
    std::vector<double> times;
    times.reserve(iters);

    // 워밍업
    for (int w = 0; w < 3; ++w) fn();

    for (size_t i = 0; i < iters; ++i) {
        auto t0 = clock_t2::now();
        fn();
        times.push_back(elapsed_us(t0));
    }

    std::sort(times.begin(), times.end());
    BenchResult r;
    r.iterations = iters;
    r.min_us     = times.front();
    r.max_us     = times.back();
    r.avg_us     = std::accumulate(times.begin(), times.end(), 0.0) / iters;
    r.p99_us     = times[static_cast<size_t>(iters * 0.99)];

    printf("[%-40s] min=%6.2fμs avg=%6.2fμs p99=%6.2fμs max=%6.2fμs (%zu iters)\n",
           name, r.min_us, r.avg_us, r.p99_us, r.max_us, iters);

    return r;
}

// ============================================================================
// 1. SQL 파싱 벤치마크
// ============================================================================
void bench_sql_parse() {
    printf("\n=== SQL Parse Benchmarks ===\n");

    const std::vector<std::pair<const char*, std::string>> queries = {
        {"simple_select",
         "SELECT price, volume FROM trades WHERE symbol = 1"},
        {"aggregate",
         "SELECT count(*), sum(volume), avg(price) FROM trades WHERE symbol = 1"},
        {"group_by",
         "SELECT symbol, sum(volume) FROM trades GROUP BY symbol"},
        {"asof_join",
         "SELECT t.price, t.volume, q.bid, q.ask "
         "FROM trades t ASOF JOIN quotes q "
         "ON t.symbol = q.symbol AND t.timestamp >= q.timestamp"},
        {"between",
         "SELECT * FROM trades WHERE symbol = 1 AND timestamp BETWEEN 1000 AND 2000"},
        {"complex",
         "SELECT t.price, t.volume, q.bid FROM trades t "
         "JOIN quotes q ON t.symbol = q.symbol "
         "WHERE t.symbol = 1 AND t.price > 15000 "
         "ORDER BY t.timestamp DESC LIMIT 100"},
    };

    for (const auto& [name, sql] : queries) {
        bench(name, 10000, [&]() {
            Parser p;
            volatile auto stmt = p.parse(sql);
            (void)stmt;
        });
    }
}

// ============================================================================
// 2. SQL 실행 오버헤드 벤치마크
// ============================================================================
void bench_sql_execute() {
    printf("\n=== SQL Execute vs Direct API ===\n");

    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ApexPipeline pipeline(cfg);
    pipeline.start();

    // 데이터 삽입: 100K 행
    constexpr size_t N = 100'000;
    for (size_t i = 0; i < N; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1;
        msg.recv_ts   = static_cast<int64_t>(i) * 1000;
        msg.price     = 15000 + static_cast<int64_t>(i % 1000);
        msg.volume    = 100 + static_cast<int64_t>(i % 100);
        msg.msg_type  = 0;
        pipeline.ingest_tick(msg);
    }
    pipeline.drain_sync(N + 100);

    QueryExecutor executor(pipeline);
    size_t stored = pipeline.total_stored_rows();
    printf("Stored rows: %zu\n", stored);

    // SQL VWAP
    bench("sql_vwap", 1000, [&]() {
        auto r = executor.execute(
            "SELECT VWAP(price, volume) FROM trades WHERE symbol = 1");
        (void)r;
    });

    // SQL COUNT
    bench("sql_count", 1000, [&]() {
        auto r = executor.execute("SELECT count(*) FROM trades");
        (void)r;
    });

    // SQL SUM
    bench("sql_sum_volume", 1000, [&]() {
        auto r = executor.execute(
            "SELECT sum(volume) FROM trades WHERE symbol = 1");
        (void)r;
    });

    // SQL filter + select
    bench("sql_filter_price_gt", 1000, [&]() {
        auto r = executor.execute(
            "SELECT price, volume FROM trades WHERE symbol = 1 AND price > 15500");
        (void)r;
    });

    // 직접 C++ API (비교 기준)
    bench("direct_vwap", 1000, [&]() {
        auto r = pipeline.query_vwap(1, 0, INT64_MAX);
        (void)r;
    });

    bench("direct_count", 1000, [&]() {
        auto r = pipeline.query_count(1, 0, INT64_MAX);
        (void)r;
    });

    pipeline.stop();
}

// ============================================================================
// 3. ASOF JOIN 성능 벤치마크
// ============================================================================
void bench_asof_join() {
    printf("\n=== ASOF JOIN Performance ===\n");

    const std::vector<size_t> sizes = {1000, 10000, 100000, 1000000};

    for (size_t N : sizes) {
        ArenaAllocator arena_l(ArenaConfig{
            .total_size = N * 32 + (1 << 20),
            .use_hugepages = false,
            .numa_node = -1
        });
        ArenaAllocator arena_r(ArenaConfig{
            .total_size = N * 32 + (1 << 20),
            .use_hugepages = false,
            .numa_node = -1
        });

        ColumnVector lk("symbol", ColumnType::INT64, arena_l);
        ColumnVector lt("timestamp", ColumnType::INT64, arena_l);
        ColumnVector rk("symbol", ColumnType::INT64, arena_r);
        ColumnVector rt("timestamp", ColumnType::INT64, arena_r);

        // 데이터 생성
        for (size_t i = 0; i < N; ++i) {
            lk.append<int64_t>(1);
            lt.append<int64_t>(static_cast<int64_t>(i) * 100 + 50);
            rk.append<int64_t>(1);
            rt.append<int64_t>(static_cast<int64_t>(i) * 100);
        }

        char name[64];
        snprintf(name, sizeof(name), "asof_join_N=%zu", N);

        bench(name, std::max(1ul, 10000000 / N), [&]() {
            AsofJoinOperator asof;
            auto r = asof.execute(lk, rk, &lt, &rt);
            (void)r;
        });
    }
}

// ============================================================================
// 4. 파싱 시간 < 50μs 검증
// ============================================================================
void verify_parse_budget() {
    printf("\n=== Parse Budget Verification (<50μs) ===\n");

    std::vector<std::string> queries = {
        "SELECT price, volume FROM trades WHERE symbol = 1 AND price > 15000",
        "SELECT count(*), sum(volume), avg(price) FROM trades WHERE symbol = 1",
        "SELECT t.price, t.volume, q.bid, q.ask FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp",
    };

    for (const auto& sql : queries) {
        auto r = bench("parse", 10000, [&]() {
            Parser p;
            auto stmt = p.parse(sql);
            (void)stmt;
        });
        bool ok = r.avg_us < 50.0;
        printf("  avg=%.2fμs → %s (< 50μs)\n", r.avg_us, ok ? "PASS ✓" : "FAIL ✗");
    }
}

// ============================================================================
// 메인
// ============================================================================

// ============================================================================
// 5. Hash Join 벤치마크
// ============================================================================
void bench_hash_join() {
    printf("\n=== Hash Join Benchmarks ===\n");

    for (size_t N : {1000, 10000, 100000, 1000000}) {
        ArenaAllocator arena_l(ArenaConfig{
            .total_size = static_cast<size_t>(N * 8 * 2),
            .use_hugepages = false, .numa_node = -1});
        ArenaAllocator arena_r(ArenaConfig{
            .total_size = static_cast<size_t>(N * 8 * 2),
            .use_hugepages = false, .numa_node = -1});

        ColumnVector lk("k", ColumnType::INT64, arena_l);
        ColumnVector rk("k", ColumnType::INT64, arena_r);

        for (size_t i = 0; i < N; ++i) {
            lk.append<int64_t>(static_cast<int64_t>(i % (N / 10 + 1)));
        }
        for (size_t i = 0; i < N; ++i) {
            rk.append<int64_t>(static_cast<int64_t>(i % (N / 10 + 1)));
        }

        char name[64];
        snprintf(name, sizeof(name), "hash_join_N=%zu", N);

        size_t iters = std::max(1ul, 1000000 / N);
        bench(name, iters, [&]() {
            HashJoinOperator hj;
            auto r = hj.execute(lk, rk);
            (void)r;
        });
    }
}

// ============================================================================
// 6. Window SUM 벤치마크
// ============================================================================
void bench_window_sum() {
    printf("\n=== Window SUM Benchmarks ===\n");

    for (size_t N : {1000, 10000, 100000, 1000000}) {
        std::vector<int64_t> input(N, 100);
        std::vector<int64_t> output(N, 0);
        WindowFrame frame;
        frame.preceding = 19; // ROWS 20 PRECEDING
        frame.following = 0;

        char name[64];
        snprintf(name, sizeof(name), "window_sum_N=%zu", N);

        size_t iters = std::max(1ul, 10000000 / N);
        bench(name, iters, [&]() {
            WindowSum wf;
            wf.compute(input.data(), N, output.data(), frame);
        });
    }
}

// ============================================================================
// 7. Rolling AVG 비교: window function vs manual loop
// ============================================================================
void bench_rolling_avg_compare() {
    printf("\n=== Rolling AVG: Window Function vs Manual Loop ===\n");

    const size_t N = 100000;
    const int64_t W = 20; // 20-period
    std::vector<int64_t> input(N);
    std::vector<int64_t> output(N, 0);
    for (size_t i = 0; i < N; ++i) input[i] = static_cast<int64_t>(i % 1000);

    // Window function (O(n))
    WindowFrame frame;
    frame.preceding = W - 1;
    frame.following = 0;
    bench("rolling_avg_window_O(n)", 100, [&]() {
        WindowAvg wf;
        wf.compute(input.data(), N, output.data(), frame);
    });

    // Manual loop (O(n*W))
    std::fill(output.begin(), output.end(), 0);
    bench("rolling_avg_manual_O(n*W)", 100, [&]() {
        for (size_t i = 0; i < N; ++i) {
            int64_t sum = 0;
            int64_t cnt = 0;
            int64_t start = std::max<int64_t>(0, static_cast<int64_t>(i) - (W - 1));
            for (int64_t j = start; j <= static_cast<int64_t>(i); ++j) {
                sum += input[j];
                cnt++;
            }
            output[i] = sum / cnt;
        }
    });
}

// ============================================================================
// 메인
// ============================================================================
int main() {
    printf("APEX-DB SQL Benchmark Suite\n");
    printf("============================\n");

    bench_sql_parse();
    bench_sql_execute();
    bench_asof_join();
    verify_parse_budget();
    bench_hash_join();
    bench_window_sum();
    bench_rolling_avg_compare();

    return 0;
}
