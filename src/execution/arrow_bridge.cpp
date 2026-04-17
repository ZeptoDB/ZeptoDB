// ============================================================================
// ZeptoDB: Arrow Bridge Implementation
// ============================================================================

#ifdef ZEPTO_ENABLE_DUCKDB

#include "zeptodb/execution/arrow_bridge.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/common/logger.h"

#include <bit>

namespace zeptodb::execution {

using namespace zeptodb::storage;
using namespace zeptodb::sql;

// ============================================================================
// columns_to_arrow_data — ColumnStore → flat vectors
// ============================================================================
std::vector<ArrowColumnData> columns_to_arrow_data(
    const Partition& partition,
    const std::vector<std::string>& column_names)
{
    std::vector<ArrowColumnData> result;
    const size_t n = partition.num_rows();

    for (const auto& col_name : column_names) {
        const auto* cv = partition.get_column(col_name);
        if (!cv) continue;

        ArrowColumnData col;
        col.name = col_name;
        col.type = cv->type();

        switch (cv->type()) {
            case ColumnType::INT64:
            case ColumnType::TIMESTAMP_NS: {
                const auto* data = static_cast<const int64_t*>(cv->raw_data());
                col.int_values.assign(data, data + n);
                break;
            }
            case ColumnType::FLOAT64: {
                const auto* data = static_cast<const double*>(cv->raw_data());
                col.dbl_values.assign(data, data + n);
                break;
            }
            case ColumnType::INT32:
            case ColumnType::SYMBOL: {
                const auto* data = static_cast<const int32_t*>(cv->raw_data());
                col.int_values.resize(n);
                for (size_t i = 0; i < n; ++i)
                    col.int_values[i] = data[i];
                break;
            }
            case ColumnType::FLOAT32: {
                const auto* data = static_cast<const float*>(cv->raw_data());
                col.dbl_values.resize(n);
                for (size_t i = 0; i < n; ++i)
                    col.dbl_values[i] = data[i];
                break;
            }
            default:
                // BOOL, STRING — copy as int
                col.int_values.resize(n, 0);
                break;
        }
        result.push_back(std::move(col));
    }
    return result;
}

// ============================================================================
// arrow_columns_to_result — DuckDB column data → QueryResultSet
// ============================================================================
void arrow_columns_to_result(
    const std::vector<ArrowColumnData>& columns,
    size_t num_rows,
    sql::QueryResultSet& result)
{
    result.rows_scanned = num_rows;

    for (const auto& col : columns) {
        result.column_names.push_back(col.name);
        result.column_types.push_back(col.type);
    }

    if (num_rows == 0 || columns.empty()) return;

    // Build typed_rows for float-aware results
    const size_t ncols = columns.size();
    result.typed_rows.resize(num_rows);

    // Check if any column has string data
    bool has_strings = false;
    for (const auto& col : columns)
        if (!col.str_values.empty()) { has_strings = true; break; }

    for (size_t row = 0; row < num_rows; ++row) {
        result.typed_rows[row].resize(ncols);
        for (size_t c = 0; c < ncols; ++c) {
            const auto& col = columns[c];
            if (!col.str_values.empty()) {
                // String columns: store 0 in typed_rows, actual value in string_rows
                result.typed_rows[row][c] = QueryResultSet::Value(int64_t(0));
            } else if (!col.dbl_values.empty()) {
                result.typed_rows[row][c] = QueryResultSet::Value(col.dbl_values[row]);
            } else if (!col.int_values.empty()) {
                result.typed_rows[row][c] = QueryResultSet::Value(col.int_values[row]);
            } else {
                result.typed_rows[row][c] = QueryResultSet::Value(int64_t(0));
            }
        }
    }

    // Populate string_rows for string columns (one string per cell, row-major)
    if (has_strings) {
        for (size_t row = 0; row < num_rows; ++row) {
            for (size_t c = 0; c < ncols; ++c) {
                const auto& col = columns[c];
                if (!col.str_values.empty())
                    result.string_rows.push_back(col.str_values[row]);
            }
        }
    }

    // Also populate rows (int64 view) for backward compatibility
    result.rows.resize(num_rows);
    for (size_t row = 0; row < num_rows; ++row) {
        result.rows[row].resize(ncols);
        for (size_t c = 0; c < ncols; ++c) {
            const auto& col = columns[c];
            if (!col.dbl_values.empty()) {
                result.rows[row][c] = std::bit_cast<int64_t>(col.dbl_values[row]);
            } else if (!col.int_values.empty()) {
                result.rows[row][c] = col.int_values[row];
            } else {
                result.rows[row][c] = 0;
            }
        }
    }
}

// Convenience overload: returns QueryResultSet by value
sql::QueryResultSet arrow_columns_to_result(
    const std::vector<ArrowColumnData>& columns,
    size_t num_rows)
{
    sql::QueryResultSet result;
    arrow_columns_to_result(columns, num_rows, result);
    return result;
}

} // namespace zeptodb::execution

#endif // ZEPTO_ENABLE_DUCKDB
