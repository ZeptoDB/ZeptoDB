#pragma once
// ============================================================================
// ZeptoDB: Embedded DuckDB Engine — Columnar Analytical Offload
// ============================================================================
// Design doc: docs/design/duckdb_embedding.md
//
// Thin wrapper around DuckDB C++ API for offloading analytical queries on
// cold/warm Parquet data. Uses ArrowBridge for columnar data conversion.
// ============================================================================

#ifdef ZEPTO_ENABLE_DUCKDB

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zeptodb::execution {

/// Configuration for the embedded DuckDB engine
struct DuckDBConfig {
    size_t memory_limit_mb = 256;
    int    threads         = 0;     // 0 = DuckDB auto
    std::string temp_directory;     // empty = DuckDB default
};

/// Result of a DuckDB query — column names + materialized data + error.
/// Data is stored as typed column vectors (int64/double/string).
struct DuckDBResult {
    std::vector<std::string> column_names;
    size_t num_rows    = 0;
    size_t num_columns = 0;
    std::string error;
    bool ok() const { return error.empty(); }

    // Materialized column data (one entry per column)
    std::vector<std::vector<int64_t>>     int_columns;
    std::vector<std::vector<double>>      dbl_columns;
    std::vector<std::vector<std::string>> str_columns;
    std::vector<int> column_type_hints;  // 0=int64, 1=double, 2=string, 3=ts_us, 4=ts_ms, 5=ts_s
};

/// Embedded DuckDB engine wrapper
/// Lazy-initialized, one connection per query for thread safety.
class DuckDBEngine {
public:
    explicit DuckDBEngine(const DuckDBConfig& config = {});
    ~DuckDBEngine();

    // Non-copyable
    DuckDBEngine(const DuckDBEngine&) = delete;
    DuckDBEngine& operator=(const DuckDBEngine&) = delete;

    /// Execute SQL and return result metadata
    DuckDBResult execute(const std::string& sql);

    /// Register a Parquet file/directory as a virtual table
    bool register_parquet(const std::string& table_name,
                          const std::string& parquet_path);

    /// Check if engine is initialized
    bool is_initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeptodb::execution

#endif // ZEPTO_ENABLE_DUCKDB
