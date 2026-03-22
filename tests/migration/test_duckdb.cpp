// ============================================================================
// APEX-DB: DuckDB Interoperability Tests
// ============================================================================
#include "apex/migration/duckdb_interop.h"
#include <gtest/gtest.h>

using namespace apex::migration;

// ============================================================================
// Type Mapper Tests
// ============================================================================

TEST(DuckDBTypeMapperTest, APEXTypes) {
    EXPECT_EQ(APEXToDuckDBTypeMapper::apex_to_duckdb("BIGINT"),    "BIGINT");
    EXPECT_EQ(APEXToDuckDBTypeMapper::apex_to_duckdb("DOUBLE"),    "DOUBLE");
    EXPECT_EQ(APEXToDuckDBTypeMapper::apex_to_duckdb("VARCHAR"),   "VARCHAR");
    EXPECT_EQ(APEXToDuckDBTypeMapper::apex_to_duckdb("TIMESTAMP"), "TIMESTAMP");
    EXPECT_EQ(APEXToDuckDBTypeMapper::apex_to_duckdb("BOOLEAN"),   "BOOLEAN");
}

TEST(DuckDBTypeMapperTest, KDBTypes) {
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_duckdb(7),  "BIGINT");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_duckdb(9),  "DOUBLE");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_duckdb(11), "VARCHAR");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_duckdb(12), "TIMESTAMP");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_duckdb(14), "DATE");
}

TEST(DuckDBTypeMapperTest, ParquetPhysicalTypes) {
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_physical(6),  "INT32");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_physical(7),  "INT64");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_physical(9),  "DOUBLE");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_physical(11), "BYTE_ARRAY");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_physical(12), "INT64");
}

TEST(DuckDBTypeMapperTest, ParquetLogicalTypes) {
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_logical(11), "STRING");
    EXPECT_EQ(APEXToDuckDBTypeMapper::ktype_to_parquet_logical(14), "DATE");
    EXPECT_NE(APEXToDuckDBTypeMapper::ktype_to_parquet_logical(12).find("TIMESTAMP"),
              std::string::npos);
}

// ============================================================================
// Query Adapter Tests
// ============================================================================

TEST(DuckDBQueryAdapterTest, XbarToTimeBucket) {
    DuckDBQueryAdapter adapter;

    std::string apex_sql = "SELECT xbar(timestamp, 300), sym FROM trades";
    std::string duck_sql = adapter.adapt(apex_sql);

    EXPECT_NE(duck_sql.find("time_bucket"), std::string::npos);
    EXPECT_NE(duck_sql.find("300"), std::string::npos);
}

TEST(DuckDBQueryAdapterTest, ReadParquetSQL) {
    DuckDBQueryAdapter adapter;

    std::string sql = adapter.read_parquet_sql("/data/trades.parquet", "t");
    EXPECT_NE(sql.find("read_parquet"), std::string::npos);
    EXPECT_NE(sql.find("/data/trades.parquet"), std::string::npos);
    EXPECT_NE(sql.find("AS t"), std::string::npos);
}

TEST(DuckDBQueryAdapterTest, ReadHDBDirectorySQL) {
    DuckDBQueryAdapter adapter;

    std::string sql = adapter.read_hdb_directory_sql("/data/hdb", "trades");
    EXPECT_NE(sql.find("read_parquet"), std::string::npos);
    EXPECT_NE(sql.find("hive_partitioning"), std::string::npos);
    EXPECT_NE(sql.find("trades"), std::string::npos);
}

TEST(DuckDBQueryAdapterTest, ParallelismHints) {
    DuckDBQueryAdapter adapter;

    std::string sql = "SELECT * FROM trades";
    std::string with_hints = adapter.add_parallelism_hints(sql, 8);

    EXPECT_NE(with_hints.find("SET threads"), std::string::npos);
    EXPECT_NE(with_hints.find("8"), std::string::npos);
}

TEST(DuckDBQueryAdapterTest, GeneratePythonScript) {
    DuckDBQueryAdapter adapter;

    std::string script = adapter.generate_python_script(
        "trades", "/data/trades.parquet",
        "SELECT * FROM trades LIMIT 10");

    EXPECT_NE(script.find("import duckdb"), std::string::npos);
    EXPECT_NE(script.find("read_parquet"), std::string::npos);
    EXPECT_NE(script.find("trades"), std::string::npos);
}

// ============================================================================
// Parquet Exporter Tests
// ============================================================================

TEST(ParquetExporterTest, GenerateCopyCommand) {
    ParquetExporter exporter;

    std::string cmd = exporter.generate_copy_command(
        "SELECT * FROM trades", "/output/trades.parquet");

    EXPECT_NE(cmd.find("COPY"), std::string::npos);
    EXPECT_NE(cmd.find("FORMAT PARQUET"), std::string::npos);
    EXPECT_NE(cmd.find("/output/trades.parquet"), std::string::npos);
}

TEST(ParquetExporterTest, GeneratePartitionedExport) {
    ParquetExporter exporter;

    std::string cmd = exporter.generate_partitioned_export(
        "trades", "/output", "date");

    EXPECT_NE(cmd.find("PARTITION_BY"), std::string::npos);
    EXPECT_NE(cmd.find("date"), std::string::npos);
    EXPECT_NE(cmd.find("FORMAT PARQUET"), std::string::npos);
}

TEST(ParquetExporterTest, GenerateHDBToParquetScript) {
    ParquetExporter exporter;

    std::string script = exporter.generate_hdb_to_parquet_script(
        "/data/hdb", "/output",
        {"trades", "quotes"});

    EXPECT_NE(script.find("apex-migrate"), std::string::npos);
    EXPECT_NE(script.find("trades"), std::string::npos);
    EXPECT_NE(script.find("quotes"), std::string::npos);
}

// ============================================================================
// DuckDB Integrator Tests
// ============================================================================

TEST(DuckDBIntegratorTest, GenerateSetupScript) {
    DuckDBIntegrator::Config config;
    config.threads = 8;
    config.memory_limit_mb = 4096;

    DuckDBIntegrator integrator(config);
    std::string script = integrator.generate_setup_script("/data/parquet");

    EXPECT_NE(script.find("SET threads"), std::string::npos);
    EXPECT_NE(script.find("SET memory_limit"), std::string::npos);
    EXPECT_NE(script.find("CREATE OR REPLACE VIEW"), std::string::npos);
    EXPECT_NE(script.find("read_parquet"), std::string::npos);
    EXPECT_NE(script.find("trades"), std::string::npos);
}

TEST(DuckDBIntegratorTest, GenerateAnalyticsExamples) {
    DuckDBIntegrator::Config config;
    DuckDBIntegrator integrator(config);

    std::string examples = integrator.generate_analytics_examples("trades");

    EXPECT_NE(examples.find("VWAP"), std::string::npos);
    EXPECT_NE(examples.find("OHLCV"), std::string::npos);
    EXPECT_NE(examples.find("time_bucket"), std::string::npos);
    EXPECT_NE(examples.find("GROUP BY"), std::string::npos);
}

TEST(DuckDBIntegratorTest, GenerateJupyterTemplate) {
    DuckDBIntegrator::Config config;
    DuckDBIntegrator integrator(config);

    std::string notebook = integrator.generate_jupyter_template("/data/parquet");

    EXPECT_NE(notebook.find("duckdb"), std::string::npos);
    EXPECT_NE(notebook.find("read_parquet"), std::string::npos);
    EXPECT_NE(notebook.find("nbformat"), std::string::npos);
}

// ============================================================================
// Parquet Schema Tests
// ============================================================================

TEST(ParquetSchemaTest, ToDuckDBDDL) {
    ParquetSchema schema;
    schema.table_name = "trades";
    schema.columns = {
        {"timestamp", "TIMESTAMPTZ", "INT64", true},
        {"sym",       "STRING",      "BYTE_ARRAY", true},
        {"price",     "DOUBLE",      "DOUBLE", true},
    };

    std::string ddl = schema.to_duckdb_ddl();
    EXPECT_NE(ddl.find("read_parquet"), std::string::npos);
    EXPECT_NE(ddl.find("trades"), std::string::npos);
}

TEST(ParquetSchemaTest, ToArrowSchema) {
    ParquetSchema schema;
    schema.table_name = "trades";
    schema.columns = {
        {"timestamp", "TIMESTAMP(isAdjustedToUTC=true, unit=NANOS)", "INT64", true},
        {"sym",       "STRING",  "BYTE_ARRAY", true},
        {"price",     "DOUBLE",  "DOUBLE",     true},
    };

    std::string arrow = schema.to_arrow_schema();
    EXPECT_NE(arrow.find("import pyarrow"), std::string::npos);
    EXPECT_NE(arrow.find("pa.schema"), std::string::npos);
    EXPECT_NE(arrow.find("timestamp"), std::string::npos);
    EXPECT_NE(arrow.find("pa.timestamp"), std::string::npos);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
