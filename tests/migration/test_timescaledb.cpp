// ============================================================================
// ZeptoDB: TimescaleDB Migration Tests
// ============================================================================
#include "zeptodb/migration/timescaledb_migrator.h"
#include <gtest/gtest.h>

using namespace zeptodb::migration;

// ============================================================================
// Type Mapper Tests
// ============================================================================

TEST(TSDBTypeMapperTest, APEXTypes) {
    EXPECT_EQ(ZeptoToTSDBTypeMapper::zepto_to_pg("BIGINT"),    "BIGINT");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::zepto_to_pg("DOUBLE"),    "DOUBLE PRECISION");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::zepto_to_pg("VARCHAR"),   "TEXT");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::zepto_to_pg("TIMESTAMP"), "TIMESTAMPTZ");
}

TEST(TSDBTypeMapperTest, KDBTypes) {
    EXPECT_EQ(ZeptoToTSDBTypeMapper::ktype_to_pg(7),  "BIGINT");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::ktype_to_pg(9),  "DOUBLE PRECISION");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::ktype_to_pg(11), "TEXT");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::ktype_to_pg(12), "TIMESTAMPTZ");
    EXPECT_EQ(ZeptoToTSDBTypeMapper::ktype_to_pg(14), "DATE");
}

// ============================================================================
// Schema Generator Tests
// ============================================================================

TEST(TSDBSchemaGeneratorTest, GenerateTradesSchema) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    EXPECT_EQ(schema.name, "trades");
    EXPECT_EQ(schema.time_column, "timestamp");
    EXPECT_FALSE(schema.columns.empty());
    EXPECT_FALSE(schema.space_partitions.empty());
    EXPECT_FALSE(schema.continuous_aggregates.empty());
}

TEST(TSDBSchemaGeneratorTest, GenerateQuotesSchema) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_quotes_schema();

    EXPECT_EQ(schema.name, "quotes");

    bool has_bid = false, has_ask = false;
    for (const auto& col : schema.columns) {
        if (col.name == "bid") has_bid = true;
        if (col.name == "ask") has_ask = true;
    }
    EXPECT_TRUE(has_bid);
    EXPECT_TRUE(has_ask);
}

TEST(TSDBSchemaGeneratorTest, GenerateFromKDB) {
    TimescaleDBSchemaGenerator gen;

    std::vector<std::pair<std::string, int8_t>> kdb_cols = {
        {"timestamp", 12},
        {"sym",       11},
        {"price",      9},
        {"size",       9},
    };

    auto schema = gen.from_kdb_schema("trades", kdb_cols);

    EXPECT_EQ(schema.name, "trades");
    EXPECT_EQ(schema.time_column, "timestamp");
    EXPECT_EQ(schema.columns.size(), 4);
    EXPECT_EQ(schema.space_partitions[0], "sym");
}

TEST(TSDBSchemaGeneratorTest, AddOHLCVAggregate) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();
    size_t initial_aggs = schema.continuous_aggregates.size();

    gen.add_ohlcv_aggregate(schema, "5 minutes");

    EXPECT_EQ(schema.continuous_aggregates.size(), initial_aggs + 1);
    EXPECT_NE(schema.continuous_aggregates.back().view_name.find("5"), std::string::npos);
}

// ============================================================================
// DDL Generation Tests
// ============================================================================

TEST(TSDBDDLTest, CreateTableSQL) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    std::string sql = schema.to_create_table_sql();

    EXPECT_NE(sql.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(sql.find("trades"), std::string::npos);
    EXPECT_NE(sql.find("TIMESTAMPTZ"), std::string::npos);
}

TEST(TSDBDDLTest, CreateHypertableSQL) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    std::string sql = schema.to_create_hypertable_sql();

    EXPECT_NE(sql.find("create_hypertable"), std::string::npos);
    EXPECT_NE(sql.find("trades"), std::string::npos);
    EXPECT_NE(sql.find("timestamp"), std::string::npos);
    EXPECT_NE(sql.find("chunk_time_interval"), std::string::npos);
}

TEST(TSDBDDLTest, CompressionSQL) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    std::string sql = schema.to_enable_compression_sql();

    EXPECT_NE(sql.find("timescaledb.compress"), std::string::npos);
    EXPECT_NE(sql.find("add_compression_policy"), std::string::npos);
    EXPECT_NE(sql.find("trades"), std::string::npos);
}

TEST(TSDBDDLTest, ContinuousAggregateSQL) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    ASSERT_FALSE(schema.continuous_aggregates.empty());
    std::string sql = schema.continuous_aggregates[0].to_create_sql();

    EXPECT_NE(sql.find("CREATE MATERIALIZED VIEW"), std::string::npos);
    EXPECT_NE(sql.find("timescaledb.continuous"), std::string::npos);
    EXPECT_NE(sql.find("time_bucket"), std::string::npos);
    EXPECT_NE(sql.find("GROUP BY"), std::string::npos);
}

TEST(TSDBDDLTest, FullSetupSQL) {
    TimescaleDBSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    std::string sql = schema.to_full_setup_sql();

    EXPECT_NE(sql.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(sql.find("create_hypertable"), std::string::npos);
    EXPECT_NE(sql.find("timescaledb.compress"), std::string::npos);
    EXPECT_NE(sql.find("CREATE MATERIALIZED VIEW"), std::string::npos);
}

// ============================================================================
// Query Translator Tests
// ============================================================================

TEST(TSDBQueryTranslatorTest, XbarToTimeBucket) {
    TimescaleDBQueryTranslator translator;

    std::string zepto_sql = "SELECT xbar(timestamp, 60), sym FROM trades";
    std::string tsdb_sql = translator.translate(zepto_sql);

    EXPECT_NE(tsdb_sql.find("time_bucket"), std::string::npos);
    EXPECT_NE(tsdb_sql.find("60"), std::string::npos);
}

TEST(TSDBQueryTranslatorTest, FirstLastRewrite) {
    TimescaleDBQueryTranslator translator;

    std::string zepto_sql = "SELECT FIRST(price), LAST(price) FROM trades";
    std::string tsdb_sql = translator.translate(zepto_sql);

    EXPECT_NE(tsdb_sql.find("first(price, timestamp)"), std::string::npos);
    EXPECT_NE(tsdb_sql.find("last(price, timestamp)"), std::string::npos);
}

TEST(TSDBQueryTranslatorTest, AsofJoin) {
    TimescaleDBQueryTranslator translator;
    std::string join = translator.translate_asof_join(
        "trades", "quotes", "sym", "timestamp");

    EXPECT_NE(join.find("LATERAL"), std::string::npos);
    EXPECT_NE(join.find("ORDER BY"), std::string::npos);
    EXPECT_NE(join.find("LIMIT 1"), std::string::npos);
}

TEST(TSDBQueryTranslatorTest, CandlestickExample) {
    TimescaleDBQueryTranslator translator;
    std::string example = translator.generate_candlestick_example("trades");

    EXPECT_NE(example.find("candlestick_agg"), std::string::npos);
    EXPECT_NE(example.find("time_bucket"), std::string::npos);
    EXPECT_NE(example.find("vwap"), std::string::npos);
}

TEST(TSDBQueryTranslatorTest, StatsAggExample) {
    TimescaleDBQueryTranslator translator;
    std::string example = translator.generate_stats_agg_example("trades");

    EXPECT_NE(example.find("stats_agg"), std::string::npos);
    EXPECT_NE(example.find("average"), std::string::npos);
    EXPECT_NE(example.find("stddev"), std::string::npos);
}

// ============================================================================
// Migration Runner Tests
// ============================================================================

TEST(TSDBMigratorTest, GenerateMigrationSQL) {
    TimescaleDBMigrator::Config config;
    config.tables = {"trades", "quotes"};
    config.create_continuous_aggregates = true;
    config.enable_compression = true;

    TimescaleDBMigrator migrator(config);
    std::string sql = migrator.generate_migration_sql();

    EXPECT_NE(sql.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(sql.find("create_hypertable"), std::string::npos);
    EXPECT_NE(sql.find("trades"), std::string::npos);
    EXPECT_NE(sql.find("quotes"), std::string::npos);
}

TEST(TSDBMigratorTest, GenerateSQLWithoutCompression) {
    TimescaleDBMigrator::Config config;
    config.tables = {"trades"};
    config.enable_compression = false;

    TimescaleDBMigrator migrator(config);
    std::string sql = migrator.generate_migration_sql();

    // Should still have basic structure
    EXPECT_NE(sql.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(sql.find("create_hypertable"), std::string::npos);
}

// ============================================================================
// Main
// ============================================================================

// main provided by test_q_to_sql.cpp
