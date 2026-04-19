// ============================================================================
// ZeptoDB: TimescaleDB Migration Toolkit
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace zeptodb::migration {

// ============================================================================
// TimescaleDB Compression Settings
// ============================================================================
struct TSDBCompressionPolicy {
    std::string order_by_column = "timestamp";
    std::vector<std::string> segment_by_columns;  // e.g., {"sym", "exchange"}
    int compress_after_days = 7;    // compress chunks older than N days
    int reorder_after_days = 1;     // reorder for better compression

    std::string to_sql(const std::string& table_name) const;
};

// ============================================================================
// TimescaleDB Continuous Aggregate
// ============================================================================
struct TSDBContinuousAggregate {
    std::string view_name;
    std::string source_table;
    std::string bucket_interval;   // e.g., "1 minute", "5 minutes", "1 hour"
    std::vector<std::string> group_by_columns;
    std::vector<std::pair<std::string, std::string>> aggregates;  // {name, expr}

    int refresh_every_minutes = 10;
    int lag_minutes = 0;           // how far behind real-time

    std::string to_create_sql() const;
    std::string to_refresh_policy_sql() const;
};

// ============================================================================
// TimescaleDB Retention Policy
// ============================================================================
struct TSDBRetentionPolicy {
    std::string table_name;
    int retain_days;
    std::string schedule_interval = "1 day";

    std::string to_sql() const;
};

// ============================================================================
// TimescaleDB Table Schema
// ============================================================================
struct TSDBTableSchema {
    std::string name;
    std::string schema_name = "public";

    // PostgreSQL columns
    struct Column {
        std::string name;
        std::string pg_type;
        bool not_null = false;
        std::string default_value;
    };
    std::vector<Column> columns;

    // Hypertable configuration
    std::string time_column;           // mandatory
    std::vector<std::string> space_partitions;  // optional (e.g., sym hash)
    int chunk_interval_days = 1;
    int space_partitions_count = 4;

    // Indexes
    struct Index {
        std::string name;
        std::vector<std::string> columns;
        bool unique = false;
    };
    std::vector<Index> indexes;

    // Compression
    TSDBCompressionPolicy compression;

    // Continuous aggregates
    std::vector<TSDBContinuousAggregate> continuous_aggregates;

    // Retention
    TSDBRetentionPolicy retention;

    // DDL generators
    std::string to_create_table_sql() const;
    std::string to_create_hypertable_sql() const;
    std::string to_create_indexes_sql() const;
    std::string to_enable_compression_sql() const;
    std::string to_full_setup_sql() const;
};

// ============================================================================
// ZeptoDB → TimescaleDB Type Mapping
// ============================================================================
class ZeptoToTSDBTypeMapper {
public:
    static std::string zepto_to_pg(const std::string& zepto_type);
    static std::string ktype_to_pg(int8_t kdb_type);
};

// ============================================================================
// Schema Generator
// ============================================================================
class TimescaleDBSchemaGenerator {
public:
    // Pre-built HFT schemas
    TSDBTableSchema generate_trades_schema(const std::string& table_name = "trades");
    TSDBTableSchema generate_quotes_schema(const std::string& table_name = "quotes");
    TSDBTableSchema generate_orderbook_schema(const std::string& table_name = "orderbook");

    // Generate from kdb+ schema
    TSDBTableSchema from_kdb_schema(
        const std::string& table_name,
        const std::vector<std::pair<std::string, int8_t>>& kdb_columns,
        const std::string& time_column = "timestamp");

    // Add standard continuous aggregates
    void add_ohlcv_aggregate(TSDBTableSchema& schema,
                             const std::string& interval = "1 minute");
    void add_vwap_aggregate(TSDBTableSchema& schema,
                            const std::string& interval = "1 minute");
};

// ============================================================================
// Query Translator: ZeptoDB SQL → TimescaleDB SQL
// ============================================================================
class TimescaleDBQueryTranslator {
public:
    std::string translate(const std::string& zepto_sql);

    // TimescaleDB-specific function translations
    std::string translate_time_bucket(const std::string& interval,
                                      const std::string& column);
    std::string translate_first_last(const std::string& func,
                                     const std::string& val_col,
                                     const std::string& time_col);
    std::string translate_asof_join(const std::string& left_table,
                                    const std::string& right_table,
                                    const std::string& join_keys,
                                    const std::string& time_col);

    // Generate hyperfunctions usage
    std::string generate_stats_agg_example(const std::string& table_name);
    std::string generate_candlestick_example(const std::string& table_name);

private:
    std::string rewrite_time_functions(const std::string& sql);
    std::string rewrite_first_last(const std::string& sql);
};

// ============================================================================
// Migration Runner
// ============================================================================
class TimescaleDBMigrator {
public:
    struct Config {
        std::string source_hdb_path;
        std::string pg_connection_string;
        // e.g., "host=localhost port=5432 dbname=hft user=admin"
        bool create_schema = true;
        bool create_continuous_aggregates = true;
        bool enable_compression = true;
        bool add_retention_policy = false;
        int retention_days = 365;
        std::vector<std::string> tables;
        // Destination ZeptoDB table name (Stage B, devlog 084). Empty =
        // legacy path; when set, stamped as `msg.table_id` on ingest.
        std::string dest_table;
    };

    explicit TimescaleDBMigrator(const Config& config);

    // Generate full migration SQL (no live DB connection required)
    std::string generate_migration_sql();

    // Run migration against PostgreSQL
    bool run();

private:
    Config config_;
    TimescaleDBSchemaGenerator schema_gen_;
    TimescaleDBQueryTranslator translator_;
};

} // namespace zeptodb::migration
