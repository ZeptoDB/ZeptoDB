// ============================================================================
// Test: DuckDB Embedded Engine + Arrow Bridge
// ============================================================================

#ifdef ZEPTO_ENABLE_DUCKDB

#include "zeptodb/execution/duckdb_engine.h"
#include "zeptodb/execution/arrow_bridge.h"
#include "zeptodb/storage/arena_allocator.h"
#include "zeptodb/storage/partition_manager.h"
#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <filesystem>

using namespace zeptodb::execution;
using namespace zeptodb::storage;

// ============================================================================
// DuckDBEngine Tests
// ============================================================================

class DuckDBEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        DuckDBConfig cfg;
        cfg.memory_limit_mb = 64;
        cfg.threads = 1;
        engine_ = std::make_unique<DuckDBEngine>(cfg);
    }
    std::unique_ptr<DuckDBEngine> engine_;
};

TEST_F(DuckDBEngineTest, InitializesSuccessfully) {
    EXPECT_TRUE(engine_->is_initialized());
}

TEST_F(DuckDBEngineTest, DefaultConfigInitializes) {
    DuckDBEngine engine;
    EXPECT_TRUE(engine.is_initialized());
}

TEST_F(DuckDBEngineTest, ExecuteSimpleSelect) {
    auto result = engine_->execute("SELECT 1 AS val, 2 AS val2");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.num_columns, 2);
    EXPECT_EQ(result.num_rows, 1);
    EXPECT_EQ(result.column_names[0], "val");
    EXPECT_EQ(result.column_names[1], "val2");
}

TEST_F(DuckDBEngineTest, ExecuteGenerateSeries) {
    auto result = engine_->execute("SELECT * FROM generate_series(1, 100)");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.num_rows, 100);
}

TEST_F(DuckDBEngineTest, ExecuteInvalidSQL) {
    auto result = engine_->execute("SELEC INVALID GARBAGE");
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST_F(DuckDBEngineTest, RegisterParquetMissingFile) {
    bool ok = engine_->register_parquet("missing", "/nonexistent/path.parquet");
    EXPECT_FALSE(ok);
}

TEST_F(DuckDBEngineTest, RegisterAndQueryParquet) {
    // Create a parquet file via DuckDB itself, then register and query it
    auto tmp = std::filesystem::temp_directory_path() / "zepto_test.parquet";
    engine_->execute("COPY (SELECT i AS id, i*1.5 AS val FROM range(10) t(i)) "
                     "TO '" + tmp.string() + "' (FORMAT PARQUET)");

    ASSERT_TRUE(engine_->register_parquet("test_tbl", tmp.string()));
    auto result = engine_->execute("SELECT * FROM test_tbl");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.num_rows, 10);
    EXPECT_EQ(result.num_columns, 2);

    std::filesystem::remove(tmp);
}

TEST_F(DuckDBEngineTest, MemoryLimitEnforced) {
    // Engine was created with 64MB limit — verify it doesn't crash
    // on a moderately large query
    auto result = engine_->execute(
        "SELECT COUNT(*) FROM generate_series(1, 1000000)");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.num_rows, 1);
}

TEST_F(DuckDBEngineTest, EmptyResultSet) {
    auto result = engine_->execute(
        "SELECT 1 AS x WHERE false");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.num_rows, 0);
    EXPECT_EQ(result.num_columns, 1);
}

// ============================================================================
// Arrow Bridge Tests
// ============================================================================

class ArrowBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ArenaConfig cfg{.total_size = 1024 * 1024, .use_hugepages = false};
        arena_ = std::make_unique<ArenaAllocator>(cfg);
    }

    std::unique_ptr<Partition> make_partition(size_t nrows) {
        PartitionKey key{.symbol_id = 1, .hour_epoch = 0};
        auto part = std::make_unique<Partition>(key, std::move(arena_));

        auto& ts_col = part->add_column("timestamp", ColumnType::TIMESTAMP_NS);
        auto& price_col = part->add_column("price", ColumnType::FLOAT64);
        auto& vol_col = part->add_column("volume", ColumnType::INT64);

        for (size_t i = 0; i < nrows; ++i) {
            ts_col.append<int64_t>(static_cast<int64_t>(i) * 1000000000LL);
            price_col.append<double>(100.0 + i * 0.5);
            vol_col.append<int64_t>(static_cast<int64_t>(i * 100));
        }
        return part;
    }

    std::unique_ptr<ArenaAllocator> arena_;
};

TEST_F(ArrowBridgeTest, ColumnsToArrowDataInt64) {
    auto part = make_partition(5);
    auto cols = columns_to_arrow_data(*part, {"volume"});
    ASSERT_EQ(cols.size(), 1);
    EXPECT_EQ(cols[0].name, "volume");
    EXPECT_EQ(cols[0].type, ColumnType::INT64);
    ASSERT_EQ(cols[0].int_values.size(), 5);
    EXPECT_EQ(cols[0].int_values[0], 0);
    EXPECT_EQ(cols[0].int_values[4], 400);
}

TEST_F(ArrowBridgeTest, ColumnsToArrowDataFloat64) {
    auto part = make_partition(3);
    auto cols = columns_to_arrow_data(*part, {"price"});
    ASSERT_EQ(cols.size(), 1);
    EXPECT_EQ(cols[0].type, ColumnType::FLOAT64);
    ASSERT_EQ(cols[0].dbl_values.size(), 3);
    EXPECT_DOUBLE_EQ(cols[0].dbl_values[0], 100.0);
    EXPECT_DOUBLE_EQ(cols[0].dbl_values[2], 101.0);
}

TEST_F(ArrowBridgeTest, ColumnsToArrowDataTimestamp) {
    auto part = make_partition(2);
    auto cols = columns_to_arrow_data(*part, {"timestamp"});
    ASSERT_EQ(cols.size(), 1);
    EXPECT_EQ(cols[0].type, ColumnType::TIMESTAMP_NS);
    ASSERT_EQ(cols[0].int_values.size(), 2);
    EXPECT_EQ(cols[0].int_values[0], 0);
    EXPECT_EQ(cols[0].int_values[1], 1000000000LL);
}

TEST_F(ArrowBridgeTest, ColumnsToArrowDataMultipleColumns) {
    auto part = make_partition(4);
    auto cols = columns_to_arrow_data(*part, {"timestamp", "price", "volume"});
    ASSERT_EQ(cols.size(), 3);
    EXPECT_EQ(cols[0].name, "timestamp");
    EXPECT_EQ(cols[1].name, "price");
    EXPECT_EQ(cols[2].name, "volume");
}

TEST_F(ArrowBridgeTest, ColumnsToArrowDataMissingColumn) {
    auto part = make_partition(3);
    auto cols = columns_to_arrow_data(*part, {"nonexistent"});
    EXPECT_EQ(cols.size(), 0);
}

TEST_F(ArrowBridgeTest, ColumnsToArrowDataEmptyPartition) {
    auto part = make_partition(0);
    auto cols = columns_to_arrow_data(*part, {"volume"});
    ASSERT_EQ(cols.size(), 1);
    EXPECT_EQ(cols[0].int_values.size(), 0);
}

TEST_F(ArrowBridgeTest, ArrowColumnsToResultInt64) {
    ArrowColumnData col;
    col.name = "id";
    col.type = ColumnType::INT64;
    col.int_values = {10, 20, 30};

    auto result = arrow_columns_to_result({col}, 3);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.rows_scanned, 3);
    EXPECT_EQ(result.column_names.size(), 1);
    EXPECT_EQ(result.column_names[0], "id");
    ASSERT_EQ(result.rows.size(), 3);
    EXPECT_EQ(result.rows[0][0], 10);
    EXPECT_EQ(result.rows[2][0], 30);
}

TEST_F(ArrowBridgeTest, ArrowColumnsToResultFloat64) {
    ArrowColumnData col;
    col.name = "price";
    col.type = ColumnType::FLOAT64;
    col.dbl_values = {1.5, 2.5, 3.5};

    auto result = arrow_columns_to_result({col}, 3);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.typed_rows.size(), 3);
    EXPECT_DOUBLE_EQ(result.typed_rows[0][0].f, 1.5);
    EXPECT_DOUBLE_EQ(result.typed_rows[2][0].f, 3.5);
    // rows (int64 view) should contain bit_cast values
    EXPECT_EQ(result.rows[0][0], std::bit_cast<int64_t>(1.5));
}

TEST_F(ArrowBridgeTest, ArrowColumnsToResultEmpty) {
    auto result = arrow_columns_to_result({}, 0);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.rows_scanned, 0);
    EXPECT_TRUE(result.rows.empty());
    EXPECT_TRUE(result.typed_rows.empty());
}

TEST_F(ArrowBridgeTest, ArrowColumnsToResultMixed) {
    ArrowColumnData int_col;
    int_col.name = "ts";
    int_col.type = ColumnType::TIMESTAMP_NS;
    int_col.int_values = {1000, 2000};

    ArrowColumnData dbl_col;
    dbl_col.name = "val";
    dbl_col.type = ColumnType::FLOAT64;
    dbl_col.dbl_values = {99.9, 100.1};

    auto result = arrow_columns_to_result({int_col, dbl_col}, 2);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.column_names.size(), 2);
    ASSERT_EQ(result.typed_rows.size(), 2);
    EXPECT_EQ(result.typed_rows[0][0].i, 1000);
    EXPECT_DOUBLE_EQ(result.typed_rows[1][1].f, 100.1);
}

// ============================================================================
// Int32/Float32/Symbol column type conversion
// ============================================================================

class ArrowBridgeTypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ArenaConfig cfg{.total_size = 1024 * 1024, .use_hugepages = false};
        arena_ = std::make_unique<ArenaAllocator>(cfg);
    }
    std::unique_ptr<ArenaAllocator> arena_;
};

TEST_F(ArrowBridgeTypeTest, Int32ColumnPromotion) {
    PartitionKey key{.symbol_id = 1, .hour_epoch = 0};
    auto part = std::make_unique<Partition>(key, std::move(arena_));
    auto& col = part->add_column("qty", ColumnType::INT32);
    col.append<int32_t>(42);
    col.append<int32_t>(-7);

    auto arrow = columns_to_arrow_data(*part, {"qty"});
    ASSERT_EQ(arrow.size(), 1);
    // INT32 is promoted to int64 in int_values
    ASSERT_EQ(arrow[0].int_values.size(), 2);
    EXPECT_EQ(arrow[0].int_values[0], 42);
    EXPECT_EQ(arrow[0].int_values[1], -7);
}

TEST_F(ArrowBridgeTypeTest, Float32ColumnPromotion) {
    PartitionKey key{.symbol_id = 2, .hour_epoch = 0};
    auto part = std::make_unique<Partition>(key, std::move(arena_));
    auto& col = part->add_column("ratio", ColumnType::FLOAT32);
    col.append<float>(1.25f);
    col.append<float>(3.75f);

    auto arrow = columns_to_arrow_data(*part, {"ratio"});
    ASSERT_EQ(arrow.size(), 1);
    // FLOAT32 is promoted to double in dbl_values
    ASSERT_EQ(arrow[0].dbl_values.size(), 2);
    EXPECT_NEAR(arrow[0].dbl_values[0], 1.25, 1e-5);
    EXPECT_NEAR(arrow[0].dbl_values[1], 3.75, 1e-5);
}

#endif // ZEPTO_ENABLE_DUCKDB
