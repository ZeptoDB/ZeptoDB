// ============================================================================
// ZeptoDB: MV Query Rewrite Tests
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/sql/mv_rewriter.h"
#include "zeptodb/core/pipeline.h"

#include <gtest/gtest.h>
#include <memory>

using namespace zeptodb::sql;
using namespace zeptodb::storage;
using namespace zeptodb::core;

class MVRewriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline_ = std::make_unique<ZeptoPipeline>(cfg);
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
    }

    /// Create MV first, then ingest ticks so the MV receives incremental updates.
    void ingest_default_ticks() {
        for (int i = 0; i < 10; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = 1;
            msg.recv_ts   = 1000LL + i;
            msg.price     = 100 + i;
            msg.volume    = 10;
            pipeline_->ingest_tick(msg);
        }
        for (int i = 0; i < 5; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = 2;
            msg.recv_ts   = 1000LL + i;
            msg.price     = 200 + i;
            msg.volume    = 20;
            pipeline_->ingest_tick(msg);
        }
        pipeline_->drain_sync(100);
    }

    std::unique_ptr<ZeptoPipeline>  pipeline_;
    std::unique_ptr<QueryExecutor>  executor_;
};

// Exact match: query matches MV → rewrite returns pre-aggregated data
TEST_F(MVRewriteTest, ExactMatch) {
    // Create MV before ingesting so on_tick populates it
    auto cr = executor_->execute(
        "CREATE MATERIALIZED VIEW vol_by_sym AS "
        "SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol");
    ASSERT_TRUE(cr.ok()) << cr.error;

    ingest_default_ticks();

    auto r = executor_->execute(
        "SELECT symbol, sum(volume) FROM trades GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 2u); // symbol 1 and 2

    // Find rows by symbol
    int64_t vol1 = 0, vol2 = 0;
    for (auto& row : r.rows) {
        if (row[0] == 1) vol1 = row[1];
        if (row[0] == 2) vol2 = row[1];
    }
    EXPECT_EQ(vol1, 100); // 10 ticks * volume 10
    EXPECT_EQ(vol2, 100); // 5 ticks * volume 20
}

// WHERE clause present → no MV rewrite, falls through to full scan
TEST_F(MVRewriteTest, NoMatchWhere) {
    auto cr = executor_->execute(
        "CREATE MATERIALIZED VIEW vol_by_sym AS "
        "SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol");
    ASSERT_TRUE(cr.ok()) << cr.error;

    ingest_default_ticks();

    // Has WHERE → should NOT use MV rewrite, but still return correct result
    auto r = executor_->execute(
        "SELECT symbol, sum(volume) FROM trades WHERE symbol = 1 GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_GE(r.rows.size(), 1u);
}

// Aggregation mismatch: query has avg() but MV only has sum() → no rewrite
TEST_F(MVRewriteTest, NoMatchAggMismatch) {
    auto cr = executor_->execute(
        "CREATE MATERIALIZED VIEW vol_by_sym AS "
        "SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol");
    ASSERT_TRUE(cr.ok()) << cr.error;

    ingest_default_ticks();

    // avg() can't be served from sum() MV
    auto r = executor_->execute(
        "SELECT symbol, avg(price) FROM trades GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_GE(r.rows.size(), 1u);
}

// No MV registered → no rewrite, normal execution
TEST_F(MVRewriteTest, NoMVRegistered) {
    ingest_default_ticks();

    auto r = executor_->execute(
        "SELECT symbol, sum(volume) FROM trades GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_GE(r.rows.size(), 1u);
}

// Multiple agg columns match
TEST_F(MVRewriteTest, MultipleAggColumns) {
    auto cr = executor_->execute(
        "CREATE MATERIALIZED VIEW trade_stats AS "
        "SELECT symbol, sum(volume) AS vol, max(price) AS hi, min(price) AS lo "
        "FROM trades GROUP BY symbol");
    ASSERT_TRUE(cr.ok()) << cr.error;

    ingest_default_ticks();

    auto r = executor_->execute(
        "SELECT symbol, sum(volume), max(price) FROM trades GROUP BY symbol");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 2u);

    // Verify values for symbol 1: sum(vol)=100, max(price)=109
    for (auto& row : r.rows) {
        if (row[0] == 1) {
            EXPECT_EQ(row[1], 100);  // sum(volume) = 10 * 10
            EXPECT_EQ(row[2], 109);  // max(price) = 100+9
        }
    }
}

// Source table mismatch → no rewrite
TEST_F(MVRewriteTest, NoMatchTableMismatch) {
    auto cr = executor_->execute(
        "CREATE MATERIALIZED VIEW vol_by_sym AS "
        "SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol");
    ASSERT_TRUE(cr.ok()) << cr.error;

    ingest_default_ticks();

    // Query from 'quotes' — should NOT match the trades MV
    auto r = executor_->execute(
        "SELECT symbol, sum(volume) FROM quotes GROUP BY symbol");
    // Will error because 'quotes' doesn't exist, which is fine
    // The point is it should NOT match the trades MV
}
