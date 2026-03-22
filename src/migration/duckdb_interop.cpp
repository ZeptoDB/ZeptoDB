// ============================================================================
// APEX-DB: DuckDB Interoperability Implementation
// ============================================================================
#include "apex/migration/duckdb_interop.h"
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace apex::migration {

// ============================================================================
// Type Mapper
// ============================================================================

std::string APEXToDuckDBTypeMapper::apex_to_duckdb(const std::string& apex_type) {
    // DuckDB is mostly ANSI SQL compatible
    if (apex_type == "BOOLEAN")   return "BOOLEAN";
    if (apex_type == "TINYINT")   return "TINYINT";
    if (apex_type == "SMALLINT")  return "SMALLINT";
    if (apex_type == "INTEGER")   return "INTEGER";
    if (apex_type == "BIGINT")    return "BIGINT";
    if (apex_type == "REAL")      return "FLOAT";
    if (apex_type == "DOUBLE")    return "DOUBLE";
    if (apex_type == "VARCHAR")   return "VARCHAR";
    if (apex_type == "TEXT")      return "VARCHAR";
    if (apex_type == "DATE")      return "DATE";
    if (apex_type == "TIMESTAMP") return "TIMESTAMP";
    if (apex_type == "TIME")      return "TIME";
    return "VARCHAR";
}

std::string APEXToDuckDBTypeMapper::apex_to_parquet(const std::string& apex_type) {
    if (apex_type == "BOOLEAN")   return "BOOLEAN";
    if (apex_type == "TINYINT")   return "INT32";
    if (apex_type == "SMALLINT")  return "INT32";
    if (apex_type == "INTEGER")   return "INT32";
    if (apex_type == "BIGINT")    return "INT64";
    if (apex_type == "REAL")      return "FLOAT";
    if (apex_type == "DOUBLE")    return "DOUBLE";
    if (apex_type == "VARCHAR")   return "BYTE_ARRAY";
    if (apex_type == "DATE")      return "INT32";   // days since epoch
    if (apex_type == "TIMESTAMP") return "INT64";   // nanoseconds since epoch
    return "BYTE_ARRAY";
}

std::string APEXToDuckDBTypeMapper::ktype_to_duckdb(int8_t kdb_type) {
    switch (kdb_type) {
        case 1:  return "BOOLEAN";
        case 4:  return "UTINYINT";
        case 5:  return "SMALLINT";
        case 6:  return "INTEGER";
        case 7:  return "BIGINT";
        case 8:  return "FLOAT";
        case 9:  return "DOUBLE";
        case 10: return "VARCHAR";    // char
        case 11: return "VARCHAR";    // symbol
        case 12: return "TIMESTAMP";  // timestamp (ns)
        case 14: return "DATE";
        case 19: return "INTEGER";    // time (ms)
        default: return "VARCHAR";
    }
}

std::string APEXToDuckDBTypeMapper::ktype_to_parquet_physical(int8_t kdb_type) {
    switch (kdb_type) {
        case 1:  return "BOOLEAN";
        case 4:  return "INT32";
        case 5:  return "INT32";
        case 6:  return "INT32";
        case 7:  return "INT64";
        case 8:  return "FLOAT";
        case 9:  return "DOUBLE";
        case 10: return "BYTE_ARRAY";
        case 11: return "BYTE_ARRAY";
        case 12: return "INT64";    // nanos
        case 14: return "INT32";    // days
        case 19: return "INT32";    // ms
        default: return "BYTE_ARRAY";
    }
}

std::string APEXToDuckDBTypeMapper::ktype_to_parquet_logical(int8_t kdb_type) {
    switch (kdb_type) {
        case 7:  return "INT(64, true)";
        case 11: return "STRING";
        case 12: return "TIMESTAMP(isAdjustedToUTC=true, unit=NANOS)";
        case 14: return "DATE";
        default: return "";
    }
}

// ============================================================================
// ParquetColumn / Schema
// ============================================================================

std::string ParquetColumn::to_schema_string() const {
    return "  " + (required ? "required" : "optional") +
           " " + physical_type +
           " " + name + ";";
}

std::string ParquetSchema::to_duckdb_ddl() const {
    std::ostringstream ss;

    ss << "-- Read Parquet file into DuckDB\n";
    ss << "CREATE OR REPLACE TABLE " << table_name
       << " AS SELECT * FROM read_parquet('*.parquet');\n\n";

    ss << "-- Or with explicit schema:\n";
    ss << "DESCRIBE SELECT * FROM read_parquet('*.parquet');\n";

    return ss.str();
}

std::string ParquetSchema::to_arrow_schema() const {
    std::ostringstream ss;

    ss << "import pyarrow as pa\n\n";
    ss << "schema = pa.schema([\n";

    for (const auto& col : columns) {
        std::string arrow_type;
        if (col.physical_type == "INT32")      arrow_type = "pa.int32()";
        else if (col.physical_type == "INT64")  arrow_type = "pa.int64()";
        else if (col.physical_type == "FLOAT")  arrow_type = "pa.float32()";
        else if (col.physical_type == "DOUBLE") arrow_type = "pa.float64()";
        else if (col.physical_type == "BYTE_ARRAY") arrow_type = "pa.large_string()";
        else if (col.logical_type == "TIMESTAMP(isAdjustedToUTC=true, unit=NANOS)")
            arrow_type = "pa.timestamp('ns', tz='UTC')";
        else if (col.logical_type == "DATE")    arrow_type = "pa.date32()";
        else arrow_type = "pa.string()";

        ss << "    pa.field('" << col.name << "', " << arrow_type << "),\n";
    }

    ss << "])\n";
    return ss.str();
}

// ============================================================================
// DuckDB Query Adapter
// ============================================================================

std::string DuckDBQueryAdapter::adapt(const std::string& apex_sql) {
    std::string result = apex_sql;
    result = rewrite_window_functions(result);
    return result;
}

std::string DuckDBQueryAdapter::add_parallelism_hints(const std::string& sql,
                                                       int thread_count) {
    std::ostringstream ss;

    if (thread_count > 0) {
        ss << "SET threads = " << thread_count << ";\n";
    } else {
        ss << "-- DuckDB auto-detects thread count\n";
    }

    ss << sql;
    return ss.str();
}

std::string DuckDBQueryAdapter::generate_python_script(
    const std::string& table_name,
    const std::string& parquet_path,
    const std::string& query)
{
    std::ostringstream ss;

    ss << "import duckdb\n";
    ss << "import pandas as pd\n\n";
    ss << "# Connect to DuckDB (in-memory)\n";
    ss << "conn = duckdb.connect()\n\n";
    ss << "# Load Parquet data\n";
    ss << table_name << " = conn.read_parquet('" << parquet_path << "')\n\n";
    ss << "# Run query\n";
    ss << "result = conn.execute(\"\"\"\n";
    ss << "    " << query << "\n";
    ss << "\"\"\").df()\n\n";
    ss << "print(result.head())\n";

    return ss.str();
}

std::string DuckDBQueryAdapter::read_parquet_sql(const std::string& parquet_path,
                                                  const std::string& table_alias) {
    return "FROM read_parquet('" + parquet_path + "') AS " + table_alias;
}

std::string DuckDBQueryAdapter::read_hdb_directory_sql(
    const std::string& hdb_path,
    const std::string& table_name)
{
    std::ostringstream ss;

    ss << "-- Read entire HDB partition directory\n";
    ss << "FROM read_parquet('" << hdb_path << "/*/" << table_name << "/*.parquet',\n";
    ss << "                  hive_partitioning = true,\n";
    ss << "                  filename = true)\n";

    return ss.str();
}

std::string DuckDBQueryAdapter::rewrite_window_functions(const std::string& sql) {
    // DuckDB supports most standard window functions
    // Minimal rewriting needed
    std::string result = sql;

    // xbar → time_bucket equivalent
    size_t pos = 0;
    while ((pos = result.find("xbar(", pos)) != std::string::npos) {
        size_t end = result.find(')', pos);
        if (end == std::string::npos) break;

        std::string args = result.substr(pos + 5, end - pos - 5);
        size_t comma = args.find(',');
        if (comma != std::string::npos) {
            std::string col = args.substr(0, comma);
            std::string interval = args.substr(comma + 1);

            auto trim = [](std::string s) {
                s.erase(s.find_last_not_of(" \t") + 1);
                s.erase(0, s.find_first_not_of(" \t"));
                return s;
            };

            col = trim(col);
            interval = trim(interval);

            // DuckDB: time_bucket(INTERVAL 'N seconds', column)
            std::string replacement =
                "time_bucket(INTERVAL '" + interval + " seconds', " + col + ")";
            result.replace(pos, end - pos + 1, replacement);
        }
        ++pos;
    }

    return result;
}

// ============================================================================
// Parquet Exporter
// ============================================================================

ParquetExporter::ParquetExporter(const ExportOptions& opts)
    : options_(opts)
{}

std::string ParquetExporter::generate_copy_command(
    const std::string& query,
    const std::string& output_path)
{
    std::ostringstream ss;

    ss << "COPY (\n";
    ss << "    " << query << "\n";
    ss << ") TO '" << output_path << "'\n";
    ss << "(FORMAT PARQUET,\n";
    ss << " COMPRESSION '" << options_.compression << "',\n";
    ss << " ROW_GROUP_SIZE " << options_.row_group_size << ",\n";

    if (options_.enable_statistics) {
        ss << " WRITE_STATISTICS TRUE,\n";
    }

    ss << " USE_TMP_FILE TRUE);\n";

    return ss.str();
}

std::string ParquetExporter::generate_partitioned_export(
    const std::string& table_name,
    const std::string& output_dir,
    const std::string& partition_col)
{
    std::ostringstream ss;

    ss << "-- Partitioned Parquet export (DuckDB)\n";
    ss << "COPY (\n";
    ss << "    SELECT * FROM " << table_name << "\n";
    ss << "    ORDER BY " << partition_col << ", sym\n";
    ss << ") TO '" << output_dir << "'\n";
    ss << "(FORMAT PARQUET,\n";
    ss << " PARTITION_BY (" << partition_col << "),\n";
    ss << " COMPRESSION '" << options_.compression << "',\n";
    ss << " ROW_GROUP_SIZE " << options_.row_group_size << ",\n";
    ss << " OVERWRITE_OR_IGNORE TRUE);\n";

    return ss.str();
}

std::string ParquetExporter::generate_hdb_to_parquet_script(
    const std::string& hdb_path,
    const std::string& output_dir,
    const std::vector<std::string>& tables)
{
    std::ostringstream ss;

    ss << "#!/usr/bin/env python3\n";
    ss << "# Convert kdb+ HDB to Parquet via APEX-DB\n\n";
    ss << "import subprocess\nimport os\n\n";

    ss << "hdb_path = '" << hdb_path << "'\n";
    ss << "output_dir = '" << output_dir << "'\n";
    ss << "os.makedirs(output_dir, exist_ok=True)\n\n";

    if (tables.empty()) {
        ss << "# Migrate all tables\n";
        ss << "subprocess.run(['apex-migrate', 'hdb',\n";
        ss << "                '--hdb-dir', hdb_path,\n";
        ss << "                '--output', output_dir])\n";
    } else {
        for (const auto& table : tables) {
            ss << "# Migrate table: " << table << "\n";
            ss << "subprocess.run(['apex-migrate', 'hdb',\n";
            ss << "                '--hdb-dir', hdb_path,\n";
            ss << "                '--output', output_dir,\n";
            ss << "                '--table', '" << table << "'])\n\n";
        }
    }

    return ss.str();
}

// ============================================================================
// DuckDB Integrator
// ============================================================================

DuckDBIntegrator::DuckDBIntegrator(const Config& config)
    : config_(config)
{}

bool DuckDBIntegrator::export_to_parquet(const std::string& table_name,
                                          const std::string& output_dir) {
    std::filesystem::create_directories(output_dir);

    // Generate export script
    auto script = parquet_exporter_.generate_partitioned_export(
        table_name, output_dir + "/" + table_name, "date");

    // Write script file
    std::string script_path = output_dir + "/export_" + table_name + ".sql";
    std::ofstream out(script_path);
    if (!out) return false;

    out << "-- APEX-DB to DuckDB/Parquet export\n";
    out << "-- Run with: duckdb < " << script_path << "\n\n";
    out << script;

    return true;
}

std::string DuckDBIntegrator::generate_setup_script(const std::string& parquet_dir) {
    std::ostringstream ss;

    ss << "-- DuckDB setup for APEX-DB data\n";
    ss << "-- Run: duckdb apex_analytics.duckdb < setup.sql\n\n";

    if (config_.threads > 0) {
        ss << "SET threads = " << config_.threads << ";\n";
    }

    if (config_.memory_limit_mb > 0) {
        ss << "SET memory_limit = '" << config_.memory_limit_mb << "MB';\n";
    }

    ss << "\n-- Create views over Parquet files\n";

    for (const std::string table : {"trades", "quotes", "orderbook"}) {
        ss << "CREATE OR REPLACE VIEW " << table << " AS\n";
        ss << "  SELECT * FROM read_parquet(\n";
        ss << "    '" << parquet_dir << "/" << table << "/**/*.parquet',\n";
        ss << "    hive_partitioning = true\n";
        ss << "  );\n\n";
    }

    ss << "-- Verify\n";
    ss << "SELECT 'trades' AS table_name, COUNT(*) AS rows FROM trades\n";
    ss << "UNION ALL\n";
    ss << "SELECT 'quotes', COUNT(*) FROM quotes;\n";

    return ss.str();
}

std::string DuckDBIntegrator::generate_analytics_examples(
    const std::string& table_name)
{
    std::ostringstream ss;

    ss << "-- APEX-DB Analytics Examples (DuckDB)\n\n";

    ss << "-- 1. VWAP by symbol (1-minute bars)\n";
    ss << "SELECT\n";
    ss << "    sym,\n";
    ss << "    time_bucket(INTERVAL '1 minute', timestamp) AS bar_time,\n";
    ss << "    SUM(size * price) / SUM(size) AS vwap,\n";
    ss << "    SUM(size) AS total_volume,\n";
    ss << "    COUNT(*) AS trade_count\n";
    ss << "FROM " << table_name << "\n";
    ss << "GROUP BY sym, bar_time\n";
    ss << "ORDER BY sym, bar_time;\n\n";

    ss << "-- 2. OHLCV bars (5-minute)\n";
    ss << "SELECT\n";
    ss << "    sym,\n";
    ss << "    time_bucket(INTERVAL '5 minutes', timestamp) AS bar_time,\n";
    ss << "    first(price ORDER BY timestamp) AS open,\n";
    ss << "    max(price)                       AS high,\n";
    ss << "    min(price)                       AS low,\n";
    ss << "    last(price ORDER BY timestamp)   AS close,\n";
    ss << "    sum(size)                        AS volume\n";
    ss << "FROM " << table_name << "\n";
    ss << "GROUP BY sym, bar_time\n";
    ss << "ORDER BY sym, bar_time;\n\n";

    ss << "-- 3. Top symbols by volume\n";
    ss << "SELECT sym, SUM(size * price) AS notional\n";
    ss << "FROM " << table_name << "\n";
    ss << "GROUP BY sym\n";
    ss << "ORDER BY notional DESC\n";
    ss << "LIMIT 20;\n\n";

    ss << "-- 4. Spread analysis (join trades and quotes)\n";
    ss << "SELECT\n";
    ss << "    t.sym,\n";
    ss << "    AVG(q.ask - q.bid) AS avg_spread,\n";
    ss << "    AVG((q.ask - q.bid) / q.bid * 100) AS avg_spread_bps\n";
    ss << "FROM quotes q\n";
    ss << "JOIN trades t\n";
    ss << "    ON t.sym = q.sym\n";
    ss << "    AND t.timestamp >= q.timestamp\n";
    ss << "    AND t.timestamp < q.timestamp + INTERVAL '1 second'\n";
    ss << "GROUP BY t.sym\n";
    ss << "ORDER BY avg_spread_bps;\n";

    return ss.str();
}

std::string DuckDBIntegrator::generate_jupyter_template(
    const std::string& parquet_dir)
{
    std::ostringstream ss;

    ss << "{\n";
    ss << " \"cells\": [\n";
    ss << "  {\n";
    ss << "   \"cell_type\": \"code\",\n";
    ss << "   \"source\": [\n";
    ss << "    \"import duckdb\\n\",\n";
    ss << "    \"import pandas as pd\\n\",\n";
    ss << "    \"import matplotlib.pyplot as plt\\n\",\n";
    ss << "    \"\\n\",\n";
    ss << "    \"conn = duckdb.connect()\\n\",\n";
    ss << "    \"\\n\",\n";
    ss << "    \"# Load APEX-DB Parquet data\\n\",\n";
    ss << "    \"conn.execute(\\\"\\\"\\\"\\n\",\n";
    ss << "    \"    CREATE VIEW trades AS\\n\",\n";
    ss << "    \"    SELECT * FROM read_parquet('" << parquet_dir << "/trades/**/*.parquet',\\n\",\n";
    ss << "    \"                               hive_partitioning=true)\\n\",\n";
    ss << "    \"\\\"\\\"\\\")\\n\"\n";
    ss << "   ]\n";
    ss << "  },\n";
    ss << "  {\n";
    ss << "   \"cell_type\": \"code\",\n";
    ss << "   \"source\": [\n";
    ss << "    \"# VWAP Analysis\\n\",\n";
    ss << "    \"vwap = conn.execute(\\\"\\\"\\\"\\n\",\n";
    ss << "    \"    SELECT sym, time_bucket(INTERVAL '1 minute', timestamp) AS bar,\\n\",\n";
    ss << "    \"           SUM(size * price) / SUM(size) AS vwap\\n\",\n";
    ss << "    \"    FROM trades\\n\",\n";
    ss << "    \"    GROUP BY sym, bar\\n\",\n";
    ss << "    \"\\\"\\\"\\\").df()\\n\",\n";
    ss << "    \"vwap.head()\"\n";
    ss << "   ]\n";
    ss << "  }\n";
    ss << " ],\n";
    ss << " \"metadata\": {\"kernelspec\": {\"name\": \"python3\"}},\n";
    ss << " \"nbformat\": 4,\n";
    ss << " \"nbformat_minor\": 5\n";
    ss << "}\n";

    return ss.str();
}

} // namespace apex::migration
