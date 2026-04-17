#pragma once
// ============================================================================
// ZeptoDB: Arrow Bridge — Columnar Data Conversion
// ============================================================================
// Design doc: docs/design/duckdb_embedding.md
//
// Converts between ZeptoDB ColumnStore data and columnar vectors
// for data exchange with DuckDB.
//
// Supported types: INT64, FLOAT64, TIMESTAMP_NS, STRING (via dictionary)
// ============================================================================

#ifdef ZEPTO_ENABLE_DUCKDB

#include "zeptodb/storage/partition_manager.h"
// executor.h included for QueryResultSet (needed by the by-value overload).
// Prefer the 3-arg version (output reference) in new code to avoid this dependency.
#include "zeptodb/sql/executor.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zeptodb::execution {

/// Convert ZeptoDB partition columns to a DuckDB-compatible SQL query result.
/// Executes the given SQL against registered Parquet files and converts
/// the DuckDB result back to a ZeptoDB QueryResultSet.
///
/// This is the main bridge function used by QueryExecutor::exec_via_duckdb().

/// Convert DuckDB query results (column-oriented int64/double/string vectors)
/// back to a ZeptoDB QueryResultSet.
struct ArrowColumnData {
    std::string name;
    storage::ColumnType type;
    std::vector<int64_t> int_values;
    std::vector<double>  dbl_values;
    std::vector<std::string> str_values;
};

/// Build a QueryResultSet from column data returned by DuckDB.
/// Populates the provided output reference with rows and typed_rows.
void arrow_columns_to_result(
    const std::vector<ArrowColumnData>& columns,
    size_t num_rows,
    sql::QueryResultSet& out);

/// Convenience overload: returns QueryResultSet by value.
sql::QueryResultSet arrow_columns_to_result(
    const std::vector<ArrowColumnData>& columns,
    size_t num_rows);

/// Convert ZeptoDB ColumnStore columns to flat vectors for DuckDB registration.
std::vector<ArrowColumnData> columns_to_arrow_data(
    const storage::Partition& partition,
    const std::vector<std::string>& column_names);

} // namespace zeptodb::execution

#endif // ZEPTO_ENABLE_DUCKDB
