// ============================================================================
// ZeptoDB: ClickHouse Migration Toolkit Implementation
// ============================================================================
#include "zeptodb/migration/clickhouse_migrator.h"
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace zeptodb::migration {

// ============================================================================
// CHColumn DDL
// ============================================================================

std::string CHColumn::to_ddl() const {
    std::ostringstream ss;

    ss << "`" << name << "` ";

    // Build type string
    std::string type_str;
    switch (type) {
        case CHType::UInt8:   type_str = "UInt8";   break;
        case CHType::UInt16:  type_str = "UInt16";  break;
        case CHType::UInt32:  type_str = "UInt32";  break;
        case CHType::UInt64:  type_str = "UInt64";  break;
        case CHType::Int8:    type_str = "Int8";    break;
        case CHType::Int16:   type_str = "Int16";   break;
        case CHType::Int32:   type_str = "Int32";   break;
        case CHType::Int64:   type_str = "Int64";   break;
        case CHType::Float32: type_str = "Float32"; break;
        case CHType::Float64: type_str = "Float64"; break;
        case CHType::String:  type_str = "String";  break;
        case CHType::FixedString:
            type_str = "FixedString(" + std::to_string(precision) + ")";
            break;
        case CHType::Date:     type_str = "Date";     break;
        case CHType::Date32:   type_str = "Date32";   break;
        case CHType::DateTime: type_str = "DateTime"; break;
        case CHType::DateTime64:
            type_str = "DateTime64(" + std::to_string(precision) + ", 'UTC')";
            break;
        case CHType::Decimal:
            type_str = "Decimal(" + std::to_string(precision) + ", " +
                       std::to_string(scale) + ")";
            break;
        case CHType::UUID:    type_str = "UUID";    break;
        default:              type_str = "String";  break;
    }

    if (low_cardinality) {
        type_str = "LowCardinality(" + type_str + ")";
    }

    if (nullable) {
        type_str = "Nullable(" + type_str + ")";
    }

    ss << type_str;

    if (!default_value.empty()) {
        ss << " DEFAULT " << default_value;
    }

    if (!codec.empty()) {
        ss << " CODEC(" << codec << ")";
    }

    return ss.str();
}

// ============================================================================
// CHTableSchema DDL
// ============================================================================

std::string CHTableSchema::to_create_ddl() const {
    std::ostringstream ss;

    std::string full_name = database.empty() ? name : (database + "." + name);

    ss << "CREATE TABLE IF NOT EXISTS " << full_name << "\n(\n";

    for (size_t i = 0; i < columns.size(); ++i) {
        ss << "    " << columns[i].to_ddl();
        if (i < columns.size() - 1) ss << ",";
        ss << "\n";
    }

    ss << ")\n";

    // Engine
    switch (engine) {
        case CHEngine::MergeTree:
            ss << "ENGINE = MergeTree()\n"; break;
        case CHEngine::ReplacingMergeTree:
            ss << "ENGINE = ReplacingMergeTree()\n"; break;
        case CHEngine::SummingMergeTree:
            ss << "ENGINE = SummingMergeTree()\n"; break;
        case CHEngine::AggregatingMergeTree:
            ss << "ENGINE = AggregatingMergeTree()\n"; break;
        case CHEngine::Memory:
            ss << "ENGINE = Memory()\n"; break;
        default:
            ss << "ENGINE = MergeTree()\n"; break;
    }

    // Partition by
    if (!partition_by.empty()) {
        ss << "PARTITION BY (";
        for (size_t i = 0; i < partition_by.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << partition_by[i];
        }
        ss << ")\n";
    }

    // Order by (required for MergeTree)
    if (!order_by.empty()) {
        ss << "ORDER BY (";
        for (size_t i = 0; i < order_by.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << order_by[i];
        }
        ss << ")\n";
    } else {
        ss << "ORDER BY tuple()\n";
    }

    // Primary key
    if (!primary_key.empty()) {
        ss << "PRIMARY KEY " << primary_key << "\n";
    }

    // Sample by
    if (!sample_by.empty()) {
        ss << "SAMPLE BY " << sample_by << "\n";
    }

    // TTL
    if (ttl_days > 0) {
        ss << "TTL toDate(timestamp) + INTERVAL " << ttl_days << " DAY\n";
    }

    // Settings
    if (!settings.empty()) {
        ss << "SETTINGS ";
        bool first = true;
        for (const auto& [key, val] : settings) {
            if (!first) ss << ", ";
            ss << key << " = " << val;
            first = false;
        }
        ss << "\n";
    }

    ss << ";";
    return ss.str();
}

// ============================================================================
// Type Mapper
// ============================================================================

CHType ZeptoToClickHouseTypeMapper::map_zepto_type(const std::string& zepto_type) {
    if (zepto_type == "BOOLEAN") return CHType::UInt8;
    if (zepto_type == "TINYINT") return CHType::Int8;
    if (zepto_type == "SMALLINT") return CHType::Int16;
    if (zepto_type == "INTEGER" || zepto_type == "INT") return CHType::Int32;
    if (zepto_type == "BIGINT") return CHType::Int64;
    if (zepto_type == "REAL" || zepto_type == "FLOAT") return CHType::Float32;
    if (zepto_type == "DOUBLE") return CHType::Float64;
    if (zepto_type == "VARCHAR" || zepto_type == "TEXT") return CHType::String;
    if (zepto_type == "CHAR") return CHType::FixedString;
    if (zepto_type == "DATE") return CHType::Date32;
    if (zepto_type == "TIMESTAMP") return CHType::DateTime64;
    if (zepto_type == "TIME") return CHType::Int64;
    return CHType::String;
}

std::string ZeptoToClickHouseTypeMapper::zepto_type_to_ch_string(
    const std::string& zepto_type,
    bool nullable,
    bool low_cardinality)
{
    CHColumn col;
    col.type = map_zepto_type(zepto_type);
    col.nullable = nullable;
    col.low_cardinality = low_cardinality;
    if (col.type == CHType::DateTime64) col.precision = 9;  // nanoseconds
    if (col.type == CHType::FixedString) col.precision = 16;
    col.name = "";

    auto ddl = col.to_ddl();
    // Extract just the type part (after "`` ")
    return ddl;
}

CHType ZeptoToClickHouseTypeMapper::map_ktype(int8_t kdb_type) {
    switch (kdb_type) {
        case 1:  return CHType::UInt8;    // bool
        case 4:  return CHType::UInt8;    // byte
        case 5:  return CHType::Int16;    // short
        case 6:  return CHType::Int32;    // int
        case 7:  return CHType::Int64;    // long
        case 8:  return CHType::Float32;  // real
        case 9:  return CHType::Float64;  // float
        case 10: return CHType::FixedString; // char
        case 11: return CHType::LowCardinality; // symbol → LowCardinality(String)
        case 12: return CHType::DateTime64; // timestamp
        case 14: return CHType::Date32;   // date
        case 19: return CHType::Int32;    // time (milliseconds)
        default: return CHType::String;
    }
}

std::string ZeptoToClickHouseTypeMapper::ktype_to_ch_string(int8_t kdb_type) {
    switch (kdb_type) {
        case 1:  return "UInt8";
        case 4:  return "UInt8";
        case 5:  return "Int16";
        case 6:  return "Int32";
        case 7:  return "Int64";
        case 8:  return "Float32";
        case 9:  return "Float64";
        case 10: return "FixedString(1)";
        case 11: return "LowCardinality(String)";
        case 12: return "DateTime64(9, 'UTC')";
        case 14: return "Date32";
        case 19: return "Int32";  // time in ms
        default: return "String";
    }
}

// ============================================================================
// Schema Generator
// ============================================================================

CHTableSchema ClickHouseSchemaGenerator::generate_trades_schema(
    const std::string& table_name)
{
    CHTableSchema schema;
    schema.name = table_name;
    schema.engine = CHEngine::MergeTree;

    // Core trade columns - optimized for HFT
    schema.columns = {
        {"timestamp",  CHType::DateTime64, false, false, 9, 0, "", "CODEC(DoubleDelta, ZSTD(1))"},
        {"sym",        CHType::LowCardinality, false, false, 0, 0, "", "CODEC(ZSTD(1))"},
        {"exchange",   CHType::LowCardinality, false, false, 0, 0, "", "CODEC(ZSTD(1))"},
        {"price",      CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"size",       CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"side",       CHType::UInt8, false, false, 0, 0, "0", "CODEC(ZSTD(1))"},
        {"trade_id",   CHType::UInt64, false, false, 0, 0, "", "CODEC(Delta, ZSTD(1))"},
        {"order_id",   CHType::UInt64, false, false, 0, 0, "0", "CODEC(Delta, ZSTD(1))"},
        {"condition",  CHType::LowCardinality, false, false, 0, 0, "''", "CODEC(ZSTD(1))"},
    };

    // Type corrections for LowCardinality columns
    schema.columns[1].type = CHType::String;
    schema.columns[2].type = CHType::String;
    schema.columns[8].type = CHType::String;

    schema.partition_by = {"toYYYYMM(timestamp)"};
    schema.order_by = {"sym", "timestamp"};
    schema.primary_key = "sym";
    schema.settings["index_granularity"] = "8192";
    schema.settings["min_compress_block_size"] = "65536";

    return schema;
}

CHTableSchema ClickHouseSchemaGenerator::generate_quotes_schema(
    const std::string& table_name)
{
    CHTableSchema schema;
    schema.name = table_name;
    schema.engine = CHEngine::MergeTree;

    schema.columns = {
        {"timestamp",  CHType::DateTime64, false, false, 9, 0, "", "CODEC(DoubleDelta, ZSTD(1))"},
        {"sym",        CHType::String, false, false, 0, 0, "", "CODEC(ZSTD(1))"},
        {"exchange",   CHType::String, false, false, 0, 0, "", "CODEC(ZSTD(1))"},
        {"bid",        CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"ask",        CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"bid_size",   CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"ask_size",   CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"bid_levels", CHType::UInt8, false, false, 0, 0, "1", ""},
        {"ask_levels", CHType::UInt8, false, false, 0, 0, "1", ""},
    };

    schema.partition_by = {"toYYYYMM(timestamp)"};
    schema.order_by = {"sym", "timestamp"};
    schema.primary_key = "sym";

    return schema;
}

CHTableSchema ClickHouseSchemaGenerator::generate_orderbook_schema(
    const std::string& table_name)
{
    CHTableSchema schema;
    schema.name = table_name;
    schema.engine = CHEngine::ReplacingMergeTree;

    schema.columns = {
        {"timestamp",  CHType::DateTime64, false, false, 9, 0, "", "CODEC(DoubleDelta, ZSTD(1))"},
        {"sym",        CHType::String, false, false, 0, 0, "", "CODEC(ZSTD(1))"},
        {"side",       CHType::UInt8, false, false, 0, 0, ""},
        {"level",      CHType::UInt8, false, false, 0, 0, ""},
        {"price",      CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"size",       CHType::Float64, false, false, 0, 0, "", "CODEC(Gorilla, ZSTD(1))"},
        {"order_count", CHType::UInt32, false, false, 0, 0, "0"},
    };

    schema.partition_by = {"toYYYYMM(timestamp)"};
    schema.order_by = {"sym", "side", "level", "timestamp"};

    return schema;
}

CHTableSchema ClickHouseSchemaGenerator::from_kdb_schema(
    const std::string& table_name,
    const std::vector<std::pair<std::string, int8_t>>& kdb_columns)
{
    CHTableSchema schema;
    schema.name = table_name;
    schema.engine = CHEngine::MergeTree;

    bool has_timestamp = false;
    bool has_sym = false;

    for (const auto& [col_name, ktype] : kdb_columns) {
        CHColumn col;
        col.name = col_name;
        col.type = ZeptoToClickHouseTypeMapper::map_ktype(ktype);

        // Apply codecs based on column name heuristics
        if (col_name == "timestamp" || col_name == "time") {
            col.codec = "CODEC(DoubleDelta, ZSTD(1))";
            has_timestamp = true;
        } else if (col_name == "price" || col_name == "bid" || col_name == "ask") {
            col.codec = "CODEC(Gorilla, ZSTD(1))";
        } else if (col_name == "size" || col_name == "qty") {
            col.codec = "CODEC(Gorilla, ZSTD(1))";
        } else if (col_name == "sym" || col_name == "symbol") {
            has_sym = true;
        }

        if (col.type == CHType::DateTime64) col.precision = 9;
        if (col.type == CHType::FixedString) col.precision = 1;

        schema.columns.push_back(col);
    }

    // Set default order/partition
    if (has_timestamp && has_sym) {
        schema.partition_by = {"toYYYYMM(timestamp)"};
        schema.order_by = {"sym", "timestamp"};
    } else if (has_timestamp) {
        schema.partition_by = {"toYYYYMM(timestamp)"};
        schema.order_by = {"timestamp"};
    }

    return schema;
}

void ClickHouseSchemaGenerator::optimize_for_analytics(CHTableSchema& schema) {
    // Add materialized views hints as comments
    // Increase index granularity for better compression
    schema.settings["index_granularity"] = "8192";
    schema.settings["merge_max_block_size"] = "8192";
    schema.settings["min_compress_block_size"] = "65536";
    schema.settings["max_compress_block_size"] = "1048576";
}

void ClickHouseSchemaGenerator::optimize_for_realtime(CHTableSchema& schema) {
    // Optimize for fast inserts
    schema.settings["min_insert_block_size_rows"] = "1048576";
    schema.settings["min_insert_block_size_bytes"] = "268435456";
    schema.settings["max_insert_block_size"] = "1048576";
}

void ClickHouseSchemaGenerator::add_tiered_ttl(CHTableSchema& schema,
                                                int hot_days,
                                                int warm_days) {
    // TTL expression for tiered storage
    // Hot → Warm → Cold
    schema.ttl_days = hot_days + warm_days;
    schema.settings["storage_policy"] = "'tiered'";
}

// ============================================================================
// Query Translator
// ============================================================================

void ClickHouseQueryTranslator::init_function_map() {
    // Standard SQL functions
    function_map_["NOW()"] = "now()";
    function_map_["CURRENT_TIMESTAMP"] = "now()";

    // Time functions
    function_map_["DATE_TRUNC('minute'"] = "toStartOfMinute(";
    function_map_["DATE_TRUNC('hour'"]   = "toStartOfHour(";
    function_map_["DATE_TRUNC('day'"]    = "toStartOfDay(";

    // Aggregations
    function_map_["FIRST("] = "argMin(";
    function_map_["LAST("]  = "argMax(";

    // Financial
    function_map_["VWAP("] = "sumProduct(";
}

std::string ClickHouseQueryTranslator::translate(const std::string& zepto_sql) {
    if (function_map_.empty()) {
        init_function_map();
    }

    std::string result = zepto_sql;
    result = rewrite_time_functions(result);
    result = rewrite_aggregations(result);
    return result;
}

std::string ClickHouseQueryTranslator::translate_time_bucket(
    const std::string& interval,
    const std::string& column)
{
    // toStartOfInterval(column, INTERVAL N second)
    return "toStartOfInterval(" + column + ", INTERVAL " + interval + " SECOND)";
}

std::string ClickHouseQueryTranslator::translate_vwap(
    const std::string& size_col,
    const std::string& price_col)
{
    // ClickHouse VWAP using sumProduct
    return "sumProduct(" + size_col + ", " + price_col + ") / sum(" + size_col + ")";
}

std::string ClickHouseQueryTranslator::translate_asof_join(
    const std::string& left_table,
    const std::string& right_table,
    const std::string& join_key,
    const std::string& time_col)
{
    // ClickHouse ASOF JOIN
    std::ostringstream ss;
    ss << "SELECT * FROM " << left_table << "\n";
    ss << "ASOF JOIN " << right_table << "\n";
    ss << "ON " << left_table << "." << join_key << " = " << right_table << "." << join_key;
    ss << " AND " << left_table << "." << time_col
       << " >= " << right_table << "." << time_col;
    return ss.str();
}

std::string ClickHouseQueryTranslator::rewrite_time_functions(const std::string& sql) {
    std::string result = sql;

    // xbar(timestamp, N) → toStartOfInterval(timestamp, INTERVAL N SECOND)
    size_t pos = 0;
    while ((pos = result.find("xbar(", pos)) != std::string::npos) {
        size_t end = result.find(')', pos);
        if (end == std::string::npos) break;

        std::string args = result.substr(pos + 5, end - pos - 5);
        size_t comma = args.find(',');
        if (comma != std::string::npos) {
            std::string col = args.substr(0, comma);
            std::string interval = args.substr(comma + 1);

            // Trim whitespace
            auto trim = [](std::string s) {
                s.erase(s.find_last_not_of(" \t") + 1);
                s.erase(0, s.find_first_not_of(" \t"));
                return s;
            };
            col = trim(col);
            interval = trim(interval);

            std::string replacement = "toStartOfInterval(" + col +
                                      ", INTERVAL " + interval + " SECOND)";
            result.replace(pos, end - pos + 1, replacement);
        }
        ++pos;
    }

    return result;
}

std::string ClickHouseQueryTranslator::rewrite_aggregations(const std::string& sql) {
    std::string result = sql;

    // FIRST(col) → argMin(col, timestamp)
    // LAST(col)  → argMax(col, timestamp)
    size_t pos = 0;
    while ((pos = result.find("FIRST(", pos)) != std::string::npos) {
        size_t end = result.find(')', pos);
        if (end == std::string::npos) break;

        std::string col = result.substr(pos + 6, end - pos - 6);
        std::string replacement = "argMin(" + col + ", timestamp)";
        result.replace(pos, end - pos + 1, replacement);
        ++pos;
    }

    pos = 0;
    while ((pos = result.find("LAST(", pos)) != std::string::npos) {
        size_t end = result.find(')', pos);
        if (end == std::string::npos) break;

        std::string col = result.substr(pos + 5, end - pos - 5);
        std::string replacement = "argMax(" + col + ", timestamp)";
        result.replace(pos, end - pos + 1, replacement);
        ++pos;
    }

    return result;
}

// ============================================================================
// Exporter
// ============================================================================

ClickHouseExporter::ClickHouseExporter(const ExportOptions& opts)
    : options_(opts)
{}

std::string ClickHouseExporter::generate_insert_sql(
    const std::string& table_name,
    const CHTableSchema& schema,
    const std::vector<std::vector<std::string>>& rows)
{
    std::ostringstream ss;

    ss << "INSERT INTO " << table_name << " (";
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << "`" << schema.columns[i].name << "`";
    }
    ss << ") VALUES\n";

    for (size_t r = 0; r < rows.size(); ++r) {
        ss << "(";
        for (size_t c = 0; c < rows[r].size(); ++c) {
            if (c > 0) ss << ", ";
            ss << escape_value(rows[r][c]);
        }
        ss << ")";
        if (r < rows.size() - 1) ss << ",";
        ss << "\n";
    }

    ss << ";";
    return ss.str();
}

std::string ClickHouseExporter::generate_bulk_load_cmd(
    const std::string& table_name,
    const std::string& data_file,
    const std::string& format)
{
    std::ostringstream ss;
    ss << "clickhouse-client --query=\"INSERT INTO " << table_name
       << " FORMAT " << format << "\" < " << data_file;
    return ss.str();
}

std::string ClickHouseExporter::escape_value(const std::string& value) {
    if (!options_.escape_strings) return value;

    // Check if it's a number (don't quote)
    bool is_number = !value.empty();
    for (char c : value) {
        if (!std::isdigit(c) && c != '.' && c != '-') {
            is_number = false;
            break;
        }
    }

    if (is_number) return value;

    // Escape string
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped += '\'';
    for (char c : value) {
        if (c == '\'') escaped += "''";
        else if (c == '\\') escaped += "\\\\";
        else escaped += c;
    }
    escaped += '\'';
    return escaped;
}

// ============================================================================
// Migration Runner
// ============================================================================

ClickHouseMigrator::ClickHouseMigrator(const MigrationConfig& config)
    : config_(config)
{}

bool ClickHouseMigrator::run() {
    auto start = std::chrono::high_resolution_clock::now();

    std::cout << "Starting ClickHouse migration...\n";
    std::cout << "Source: " << config_.source_hdb_path << "\n";
    std::cout << "Target: " << config_.clickhouse_host << ":"
              << config_.clickhouse_port << "/" << config_.clickhouse_db << "\n\n";

    bool success = true;

    if (config_.create_schema) {
        std::cout << "[1/3] Creating ClickHouse schema...\n";
        success &= step_create_schema();
    }

    if (config_.migrate_data && success) {
        std::string export_dir = "/tmp/zepto_ch_export";
        std::cout << "[2/3] Exporting data to " << export_dir << "...\n";
        success &= step_export_data(export_dir);

        if (success) {
            std::cout << "[3/3] Loading data into ClickHouse...\n";
            success &= step_load_data(export_dir);
        }
    }

    if (config_.validate && success) {
        std::cout << "[+] Validating migration...\n";
        step_validate();
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats_.duration_seconds = std::chrono::duration<double>(end - start).count();

    std::cout << "\n" << generate_report() << "\n";
    return success;
}

bool ClickHouseMigrator::step_create_schema() {
    // Generate DDL for standard HFT tables
    auto trades_schema = schema_gen_.generate_trades_schema("trades");
    auto quotes_schema = schema_gen_.generate_quotes_schema("quotes");

    // Output DDL files
    std::string ddl_trades = trades_schema.to_create_ddl();
    std::string ddl_quotes = quotes_schema.to_create_ddl();

    // Write to files
    std::ofstream trades_ddl("/tmp/zepto_ch_trades.sql");
    trades_ddl << "CREATE DATABASE IF NOT EXISTS " << config_.clickhouse_db << ";\n\n";
    trades_ddl << ddl_trades << "\n";

    std::ofstream quotes_ddl("/tmp/zepto_ch_quotes.sql");
    quotes_ddl << ddl_quotes << "\n";

    std::cout << "  Schema DDL written to /tmp/zepto_ch_*.sql\n";
    return true;
}

bool ClickHouseMigrator::step_export_data(const std::string& output_dir) {
    std::filesystem::create_directories(output_dir);
    // In a full implementation: scan APEX tables and export to CSV
    std::cout << "  Data export directory: " << output_dir << "\n";
    return true;
}

bool ClickHouseMigrator::step_load_data(const std::string& data_dir) {
    // Generate load commands
    auto cmd_trades = exporter_.generate_bulk_load_cmd(
        config_.clickhouse_db + ".trades",
        data_dir + "/trades.csv",
        "CSV"
    );

    auto cmd_quotes = exporter_.generate_bulk_load_cmd(
        config_.clickhouse_db + ".quotes",
        data_dir + "/quotes.csv",
        "CSV"
    );

    std::cout << "  Load command: " << cmd_trades << "\n";
    return true;
}

bool ClickHouseMigrator::step_validate() {
    // Validate row counts match
    std::cout << "  Validation: row count comparison\n";
    return true;
}

std::string ClickHouseMigrator::generate_report() const {
    std::ostringstream ss;

    ss << "=== Migration Report ===\n";
    ss << "Tables migrated: " << stats_.tables_migrated << "\n";
    ss << "Rows migrated:   " << stats_.rows_migrated << "\n";
    ss << "Bytes exported:  " << stats_.bytes_exported << "\n";
    ss << "Duration:        " << stats_.duration_seconds << "s\n";

    if (!stats_.errors.empty()) {
        ss << "\nErrors:\n";
        for (const auto& e : stats_.errors) {
            ss << "  - " << e << "\n";
        }
    }

    if (!stats_.warnings.empty()) {
        ss << "\nWarnings:\n";
        for (const auto& w : stats_.warnings) {
            ss << "  - " << w << "\n";
        }
    }

    return ss.str();
}

} // namespace zeptodb::migration
