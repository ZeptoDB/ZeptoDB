// ============================================================================
// APEX-DB: ClickHouse Migration Tests
// ============================================================================
#include "apex/migration/clickhouse_migrator.h"
#include <gtest/gtest.h>
#include <string>

using namespace apex::migration;

// ============================================================================
// Type Mapper Tests
// ============================================================================

TEST(ClickHouseTypeMapperTest, APEXTypes) {
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_apex_type("BIGINT"),    CHType::Int64);
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_apex_type("DOUBLE"),    CHType::Float64);
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_apex_type("VARCHAR"),   CHType::String);
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_apex_type("TIMESTAMP"), CHType::DateTime64);
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_apex_type("DATE"),      CHType::Date32);
}

TEST(ClickHouseTypeMapperTest, KDBTypes) {
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_ktype(7),  CHType::Int64);    // long
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_ktype(9),  CHType::Float64);  // float
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_ktype(11), CHType::LowCardinality); // sym
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_ktype(12), CHType::DateTime64); // timestamp
    EXPECT_EQ(APEXToClickHouseTypeMapper::map_ktype(14), CHType::Date32);   // date
}

TEST(ClickHouseTypeMapperTest, KDBTypeStrings) {
    EXPECT_EQ(APEXToClickHouseTypeMapper::ktype_to_ch_string(7),  "Int64");
    EXPECT_EQ(APEXToClickHouseTypeMapper::ktype_to_ch_string(9),  "Float64");
    EXPECT_EQ(APEXToClickHouseTypeMapper::ktype_to_ch_string(11), "LowCardinality(String)");
    EXPECT_EQ(APEXToClickHouseTypeMapper::ktype_to_ch_string(12), "DateTime64(9, 'UTC')");
}

// ============================================================================
// Column DDL Tests
// ============================================================================

TEST(ClickHouseColumnTest, BasicColumn) {
    CHColumn col;
    col.name = "price";
    col.type = CHType::Float64;

    std::string ddl = col.to_ddl();
    EXPECT_NE(ddl.find("price"), std::string::npos);
    EXPECT_NE(ddl.find("Float64"), std::string::npos);
}

TEST(ClickHouseColumnTest, NullableColumn) {
    CHColumn col;
    col.name = "condition";
    col.type = CHType::String;
    col.nullable = true;

    std::string ddl = col.to_ddl();
    EXPECT_NE(ddl.find("Nullable"), std::string::npos);
    EXPECT_NE(ddl.find("String"), std::string::npos);
}

TEST(ClickHouseColumnTest, LowCardinalityColumn) {
    CHColumn col;
    col.name = "sym";
    col.type = CHType::String;
    col.low_cardinality = true;

    std::string ddl = col.to_ddl();
    EXPECT_NE(ddl.find("LowCardinality"), std::string::npos);
}

TEST(ClickHouseColumnTest, DateTime64Column) {
    CHColumn col;
    col.name = "timestamp";
    col.type = CHType::DateTime64;
    col.precision = 9;

    std::string ddl = col.to_ddl();
    EXPECT_NE(ddl.find("DateTime64"), std::string::npos);
    EXPECT_NE(ddl.find("9"), std::string::npos);
}

TEST(ClickHouseColumnTest, ColumnWithCodec) {
    CHColumn col;
    col.name = "price";
    col.type = CHType::Float64;
    col.codec = "Gorilla, ZSTD(1)";

    std::string ddl = col.to_ddl();
    EXPECT_NE(ddl.find("CODEC"), std::string::npos);
    EXPECT_NE(ddl.find("Gorilla"), std::string::npos);
}

// ============================================================================
// Schema Generator Tests
// ============================================================================

TEST(ClickHouseSchemaGeneratorTest, GenerateTradesSchema) {
    ClickHouseSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();

    EXPECT_EQ(schema.name, "trades");
    EXPECT_FALSE(schema.columns.empty());
    EXPECT_FALSE(schema.order_by.empty());
    EXPECT_FALSE(schema.partition_by.empty());

    // Verify key columns exist
    bool has_timestamp = false, has_sym = false, has_price = false;
    for (const auto& col : schema.columns) {
        if (col.name == "timestamp") has_timestamp = true;
        if (col.name == "sym")       has_sym = true;
        if (col.name == "price")     has_price = true;
    }
    EXPECT_TRUE(has_timestamp);
    EXPECT_TRUE(has_sym);
    EXPECT_TRUE(has_price);
}

TEST(ClickHouseSchemaGeneratorTest, GenerateQuotesSchema) {
    ClickHouseSchemaGenerator gen;
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

TEST(ClickHouseSchemaGeneratorTest, GenerateFromKDBSchema) {
    ClickHouseSchemaGenerator gen;

    std::vector<std::pair<std::string, int8_t>> kdb_cols = {
        {"timestamp", 12},  // timestamp
        {"sym",       11},  // symbol
        {"price",      9},  // float
        {"size",       9},  // float
    };

    auto schema = gen.from_kdb_schema("trades", kdb_cols);

    EXPECT_EQ(schema.name, "trades");
    EXPECT_EQ(schema.columns.size(), 4);
    EXPECT_FALSE(schema.order_by.empty());
}

// ============================================================================
// DDL Generation Tests
// ============================================================================

TEST(ClickHouseDDLTest, TradesCreateDDL) {
    ClickHouseSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();
    std::string ddl = schema.to_create_ddl();

    EXPECT_NE(ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(ddl.find("trades"), std::string::npos);
    EXPECT_NE(ddl.find("ENGINE"), std::string::npos);
    EXPECT_NE(ddl.find("MergeTree"), std::string::npos);
    EXPECT_NE(ddl.find("ORDER BY"), std::string::npos);
    EXPECT_NE(ddl.find("PARTITION BY"), std::string::npos);
}

TEST(ClickHouseDDLTest, WithDatabase) {
    ClickHouseSchemaGenerator gen;
    auto schema = gen.generate_trades_schema();
    schema.database = "hft";

    std::string ddl = schema.to_create_ddl();
    EXPECT_NE(ddl.find("hft.trades"), std::string::npos);
}

// ============================================================================
// Query Translator Tests
// ============================================================================

TEST(ClickHouseQueryTranslatorTest, XbarToInterval) {
    ClickHouseQueryTranslator translator;

    std::string apex_sql = "SELECT xbar(timestamp, 300), sym FROM trades";
    std::string ch_sql = translator.translate(apex_sql);

    EXPECT_NE(ch_sql.find("toStartOfInterval"), std::string::npos);
    EXPECT_NE(ch_sql.find("300"), std::string::npos);
}

TEST(ClickHouseQueryTranslatorTest, VWAP) {
    ClickHouseQueryTranslator translator;
    std::string vwap = translator.translate_vwap("size", "price");

    EXPECT_NE(vwap.find("sumProduct"), std::string::npos);
    EXPECT_NE(vwap.find("sum(size)"), std::string::npos);
}

TEST(ClickHouseQueryTranslatorTest, AsofJoin) {
    ClickHouseQueryTranslator translator;
    std::string join = translator.translate_asof_join("trades", "quotes", "sym", "timestamp");

    EXPECT_NE(join.find("ASOF JOIN"), std::string::npos);
    EXPECT_NE(join.find("trades"), std::string::npos);
    EXPECT_NE(join.find("quotes"), std::string::npos);
}

TEST(ClickHouseQueryTranslatorTest, FirstLastRewrite) {
    ClickHouseQueryTranslator translator;

    std::string apex_sql = "SELECT FIRST(price), LAST(price) FROM trades";
    std::string ch_sql = translator.translate(apex_sql);

    EXPECT_NE(ch_sql.find("argMin"), std::string::npos);
    EXPECT_NE(ch_sql.find("argMax"), std::string::npos);
}

// ============================================================================
// Exporter Tests
// ============================================================================

TEST(ClickHouseExporterTest, GenerateBulkLoadCommand) {
    ClickHouseExporter exporter;

    std::string cmd = exporter.generate_bulk_load_cmd(
        "hft.trades", "/data/trades.csv", "CSV");

    EXPECT_NE(cmd.find("clickhouse-client"), std::string::npos);
    EXPECT_NE(cmd.find("hft.trades"), std::string::npos);
    EXPECT_NE(cmd.find("/data/trades.csv"), std::string::npos);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
