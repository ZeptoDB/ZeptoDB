// ============================================================================
// APEX-DB: DuckDB Interoperability Layer
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace apex::migration {

// ============================================================================
// DuckDB Export Formats
// ============================================================================
enum class DuckDBFormat {
    Parquet,    // Best for analytics / archival
    CSV,        // Universal compatibility
    Arrow,      // Zero-copy interop with pandas/polars
    JSON,       // Debugging / web
    NDJSON      // Streaming JSON
};

// ============================================================================
// Parquet Schema
// ============================================================================
struct ParquetColumn {
    std::string name;
    std::string logical_type;   // INT64, DOUBLE, BYTE_ARRAY, etc.
    std::string physical_type;
    bool required = true;
    std::string encoding = "PLAIN_DICTIONARY";
    std::string compression = "SNAPPY";

    std::string to_schema_string() const;
};

struct ParquetSchema {
    std::string table_name;
    std::vector<ParquetColumn> columns;

    // DuckDB CREATE TABLE AS SELECT from Parquet
    std::string to_duckdb_ddl() const;

    // Arrow schema (for pyarrow compatibility)
    std::string to_arrow_schema() const;
};

// ============================================================================
// APEX → DuckDB Type Mapping
// ============================================================================
class APEXToDuckDBTypeMapper {
public:
    static std::string apex_to_duckdb(const std::string& apex_type);
    static std::string apex_to_parquet(const std::string& apex_type);
    static std::string ktype_to_duckdb(int8_t kdb_type);
    static std::string ktype_to_parquet_physical(int8_t kdb_type);
    static std::string ktype_to_parquet_logical(int8_t kdb_type);
};

// ============================================================================
// DuckDB Query Adapter
// ============================================================================
class DuckDBQueryAdapter {
public:
    // Convert APEX SQL to DuckDB SQL
    // DuckDB is mostly ANSI SQL compliant, minimal changes needed
    std::string adapt(const std::string& apex_sql);

    // DuckDB-specific optimizations
    std::string add_parallelism_hints(const std::string& sql, int thread_count = 0);

    // Generate DuckDB Python script for analysis
    std::string generate_python_script(const std::string& table_name,
                                       const std::string& parquet_path,
                                       const std::string& query);

    // Generate DuckDB SQL to read Parquet files
    std::string read_parquet_sql(const std::string& parquet_path,
                                 const std::string& table_alias = "t");

    // Generate DuckDB SQL to read HDB directory (via Parquet)
    std::string read_hdb_directory_sql(const std::string& hdb_path,
                                       const std::string& table_name);

private:
    std::string rewrite_window_functions(const std::string& sql);
};

// ============================================================================
// Parquet Exporter
// ============================================================================
class ParquetExporter {
public:
    struct ExportOptions {
        std::string compression = "SNAPPY";  // SNAPPY, GZIP, ZSTD, BROTLI, LZ4
        int compression_level = -1;          // -1 = default
        size_t row_group_size = 100000;
        bool enable_dictionary = true;
        bool enable_statistics = true;
        bool write_arrow_schema = true;       // embed Arrow schema in metadata
        std::string partition_column;         // for partitioned export
    };

    explicit ParquetExporter(const ExportOptions& opts = {});

    // Generate DuckDB COPY TO command for Parquet export
    std::string generate_copy_command(const std::string& query,
                                      const std::string& output_path);

    // Generate DuckDB script for partitioned Parquet export
    std::string generate_partitioned_export(const std::string& table_name,
                                            const std::string& output_dir,
                                            const std::string& partition_col = "date");

    // Generate schema file for Arrow/Parquet compatibility
    ParquetSchema generate_schema(
        const std::string& table_name,
        const std::vector<std::pair<std::string, std::string>>& columns);

    // Generate kdb+ HDB → Parquet conversion script
    std::string generate_hdb_to_parquet_script(const std::string& hdb_path,
                                               const std::string& output_dir,
                                               const std::vector<std::string>& tables);

private:
    ExportOptions options_;
};

// ============================================================================
// DuckDB Integration
// ============================================================================
class DuckDBIntegrator {
public:
    struct Config {
        std::string apex_data_path;
        std::string duckdb_db_path = ":memory:";  // or file path
        int threads = 0;                           // 0 = auto
        size_t memory_limit_mb = 0;               // 0 = auto
        bool enable_progress_bar = true;
    };

    explicit DuckDBIntegrator(const Config& config);

    // Export APEX data to Parquet for DuckDB consumption
    bool export_to_parquet(const std::string& table_name,
                           const std::string& output_dir);

    // Generate complete DuckDB setup script
    std::string generate_setup_script(const std::string& parquet_dir);

    // Generate analytics examples for common HFT queries
    std::string generate_analytics_examples(const std::string& table_name);

    // Generate Python notebook template (Jupyter)
    std::string generate_jupyter_template(const std::string& parquet_dir);

private:
    Config config_;
    ParquetExporter parquet_exporter_;
    DuckDBQueryAdapter query_adapter_;
};

} // namespace apex::migration
