// ============================================================================
// ZeptoDB: ClickHouse Migration Toolkit
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstdint>

namespace zeptodb::migration {

// ============================================================================
// ClickHouse Column Types
// ============================================================================
enum class CHType {
    UInt8, UInt16, UInt32, UInt64,
    Int8,  Int16,  Int32,  Int64,
    Float32, Float64,
    String, FixedString,
    Date, Date32, DateTime, DateTime64,
    Decimal,
    UUID,
    Nullable,
    Array,
    LowCardinality
};

// ============================================================================
// ClickHouse Table Engine
// ============================================================================
enum class CHEngine {
    MergeTree,
    ReplacingMergeTree,
    SummingMergeTree,
    AggregatingMergeTree,
    CollapsingMergeTree,
    VersionedCollapsingMergeTree,
    ReplicatedMergeTree,
    Distributed,
    Memory,
    Log
};

// ============================================================================
// Table Schema
// ============================================================================
struct CHColumn {
    std::string name;
    CHType type;
    bool nullable = false;
    bool low_cardinality = false;
    int precision = 0;  // for Decimal/DateTime64
    int scale = 0;
    std::string default_value;
    std::string codec;  // CODEC(ZSTD, LZ4, etc.)

    std::string to_ddl() const;
};

struct CHTableSchema {
    std::string name;
    std::string database;
    CHEngine engine = CHEngine::MergeTree;
    std::vector<CHColumn> columns;
    std::vector<std::string> order_by;
    std::vector<std::string> partition_by;
    std::string primary_key;
    std::string sample_by;
    int32_t ttl_days = 0;
    std::unordered_map<std::string, std::string> settings;

    std::string to_create_ddl() const;
};

// ============================================================================
// ZeptoDB → ClickHouse Type Mapping
// ============================================================================
class ZeptoToClickHouseTypeMapper {
public:
    static CHType map_zepto_type(const std::string& zepto_type);
    static std::string zepto_type_to_ch_string(const std::string& zepto_type,
                                               bool nullable = false,
                                               bool low_cardinality = false);

    // kdb+ specific mappings
    static CHType map_ktype(int8_t kdb_type);
    static std::string ktype_to_ch_string(int8_t kdb_type);
};

// ============================================================================
// Schema Generator
// ============================================================================
class ClickHouseSchemaGenerator {
public:
    // Generate optimal ClickHouse schema for HFT data
    CHTableSchema generate_trades_schema(const std::string& table_name = "trades");
    CHTableSchema generate_quotes_schema(const std::string& table_name = "quotes");
    CHTableSchema generate_orderbook_schema(const std::string& table_name = "orderbook");

    // Generate from existing ZeptoDB table schema
    CHTableSchema from_zepto_schema(const std::string& table_name,
                                   const std::vector<std::pair<std::string, std::string>>& zepto_columns);

    // Generate from kdb+ table schema
    CHTableSchema from_kdb_schema(const std::string& table_name,
                                  const std::vector<std::pair<std::string, int8_t>>& kdb_columns);

    // Optimize schema for ClickHouse
    void optimize_for_analytics(CHTableSchema& schema);
    void optimize_for_realtime(CHTableSchema& schema);
    void add_tiered_ttl(CHTableSchema& schema, int hot_days, int warm_days);
};

// ============================================================================
// Query Translator: ZeptoDB SQL → ClickHouse SQL
// ============================================================================
class ClickHouseQueryTranslator {
public:
    // Translate ZeptoDB SQL to ClickHouse SQL
    std::string translate(const std::string& zepto_sql);

    // Translate specific constructs
    std::string translate_window_function(const std::string& func,
                                          const std::string& args);
    std::string translate_time_bucket(const std::string& interval,
                                      const std::string& column);
    std::string translate_vwap(const std::string& size_col,
                               const std::string& price_col);
    std::string translate_asof_join(const std::string& left_table,
                                    const std::string& right_table,
                                    const std::string& join_key,
                                    const std::string& time_col);

private:
    // Function mappings: ZeptoDB → ClickHouse
    std::unordered_map<std::string, std::string> function_map_;

    void init_function_map();
    std::string rewrite_time_functions(const std::string& sql);
    std::string rewrite_aggregations(const std::string& sql);
};

// ============================================================================
// Data Exporter: ZeptoDB → ClickHouse native format
// ============================================================================
class ClickHouseExporter {
public:
    struct ExportOptions {
        std::string format;
        bool include_header;
        char delimiter;
        std::string null_value;
        bool escape_strings;
        size_t batch_size;
        ExportOptions()
            : format("CSV"), include_header(true), delimiter(','),
              null_value("\\N"), escape_strings(true), batch_size(100000) {}
    };

    explicit ClickHouseExporter(const ExportOptions& opts = ExportOptions{});

    // Export to file
    bool export_to_file(const std::string& table_name,
                        const std::string& output_file);

    // Export to ClickHouse HTTP endpoint
    bool export_to_http(const std::string& table_name,
                        const std::string& clickhouse_url,
                        const std::string& target_table);

    // Generate INSERT statements
    std::string generate_insert_sql(const std::string& table_name,
                                    const CHTableSchema& schema,
                                    const std::vector<std::vector<std::string>>& rows);

    // Generate COPY command (for bulk load)
    std::string generate_bulk_load_cmd(const std::string& table_name,
                                       const std::string& data_file,
                                       const std::string& format = "CSV");

private:
    ExportOptions options_;

    std::string escape_value(const std::string& value);
    std::string format_timestamp(int64_t ns_epoch);
};

// ============================================================================
// Migration Runner
// ============================================================================
class ClickHouseMigrator {
public:
    struct MigrationConfig {
        struct ExportOptions {
            std::string format = "CSV";
            size_t batch_size = 100000;
        };

        std::string source_hdb_path;
        std::string clickhouse_host = "localhost";
        int clickhouse_port = 8123;
        std::string clickhouse_db = "zepto_migration";
        std::string clickhouse_user = "default";
        std::string clickhouse_password;
        bool create_schema = true;
        bool migrate_data = true;
        bool validate = true;
        std::vector<std::string> tables;  // empty = all tables
        ExportOptions export_opts;
    };

    using Config = MigrationConfig;

    explicit ClickHouseMigrator(const MigrationConfig& config);

    // Run full migration
    bool run();

    // Step-by-step
    bool step_create_schema();
    bool step_export_data(const std::string& output_dir);
    bool step_load_data(const std::string& data_dir);
    bool step_validate();

    // Generate migration report
    std::string generate_report() const;

private:
    MigrationConfig config_;
    ClickHouseSchemaGenerator schema_gen_;
    ClickHouseExporter exporter_;

    struct MigrationStats {
        size_t tables_migrated = 0;
        size_t rows_migrated = 0;
        size_t bytes_exported = 0;
        double duration_seconds = 0.0;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    } stats_;

    bool migrate_table(const std::string& table_name);
};

} // namespace zeptodb::migration
