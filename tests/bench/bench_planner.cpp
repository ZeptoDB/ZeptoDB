// ============================================================================
// ZeptoDB: Cost-Based Planner Overhead Benchmark
// ============================================================================
// Measures the overhead introduced by the cost-based planner (Phase 1-6).
// The planner is observation-only — it builds LogicalPlan → PhysicalPlan
// but does NOT change actual execution. This benchmark quantifies:
//   1. Zero-overhead on simple queries (no JOIN/CTE/subquery/set-op)
//   2. Planning overhead on complex queries (JOIN, CTE, subquery, set-op)
//   3. EXPLAIN output comparison (text vs tree)
//   4. TableStatistics collection cost
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/execution/query_planner.h"
#include "zeptodb/execution/table_statistics.h"
#include "zeptodb/execution/cost_model.h"
#include "zeptodb/core/pipeline.h"

#include <chrono>
#include <cstdio>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>

using namespace zeptodb::sql;
using namespace zeptodb::execution;
using namespace zeptodb::storage;
using namespace zeptodb::core;

using hrclock = std::chrono::high_resolution_clock;

static double elapsed_us(hrclock::time_point start) {
    return std::chrono::duration<double, std::micro>(hrclock::now() - start).count();
}

struct BenchResult {
    double min_us, max_us, avg_us, p50_us, p99_us;
    size_t iterations;
};

template <typename Fn>
BenchResult bench(const char* name, size_t iters, Fn&& fn) {
    std::vector<double> times;
    times.reserve(iters);
    for (int w = 0; w < 5; ++w) fn();  // warmup
    for (size_t i = 0; i < iters; ++i) {
        auto t0 = hrclock::now();
        fn();
        times.push_back(elapsed_us(t0));
    }
    std::sort(times.begin(), times.end());
    BenchResult r;
    r.iterations = iters;
    r.min_us  = times.front();
    r.max_us  = times.back();
    r.avg_us  = std::accumulate(times.begin(), times.end(), 0.0) / iters;
    r.p50_us  = times[iters / 2];
    r.p99_us  = times[static_cast<size_t>(iters * 0.99)];
    printf("  %-50s  min=%7.1f  avg=%7.1f  p50=%7.1f  p99=%7.1f  max=%7.1f μs  (%zu iters)\n",
           name, r.min_us, r.avg_us, r.p50_us, r.p99_us, r.max_us, iters);
    return r;
}

// ============================================================================
// Setup: create pipeline with trades + quotes tables
// ============================================================================
struct TestEnv {
    PipelineConfig cfg;
    std::unique_ptr<ZeptoPipeline> pipeline;
    std::unique_ptr<QueryExecutor> executor;
    size_t trade_rows = 0;
    size_t quote_rows = 0;
};

static TestEnv setup(size_t n_symbols, size_t rows_per_symbol) {
    TestEnv env;
    env.cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    env.pipeline = std::make_unique<ZeptoPipeline>(env.cfg);
    env.pipeline->start();

    // trades: symbol 1..n_symbols
    for (size_t s = 1; s <= n_symbols; ++s) {
        for (size_t i = 0; i < rows_per_symbol; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = static_cast<zeptodb::SymbolId>(s);
            msg.recv_ts   = static_cast<int64_t>(i) * 1000;
            msg.price     = 15000 + static_cast<int64_t>(i % 1000);
            msg.volume    = 100 + static_cast<int64_t>(i % 100);
            msg.msg_type  = 0;  // trade
            env.pipeline->ingest_tick(msg);
        }
    }
    // quotes: same symbols, offset timestamps
    for (size_t s = 1; s <= n_symbols; ++s) {
        for (size_t i = 0; i < rows_per_symbol; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = static_cast<zeptodb::SymbolId>(s);
            msg.recv_ts   = static_cast<int64_t>(i) * 1000 + 10;
            msg.price     = 14990 + static_cast<int64_t>(i % 1000);  // bid
            msg.volume    = 15010 + static_cast<int64_t>(i % 1000);  // ask (stored in volume)
            msg.msg_type  = 1;  // quote
            env.pipeline->ingest_tick(msg);
        }
    }

    size_t total = n_symbols * rows_per_symbol * 2;
    env.pipeline->drain_sync(total + 200);
    env.trade_rows = n_symbols * rows_per_symbol;
    env.quote_rows = n_symbols * rows_per_symbol;
    env.executor = std::make_unique<QueryExecutor>(*env.pipeline);
    return env;
}

// ============================================================================
// 1. Simple queries — should have ZERO planner overhead
// ============================================================================
static void bench_simple_queries(TestEnv& env) {
    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  1. SIMPLE QUERIES (no planner activation expected)\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    printf("  Stored: %zu trade rows, %zu quote rows\n\n", env.trade_rows, env.quote_rows);

    bench("SELECT count(*) FROM trades", 2000, [&]() {
        auto r = env.executor->execute("SELECT count(*) FROM trades");
    });

    bench("SELECT price,volume WHERE symbol=1", 2000, [&]() {
        auto r = env.executor->execute(
            "SELECT price, volume FROM trades WHERE symbol = 1");
    });

    bench("SELECT sum(volume) GROUP BY symbol", 2000, [&]() {
        auto r = env.executor->execute(
            "SELECT symbol, sum(volume) FROM trades GROUP BY symbol");
    });

    bench("SELECT avg(price) WHERE symbol=1 ORDER BY ts", 2000, [&]() {
        auto r = env.executor->execute(
            "SELECT avg(price) FROM trades WHERE symbol = 1");
    });

    bench("SELECT VWAP(price,volume) WHERE symbol=1", 2000, [&]() {
        auto r = env.executor->execute(
            "SELECT VWAP(price, volume) FROM trades WHERE symbol = 1");
    });
}

// ============================================================================
// 2. Complex queries — planner activates (JOIN, CTE, subquery, set-op)
// ============================================================================
static void bench_complex_queries(TestEnv& env) {
    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  2. COMPLEX QUERIES (planner activates: JOIN/CTE/subquery/set-op)\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    bench("ASOF JOIN trades/quotes", 500, [&]() {
        auto r = env.executor->execute(
            "SELECT t.price, t.volume, q.price AS bid "
            "FROM trades t ASOF JOIN quotes q "
            "ON t.symbol = q.symbol AND t.timestamp >= q.timestamp "
            "WHERE t.symbol = 1");
    });

    bench("HASH JOIN trades/quotes ON symbol", 500, [&]() {
        auto r = env.executor->execute(
            "SELECT t.price, q.price AS bid "
            "FROM trades t JOIN quotes q ON t.symbol = q.symbol "
            "WHERE t.symbol = 1");
    });

    bench("CTE + aggregate", 500, [&]() {
        auto r = env.executor->execute(
            "WITH sym_vol AS ("
            "  SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol"
            ") SELECT * FROM sym_vol ORDER BY total_vol DESC LIMIT 5");
    });

    bench("Scalar subquery in WHERE", 500, [&]() {
        auto r = env.executor->execute(
            "SELECT price, volume FROM trades "
            "WHERE symbol = 1 AND price > (SELECT avg(price) FROM trades WHERE symbol = 1)");
    });

    bench("UNION ALL two selects", 500, [&]() {
        auto r = env.executor->execute(
            "SELECT symbol, price FROM trades WHERE symbol = 1 "
            "UNION ALL "
            "SELECT symbol, price FROM trades WHERE symbol = 2");
    });
}

// ============================================================================
// 3. Planner-only overhead: isolate LogicalPlan + PhysicalPlan build time
// ============================================================================
static void bench_planner_only(TestEnv& env) {
    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  3. PLANNER-ONLY OVERHEAD (LogicalPlan build + optimize + PhysicalPlan)\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    // Pre-populate statistics so we measure pure planning time
    TableStatistics stats;
    for (auto* p : env.pipeline->partition_manager().get_all_partitions()) {
        stats.update_partition(p);
    }

    auto measure_plan = [&](const char* name, const std::string& sql) {
        Parser parser;
        auto stmt = parser.parse(sql);

        bench(name, 10000, [&]() {
            auto logical = LogicalPlan::build(stmt);
            LogicalPlan::optimize(logical);
            auto physical = PhysicalPlanner::plan(logical, stats);
        });
    };

    measure_plan("plan: simple SELECT (no-op check)",
        "SELECT price FROM trades WHERE symbol = 1");

    measure_plan("plan: ASOF JOIN",
        "SELECT t.price, q.price FROM trades t ASOF JOIN quotes q "
        "ON t.symbol = q.symbol AND t.timestamp >= q.timestamp");

    measure_plan("plan: HASH JOIN",
        "SELECT t.price, q.price FROM trades t JOIN quotes q ON t.symbol = q.symbol");

    measure_plan("plan: JOIN + WHERE + ORDER + LIMIT",
        "SELECT t.price, q.price FROM trades t JOIN quotes q ON t.symbol = q.symbol "
        "WHERE t.symbol = 1 ORDER BY t.timestamp DESC LIMIT 100");

    measure_plan("plan: GROUP BY + ORDER BY",
        "SELECT symbol, sum(volume) FROM trades GROUP BY symbol ORDER BY sum(volume) DESC");
}

// ============================================================================
// 4. TableStatistics collection cost
// ============================================================================
static void bench_stats_collection(TestEnv& env) {
    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  4. TABLE STATISTICS COLLECTION COST\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    auto partitions = env.pipeline->partition_manager().get_all_partitions();
    printf("  Partitions: %zu\n\n", partitions.size());

    // Full stats rebuild
    bench("stats: full rebuild (all partitions)", 200, [&]() {
        TableStatistics stats;
        for (auto* p : partitions) stats.update_partition(p);
    });

    // Incremental (already tracked — should be near-zero)
    TableStatistics pre_stats;
    for (auto* p : partitions) pre_stats.update_partition(p);

    bench("stats: incremental (all tracked, skip)", 2000, [&]() {
        for (auto* p : partitions) {
            if (pre_stats.partition_stats().find(p) == pre_stats.partition_stats().end())
                pre_stats.update_partition(p);
        }
    });
}

// ============================================================================
// 5. EXPLAIN output comparison
// ============================================================================
static void bench_explain(TestEnv& env) {
    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  5. EXPLAIN OUTPUT COMPARISON\n");
    printf("══════════════════════════════════════════════════════════════════\n\n");

    // Simple query EXPLAIN (text-based, no planner)
    auto r1 = env.executor->execute(
        "EXPLAIN SELECT price, volume FROM trades WHERE symbol = 1");
    printf("  --- Simple EXPLAIN (text-based) ---\n");
    for (auto& row : r1.string_rows) printf("    %s\n", row.c_str());

    printf("\n");

    // Complex query EXPLAIN (tree-based, with planner)
    auto r2 = env.executor->execute(
        "EXPLAIN SELECT t.price, q.price AS bid "
        "FROM trades t ASOF JOIN quotes q "
        "ON t.symbol = q.symbol AND t.timestamp >= q.timestamp "
        "WHERE t.symbol = 1");
    printf("  --- Complex EXPLAIN (cost-based tree) ---\n");
    for (auto& row : r2.string_rows) printf("    %s\n", row.c_str());

    printf("\n");

    // EXPLAIN with JOIN + ORDER + LIMIT
    auto r3 = env.executor->execute(
        "EXPLAIN SELECT t.price, q.price "
        "FROM trades t JOIN quotes q ON t.symbol = q.symbol "
        "WHERE t.symbol = 1 ORDER BY t.price DESC LIMIT 10");
    printf("  --- JOIN + ORDER + LIMIT EXPLAIN ---\n");
    for (auto& row : r3.string_rows) printf("    %s\n", row.c_str());

    printf("\n");

    // Timing comparison
    bench("EXPLAIN simple (text path)", 2000, [&]() {
        auto r = env.executor->execute(
            "EXPLAIN SELECT count(*) FROM trades WHERE symbol = 1");
    });

    bench("EXPLAIN complex (planner path)", 2000, [&]() {
        auto r = env.executor->execute(
            "EXPLAIN SELECT t.price, q.price "
            "FROM trades t ASOF JOIN quotes q "
            "ON t.symbol = q.symbol AND t.timestamp >= q.timestamp "
            "WHERE t.symbol = 1");
    });
}

// ============================================================================
int main() {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  ZeptoDB Cost-Based Planner Benchmark                          ║\n");
    printf("║  Measures planning overhead on simple vs complex queries       ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    // Setup: 2 symbols × 5K rows = 10K trades + 10K quotes
    printf("\nSetting up: 2 symbols × 5K rows = 10K trades + 10K quotes...\n");
    auto env = setup(2, 5000);
    printf("Ready: %zu total stored rows\n", env.pipeline->total_stored_rows());

    bench_simple_queries(env);
    bench_complex_queries(env);
    bench_planner_only(env);
    bench_stats_collection(env);
    bench_explain(env);

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  DONE\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    env.executor.reset();
    env.pipeline->stop();
    return 0;
}
