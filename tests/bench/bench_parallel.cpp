// ============================================================================
// ZeptoDB: 병렬 쿼리 엔진 벤치마크
// ============================================================================
// 측정 항목:
//   - 스레드 수 1/2/4/8 별 SUM, COUNT, VWAP, GROUP BY 성능
//   - 단일 스레드 대비 speedup
//   - 1M / 10M rows 두 가지 규모
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/execution/query_scheduler.h"
#include "zeptodb/execution/local_scheduler.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace zeptodb::core;
using namespace zeptodb::sql;

// ============================================================================
// 타이머
// ============================================================================
static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// 데이터 생성
// ============================================================================
static std::unique_ptr<ZeptoPipeline> make_pipeline(size_t n_rows, size_t n_symbols = 8) {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    auto pipeline = std::make_unique<ZeptoPipeline>(cfg);

    for (size_t i = 0; i < n_rows; ++i) {
        zeptodb::ingestion::TickMessage msg{};
        msg.symbol_id = static_cast<zeptodb::SymbolId>((i % n_symbols) + 1);
        msg.recv_ts   = static_cast<int64_t>(1'000'000LL + i);
        msg.price     = 10'000LL + static_cast<int64_t>(i % 1000);
        msg.volume    = 100LL    + static_cast<int64_t>(i % 500);
        msg.msg_type  = 0;
        pipeline->ingest_tick(msg);
    }
    pipeline->drain_sync(static_cast<uint64_t>(n_rows + 1000));
    return pipeline;
}

// ============================================================================
// 단일 벤치마크: SQL 실행 + 시간 측정
// ============================================================================
static double bench_query(QueryExecutor& ex, const std::string& sql, int reps = 3) {
    double best = 1e18;
    for (int r = 0; r < reps; ++r) {
        double t0 = now_ms();
        auto result = ex.execute(sql);
        double elapsed = now_ms() - t0;
        if (!result.ok()) {
            std::printf("  ERROR: %s\n", result.error.c_str());
            return -1.0;
        }
        if (elapsed < best) best = elapsed;
    }
    return best;
}

// ============================================================================
// 스레드 수별 speedup 측정
// ============================================================================
static void bench_speedup(ZeptoPipeline& pipeline,
                          const std::string& label,
                          const std::string& sql,
                          size_t row_threshold = 1000)
{
    std::printf("\n  [%s]\n", label.c_str());
    std::printf("  %-12s %10s %10s\n", "Threads", "ms(best)", "Speedup");
    std::printf("  %-12s %10s %10s\n", "-------", "--------", "-------");

    double base_ms = -1.0;
    for (size_t n_threads : {1u, 2u, 4u, 8u}) {
        QueryExecutor ex(pipeline);
        ex.enable_parallel(n_threads, row_threshold);

        double ms = bench_query(ex, sql, 5);
        if (ms < 0) continue;

        if (n_threads == 1) base_ms = ms;
        double speedup = (base_ms > 0) ? base_ms / ms : 1.0;
        std::printf("  %-12zu %10.3f %10.2fx\n", n_threads, ms, speedup);
    }
}

// ============================================================================
// main
// ============================================================================
int main() {
    std::printf("=============================================================\n");
    std::printf("  ZeptoDB 병렬 쿼리 엔진 벤치마크\n");
    std::printf("=============================================================\n");

    for (size_t n_rows : {1'000'000UL, 5'000'000UL}) {
        std::printf("\n[데이터: %zuM rows, 8 symbols]\n", n_rows / 1'000'000UL);
        std::printf("  파이프라인 생성 중...\n");
        double t_build = now_ms();
        auto pipeline = make_pipeline(n_rows, 8);
        std::printf("  완료: %.1f ms\n", now_ms() - t_build);

        // --- SUM ---
        bench_speedup(*pipeline, "sum(volume) / symbol=1",
            "SELECT sum(volume) FROM trades WHERE symbol = 1",
            n_rows / 20);

        // --- COUNT ---
        bench_speedup(*pipeline, "count(*)",
            "SELECT count(*) FROM trades",
            n_rows / 20);

        // --- VWAP ---
        bench_speedup(*pipeline, "vwap(price, volume) / symbol=1",
            "SELECT vwap(price, volume) FROM trades WHERE symbol = 1",
            n_rows / 20);

        // --- GROUP BY ---
        bench_speedup(*pipeline, "GROUP BY symbol sum(volume)",
            "SELECT symbol, sum(volume), count(*) FROM trades GROUP BY symbol",
            n_rows / 20);
    }

    // ── LocalQueryScheduler scatter/gather 직접 벤치마크 ──
    std::printf("\n[LocalQueryScheduler scatter/gather API]\n");
    {
        size_t n_rows = 1'000'000;
        auto pipeline = make_pipeline(n_rows, 4);
        zeptodb::execution::LocalQueryScheduler sched(*pipeline, 4);

        // 빈 fragment 목록으로 scatter 성능 측정 (오버헤드)
        std::vector<zeptodb::execution::QueryFragment> frags(4);
        for (size_t i = 0; i < 4; ++i) {
            frags[i].table_name = "trades";
            frags[i].agg_types  = {zeptodb::execution::AggType::COUNT};
            frags[i].agg_columns = {""};
        }

        double t0 = now_ms();
        for (int r = 0; r < 10; ++r) {
            auto partials = sched.scatter(frags);
            auto result   = sched.gather(std::move(partials));
            (void)result;
        }
        std::printf("  scatter/gather 10 rounds: %.3f ms avg\n",
                    (now_ms() - t0) / 10.0);
    }

    std::printf("\n=============================================================\n");
    std::printf("  벤치마크 완료\n");
    std::printf("=============================================================\n");
    return 0;
}
