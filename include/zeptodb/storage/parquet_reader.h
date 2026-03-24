#pragma once
// ============================================================================
// ParquetReader — Read Parquet files into ZeptoDB pipeline
// ============================================================================
// Reads .parquet files (local or from buffer) and returns rows as
// QueryResultSet, or ingests directly into a pipeline.
//
// Requires Apache Arrow/Parquet libraries (compile-time optional).
// When not available, returns error results.
// ============================================================================

#include "zeptodb/sql/executor.h"
#include "zeptodb/core/pipeline.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zeptodb::storage {

struct ParquetReadResult {
    std::vector<std::string>           column_names;
    std::vector<std::vector<int64_t>>  rows;
    size_t                             total_rows = 0;
    std::string                        error;

    bool ok() const { return error.empty(); }
};

class ParquetReader {
public:
    ParquetReader() = default;

    /// Read a Parquet file and return rows.
    /// @param path  Local file path to .parquet file
    /// @param limit Max rows to read (0 = all)
    ParquetReadResult read_file(const std::string& path, size_t limit = 0);

    /// Read a Parquet file and ingest rows into a pipeline.
    /// @return Number of rows ingested.
    size_t ingest_file(const std::string& path,
                       zeptodb::core::ZeptoPipeline& pipeline,
                       size_t limit = 0);
};

} // namespace zeptodb::storage
