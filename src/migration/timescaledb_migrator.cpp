// ============================================================================
// APEX-DB: TimescaleDB Migration Toolkit Implementation
// ============================================================================
#include "apex/migration/timescaledb_migrator.h"
#include <sstream>
#include <iostream>
#include <fstream>

namespace apex::migration {

// ============================================================================
// TSDBCompressionPolicy
// ============================================================================

std::string TSDBCompressionPolicy::to_sql(const std::string& table_name) const {
    std::ostringstream ss;

    ss << "ALTER TABLE " << table_name << " SET (\n";
    ss << "  timescaledb.compress,\n";
    ss << "  timescaledb.compress_orderby = '" << order_by_column << " DESC',\n";

    if (!segment_by_columns.empty()) {
        ss << "  timescaledb.compress_segmentby = '";
        for (size_t i = 0; i < segment_by_columns.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << segment_by_columns[i];
        }
        ss << "'\n";
    }

    ss << ");\n\n";

    ss << "SELECT add_compression_policy('" << table_name
       << "', INTERVAL '" << compress_after_days << " days');\n";

    return ss.str();
}

// ============================================================================
// TSDBContinuousAggregate
// ============================================================================

std::string TSDBContinuousAggregate::to_create_sql() const {
    std::ostringstream ss;

    ss << "CREATE MATERIALIZED VIEW " << view_name << "\n";
    ss << "WITH (timescaledb.continuous) AS\n";
    ss << "SELECT\n";

    // Time bucket
    ss << "    time_bucket('" << bucket_interval << "', timestamp) AS bucket,\n";

    // Group by columns
    for (const auto& col : group_by_columns) {
        ss << "    " << col << ",\n";
    }

    // Aggregates
    for (size_t i = 0; i < aggregates.size(); ++i) {
        ss << "    " << aggregates[i].second << " AS " << aggregates[i].first;
        if (i < aggregates.size() - 1) ss << ",";
        ss << "\n";
    }

    ss << "FROM " << source_table << "\n";
    ss << "GROUP BY bucket";
    for (const auto& col : group_by_columns) {
        ss << ", " << col;
    }
    ss << "\nWITH NO DATA;\n";

    return ss.str();
}

std::string TSDBContinuousAggregate::to_refresh_policy_sql() const {
    std::ostringstream ss;

    ss << "SELECT add_continuous_aggregate_policy('" << view_name << "',\n";
    ss << "  start_offset => INTERVAL '30 days',\n";
    ss << "  end_offset   => INTERVAL '" << lag_minutes << " minutes',\n";
    ss << "  schedule_interval => INTERVAL '" << refresh_every_minutes << " minutes');\n";

    return ss.str();
}

// ============================================================================
// TSDBRetentionPolicy
// ============================================================================

std::string TSDBRetentionPolicy::to_sql() const {
    std::ostringstream ss;
    ss << "SELECT add_retention_policy('" << table_name << "', "
       << "INTERVAL '" << retain_days << " days');\n";
    return ss.str();
}

// ============================================================================
// TSDBTableSchema DDL
// ============================================================================

std::string TSDBTableSchema::to_create_table_sql() const {
    std::ostringstream ss;

    std::string full_name = schema_name == "public" ? name : (schema_name + "." + name);

    ss << "CREATE TABLE IF NOT EXISTS " << full_name << " (\n";

    for (size_t i = 0; i < columns.size(); ++i) {
        const auto& col = columns[i];
        ss << "    " << col.name << " " << col.pg_type;
        if (col.not_null) ss << " NOT NULL";
        if (!col.default_value.empty()) ss << " DEFAULT " << col.default_value;
        if (i < columns.size() - 1) ss << ",";
        ss << "\n";
    }

    ss << ");\n";
    return ss.str();
}

std::string TSDBTableSchema::to_create_hypertable_sql() const {
    std::ostringstream ss;

    std::string full_name = schema_name == "public" ? name : (schema_name + "." + name);

    ss << "SELECT create_hypertable(\n";
    ss << "    '" << full_name << "',\n";
    ss << "    '" << time_column << "',\n";

    if (!space_partitions.empty()) {
        ss << "    partitioning_column => '" << space_partitions[0] << "',\n";
        ss << "    number_partitions => " << space_partitions_count << ",\n";
    }

    ss << "    chunk_time_interval => INTERVAL '" << chunk_interval_days << " day',\n";
    ss << "    if_not_exists => TRUE\n";
    ss << ");\n";

    return ss.str();
}

std::string TSDBTableSchema::to_create_indexes_sql() const {
    std::ostringstream ss;

    for (const auto& idx : indexes) {
        ss << "CREATE ";
        if (idx.unique) ss << "UNIQUE ";
        ss << "INDEX IF NOT EXISTS " << idx.name << "\n";
        ss << "    ON " << name << " (";
        for (size_t i = 0; i < idx.columns.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << idx.columns[i];
        }
        ss << ");\n";
    }

    return ss.str();
}

std::string TSDBTableSchema::to_enable_compression_sql() const {
    return compression.to_sql(name);
}

std::string TSDBTableSchema::to_full_setup_sql() const {
    std::ostringstream ss;

    ss << "-- ================================================================\n";
    ss << "-- TimescaleDB: " << name << " setup\n";
    ss << "-- ================================================================\n\n";

    // 1. Create table
    ss << "-- Step 1: Create table\n";
    ss << to_create_table_sql() << "\n";

    // 2. Create hypertable
    ss << "-- Step 2: Create hypertable\n";
    ss << to_create_hypertable_sql() << "\n";

    // 3. Create indexes
    if (!indexes.empty()) {
        ss << "-- Step 3: Create indexes\n";
        ss << to_create_indexes_sql() << "\n";
    }

    // 4. Enable compression
    ss << "-- Step 4: Enable compression\n";
    ss << to_enable_compression_sql() << "\n";

    // 5. Continuous aggregates
    if (!continuous_aggregates.empty()) {
        ss << "-- Step 5: Continuous aggregates\n";
        for (const auto& ca : continuous_aggregates) {
            ss << ca.to_create_sql() << "\n";
            ss << ca.to_refresh_policy_sql() << "\n";
        }
    }

    // 6. Retention
    if (retention.retain_days > 0) {
        ss << "-- Step 6: Retention policy\n";
        ss << retention.to_sql() << "\n";
    }

    return ss.str();
}

// ============================================================================
// Type Mapper
// ============================================================================

std::string APEXToTSDBTypeMapper::apex_to_pg(const std::string& apex_type) {
    if (apex_type == "BOOLEAN")   return "BOOLEAN";
    if (apex_type == "TINYINT")   return "SMALLINT";
    if (apex_type == "SMALLINT")  return "SMALLINT";
    if (apex_type == "INTEGER")   return "INTEGER";
    if (apex_type == "BIGINT")    return "BIGINT";
    if (apex_type == "REAL")      return "REAL";
    if (apex_type == "DOUBLE")    return "DOUBLE PRECISION";
    if (apex_type == "VARCHAR")   return "TEXT";
    if (apex_type == "DATE")      return "DATE";
    if (apex_type == "TIMESTAMP") return "TIMESTAMPTZ";
    if (apex_type == "TIME")      return "TIME";
    return "TEXT";
}

std::string APEXToTSDBTypeMapper::ktype_to_pg(int8_t kdb_type) {
    switch (kdb_type) {
        case 1:  return "BOOLEAN";
        case 4:  return "SMALLINT";
        case 5:  return "SMALLINT";
        case 6:  return "INTEGER";
        case 7:  return "BIGINT";
        case 8:  return "REAL";
        case 9:  return "DOUBLE PRECISION";
        case 10: return "CHAR(1)";
        case 11: return "TEXT";
        case 12: return "TIMESTAMPTZ";   // timestamp (nanosecond precision needs BIGINT)
        case 14: return "DATE";
        case 19: return "INTEGER";       // time in ms
        default: return "TEXT";
    }
}

// ============================================================================
// Schema Generator
// ============================================================================

TSDBTableSchema TimescaleDBSchemaGenerator::generate_trades_schema(
    const std::string& table_name)
{
    TSDBTableSchema schema;
    schema.name = table_name;
    schema.time_column = "timestamp";
    schema.space_partitions = {"sym"};
    schema.space_partitions_count = 4;
    schema.chunk_interval_days = 1;

    schema.columns = {
        {"timestamp", "TIMESTAMPTZ", true, ""},
        {"sym",       "TEXT",        true, ""},
        {"exchange",  "TEXT",        true, ""},
        {"price",     "DOUBLE PRECISION", true, ""},
        {"size",      "DOUBLE PRECISION", true, ""},
        {"side",      "SMALLINT",    false, "0"},
        {"trade_id",  "BIGINT",      false, "0"},
        {"condition", "TEXT",        false, "''"},
    };

    // Indexes
    schema.indexes = {
        {"trades_sym_ts_idx",  {"sym", "timestamp DESC"}, false},
        {"trades_ts_idx",      {"timestamp DESC"},         false},
    };

    // Compression: group by symbol
    schema.compression.order_by_column = "timestamp";
    schema.compression.segment_by_columns = {"sym"};
    schema.compression.compress_after_days = 7;

    // Continuous aggregates
    TSDBContinuousAggregate ohlcv;
    ohlcv.view_name = table_name + "_1min";
    ohlcv.source_table = table_name;
    ohlcv.bucket_interval = "1 minute";
    ohlcv.group_by_columns = {"sym"};
    ohlcv.aggregates = {
        {"open",   "first(price, timestamp)"},
        {"high",   "max(price)"},
        {"low",    "min(price)"},
        {"close",  "last(price, timestamp)"},
        {"volume", "sum(size)"},
        {"vwap",   "sum(size * price) / sum(size)"},
        {"count",  "count(*)"},
    };
    ohlcv.refresh_every_minutes = 1;

    schema.continuous_aggregates = {ohlcv};

    return schema;
}

TSDBTableSchema TimescaleDBSchemaGenerator::generate_quotes_schema(
    const std::string& table_name)
{
    TSDBTableSchema schema;
    schema.name = table_name;
    schema.time_column = "timestamp";
    schema.space_partitions = {"sym"};
    schema.chunk_interval_days = 1;

    schema.columns = {
        {"timestamp", "TIMESTAMPTZ",    true, ""},
        {"sym",       "TEXT",           true, ""},
        {"exchange",  "TEXT",           true, ""},
        {"bid",       "DOUBLE PRECISION", true, ""},
        {"ask",       "DOUBLE PRECISION", true, ""},
        {"bid_size",  "DOUBLE PRECISION", true, ""},
        {"ask_size",  "DOUBLE PRECISION", true, ""},
    };

    schema.indexes = {
        {"quotes_sym_ts_idx", {"sym", "timestamp DESC"}, false},
    };

    schema.compression.segment_by_columns = {"sym"};
    schema.compression.compress_after_days = 7;

    // 1-minute spread aggregate
    TSDBContinuousAggregate spread;
    spread.view_name = table_name + "_spread_1min";
    spread.source_table = table_name;
    spread.bucket_interval = "1 minute";
    spread.group_by_columns = {"sym"};
    spread.aggregates = {
        {"avg_bid",    "avg(bid)"},
        {"avg_ask",    "avg(ask)"},
        {"avg_spread", "avg(ask - bid)"},
        {"avg_spread_bps", "avg((ask - bid) / bid * 10000)"},
        {"avg_bid_size",   "avg(bid_size)"},
        {"avg_ask_size",   "avg(ask_size)"},
    };

    schema.continuous_aggregates = {spread};

    return schema;
}

TSDBTableSchema TimescaleDBSchemaGenerator::generate_orderbook_schema(
    const std::string& table_name)
{
    TSDBTableSchema schema;
    schema.name = table_name;
    schema.time_column = "timestamp";
    schema.chunk_interval_days = 1;

    schema.columns = {
        {"timestamp",   "TIMESTAMPTZ",    true, ""},
        {"sym",         "TEXT",           true, ""},
        {"side",        "SMALLINT",       true, ""},
        {"level",       "SMALLINT",       true, ""},
        {"price",       "DOUBLE PRECISION", true, ""},
        {"size",        "DOUBLE PRECISION", true, ""},
        {"order_count", "INTEGER",        false, "0"},
    };

    schema.compression.segment_by_columns = {"sym", "side"};
    schema.compression.compress_after_days = 1;

    return schema;
}

TSDBTableSchema TimescaleDBSchemaGenerator::from_kdb_schema(
    const std::string& table_name,
    const std::vector<std::pair<std::string, int8_t>>& kdb_columns,
    const std::string& time_column)
{
    TSDBTableSchema schema;
    schema.name = table_name;
    schema.time_column = time_column;
    schema.chunk_interval_days = 1;

    bool has_sym = false;

    for (const auto& [col_name, ktype] : kdb_columns) {
        TSDBTableSchema::Column col;
        col.name = col_name;
        col.pg_type = APEXToTSDBTypeMapper::ktype_to_pg(ktype);
        col.not_null = (col_name == time_column);
        schema.columns.push_back(col);

        if (col_name == "sym" || col_name == "symbol") {
            has_sym = true;
        }
    }

    if (has_sym) {
        schema.space_partitions = {"sym"};
        schema.space_partitions_count = 4;
        schema.compression.segment_by_columns = {"sym"};
    }

    schema.compression.order_by_column = time_column;
    schema.compression.compress_after_days = 7;

    // Default index
    schema.indexes = {
        {table_name + "_ts_idx", {time_column + " DESC"}, false},
    };

    return schema;
}

void TimescaleDBSchemaGenerator::add_ohlcv_aggregate(TSDBTableSchema& schema,
                                                      const std::string& interval) {
    TSDBContinuousAggregate ca;
    ca.view_name = schema.name + "_ohlcv_" + interval;
    // Replace spaces with underscore in view name
    for (char& c : ca.view_name) if (c == ' ') c = '_';

    ca.source_table = schema.name;
    ca.bucket_interval = interval;
    ca.group_by_columns = {"sym"};
    ca.aggregates = {
        {"open",   "first(price, timestamp)"},
        {"high",   "max(price)"},
        {"low",    "min(price)"},
        {"close",  "last(price, timestamp)"},
        {"volume", "sum(size)"},
        {"trades", "count(*)"},
    };

    schema.continuous_aggregates.push_back(ca);
}

void TimescaleDBSchemaGenerator::add_vwap_aggregate(TSDBTableSchema& schema,
                                                      const std::string& interval) {
    TSDBContinuousAggregate ca;
    ca.view_name = schema.name + "_vwap_" + interval;
    for (char& c : ca.view_name) if (c == ' ') c = '_';

    ca.source_table = schema.name;
    ca.bucket_interval = interval;
    ca.group_by_columns = {"sym"};
    ca.aggregates = {
        {"vwap",   "sum(price * size) / sum(size)"},
        {"volume", "sum(size)"},
    };

    schema.continuous_aggregates.push_back(ca);
}

// ============================================================================
// Query Translator
// ============================================================================

std::string TimescaleDBQueryTranslator::translate(const std::string& apex_sql) {
    std::string result = apex_sql;
    result = rewrite_time_functions(result);
    result = rewrite_first_last(result);
    return result;
}

std::string TimescaleDBQueryTranslator::translate_time_bucket(
    const std::string& interval,
    const std::string& column)
{
    // TimescaleDB native function
    return "time_bucket('" + interval + "', " + column + ")";
}

std::string TimescaleDBQueryTranslator::translate_first_last(
    const std::string& func,
    const std::string& val_col,
    const std::string& time_col)
{
    if (func == "FIRST" || func == "first") {
        return "first(" + val_col + ", " + time_col + ")";
    } else {
        return "last(" + val_col + ", " + time_col + ")";
    }
}

std::string TimescaleDBQueryTranslator::translate_asof_join(
    const std::string& left_table,
    const std::string& right_table,
    const std::string& join_keys,
    const std::string& time_col)
{
    // TimescaleDB doesn't have ASOF JOIN natively
    // Use LATERAL or locf() from toolkit
    std::ostringstream ss;

    ss << "-- TimescaleDB ASOF JOIN equivalent using LATERAL\n";
    ss << "SELECT l.*, r.*\n";
    ss << "FROM " << left_table << " l\n";
    ss << "CROSS JOIN LATERAL (\n";
    ss << "    SELECT *\n";
    ss << "    FROM " << right_table << " r\n";
    ss << "    WHERE r." << join_keys << " = l." << join_keys << "\n";
    ss << "      AND r." << time_col << " <= l." << time_col << "\n";
    ss << "    ORDER BY r." << time_col << " DESC\n";
    ss << "    LIMIT 1\n";
    ss << ") r;\n";

    return ss.str();
}

std::string TimescaleDBQueryTranslator::generate_stats_agg_example(
    const std::string& table_name)
{
    std::ostringstream ss;

    ss << "-- TimescaleDB Toolkit: Statistical aggregates\n";
    ss << "SELECT\n";
    ss << "    sym,\n";
    ss << "    time_bucket('1 day', timestamp) AS day,\n";
    ss << "    stats_agg(price) AS price_stats\n";
    ss << "FROM " << table_name << "\n";
    ss << "GROUP BY sym, day;\n\n";

    ss << "-- Extract stats from stats_agg\n";
    ss << "SELECT\n";
    ss << "    sym, day,\n";
    ss << "    average(price_stats)  AS avg_price,\n";
    ss << "    stddev(price_stats)   AS std_price,\n";
    ss << "    skewness(price_stats) AS skew_price,\n";
    ss << "    kurtosis(price_stats) AS kurt_price\n";
    ss << "FROM (\n";
    ss << "    SELECT sym, time_bucket('1 day', timestamp) AS day,\n";
    ss << "           stats_agg(price) AS price_stats\n";
    ss << "    FROM " << table_name << "\n";
    ss << "    GROUP BY sym, day\n";
    ss << ") sub;\n";

    return ss.str();
}

std::string TimescaleDBQueryTranslator::generate_candlestick_example(
    const std::string& table_name)
{
    std::ostringstream ss;

    ss << "-- TimescaleDB Toolkit: Candlestick (OHLCV)\n";
    ss << "SELECT\n";
    ss << "    sym,\n";
    ss << "    time_bucket('5 minutes', timestamp) AS bar,\n";
    ss << "    candlestick_agg(timestamp, price, size) AS candle\n";
    ss << "FROM " << table_name << "\n";
    ss << "GROUP BY sym, bar;\n\n";

    ss << "-- Decompose candlestick\n";
    ss << "SELECT\n";
    ss << "    sym, bar,\n";
    ss << "    open(candle)      AS open,\n";
    ss << "    high(candle)      AS high,\n";
    ss << "    low(candle)       AS low,\n";
    ss << "    close(candle)     AS close,\n";
    ss << "    volume(candle)    AS volume,\n";
    ss << "    vwap(candle)      AS vwap\n";
    ss << "FROM (\n";
    ss << "    SELECT sym, time_bucket('5 minutes', timestamp) AS bar,\n";
    ss << "           candlestick_agg(timestamp, price, size) AS candle\n";
    ss << "    FROM " << table_name << " GROUP BY sym, bar\n";
    ss << ") c\n";
    ss << "ORDER BY sym, bar;\n";

    return ss.str();
}

std::string TimescaleDBQueryTranslator::rewrite_time_functions(const std::string& sql) {
    std::string result = sql;

    // xbar(col, N) → time_bucket('N seconds', col)
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

            std::string replacement =
                "time_bucket('" + interval + " seconds', " + col + ")";
            result.replace(pos, end - pos + 1, replacement);
        }
        ++pos;
    }

    return result;
}

std::string TimescaleDBQueryTranslator::rewrite_first_last(const std::string& sql) {
    std::string result = sql;

    // FIRST(col) → first(col, timestamp)
    // LAST(col)  → last(col, timestamp)
    for (const auto& [old_fn, new_fn] : std::vector<std::pair<std::string,std::string>>{
        {"FIRST(", "first("}, {"LAST(", "last("}
    }) {
        size_t pos = 0;
        while ((pos = result.find(old_fn, pos)) != std::string::npos) {
            size_t end = result.find(')', pos);
            if (end == std::string::npos) break;

            std::string col = result.substr(pos + old_fn.length(),
                                           end - pos - old_fn.length());
            std::string replacement = new_fn + col + ", timestamp)";
            result.replace(pos, end - pos + 1, replacement);
            ++pos;
        }
    }

    return result;
}

// ============================================================================
// Migration Runner
// ============================================================================

TimescaleDBMigrator::TimescaleDBMigrator(const Config& config)
    : config_(config)
{}

std::string TimescaleDBMigrator::generate_migration_sql() {
    std::ostringstream ss;

    ss << "-- ================================================================\n";
    ss << "-- APEX-DB → TimescaleDB Migration Script\n";
    ss << "-- Generated by apex-migrate\n";
    ss << "-- ================================================================\n\n";

    ss << "-- Prerequisites:\n";
    ss << "-- CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE;\n";
    ss << "-- CREATE EXTENSION IF NOT EXISTS timescaledb_toolkit CASCADE;\n\n";

    // Generate schemas for all requested tables
    std::vector<std::string> tables_to_migrate = config_.tables;
    if (tables_to_migrate.empty()) {
        tables_to_migrate = {"trades", "quotes", "orderbook"};
    }

    for (const auto& table_name : tables_to_migrate) {
        TSDBTableSchema schema;

        if (table_name == "trades") {
            schema = schema_gen_.generate_trades_schema(table_name);
        } else if (table_name == "quotes") {
            schema = schema_gen_.generate_quotes_schema(table_name);
        } else if (table_name == "orderbook") {
            schema = schema_gen_.generate_orderbook_schema(table_name);
        } else {
            // Generic schema
            schema.name = table_name;
            schema.time_column = "timestamp";
            schema.columns = {
                {"timestamp", "TIMESTAMPTZ", true, ""},
                {"sym",       "TEXT",        true, ""},
            };
        }

        if (!config_.create_continuous_aggregates) {
            schema.continuous_aggregates.clear();
        }

        if (!config_.add_retention_policy || config_.retention_days <= 0) {
            schema.retention.retain_days = 0;
        } else {
            schema.retention.table_name = table_name;
            schema.retention.retain_days = config_.retention_days;
        }

        if (!config_.enable_compression) {
            // Zero out compression
            schema.compression.segment_by_columns.clear();
            schema.compression.compress_after_days = 0;
        }

        ss << schema.to_full_setup_sql() << "\n";
    }

    // Query examples
    ss << "\n-- ================================================================\n";
    ss << "-- Example Queries\n";
    ss << "-- ================================================================\n\n";

    ss << translator_.generate_candlestick_example("trades") << "\n";
    ss << translator_.generate_stats_agg_example("trades") << "\n";

    return ss.str();
}

bool TimescaleDBMigrator::run() {
    // Generate migration SQL
    std::string migration_sql = generate_migration_sql();

    // Write to file
    std::string sql_file = "/tmp/apex_timescaledb_migration.sql";
    std::ofstream out(sql_file);
    if (!out) {
        std::cerr << "Failed to write migration SQL" << std::endl;
        return false;
    }

    out << migration_sql;
    out.close();

    std::cout << "Migration SQL written to: " << sql_file << "\n";
    std::cout << "Run with: psql \"" << config_.pg_connection_string
              << "\" -f " << sql_file << "\n";

    return true;
}

} // namespace apex::migration
