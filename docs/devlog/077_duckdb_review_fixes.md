# Devlog 077: DuckDB Embedding — Review Fixes (CRITICAL + MAJOR)

*Date: 2026-04-16*

## Summary

Fixed all CRITICAL and MAJOR review findings from the DuckDB Embedding
code review (devlog 076). Addresses SQL injection, empty result sets,
path traversal, and misleading documentation.

## Fixes Applied

### CRITICAL 1 + MAJOR 6: `exec_via_duckdb()` returns empty results

**Problem:** `DuckDBEngine::execute()` iterated chunks with `Fetch()` to count
rows but discarded the data. `exec_via_duckdb()` only copied column names and
row count — no actual row data. `ArrowBridge` functions existed but were never called.

**Fix:**
- Added materialized data fields to `DuckDBResult`: `int_columns`, `dbl_columns`,
  `str_columns`, `column_type_hints` (0=int64, 1=double, 2=string)
- `execute()` now materializes all chunk data using `GetValue<T>()` per column type
- `exec_via_duckdb()` converts `DuckDBResult` data to `ArrowColumnData` and calls
  `arrow_columns_to_result()` to populate `rows` and `typed_rows`

### CRITICAL 2: SQL injection in `register_parquet()`

**Problem:** `parquet_path` and `table_name` concatenated into SQL without escaping.

**Fix:** Added `escape_sql_string()` (replaces `'` → `''`) and
`escape_sql_identifier()` (replaces `"` → `""`) helpers. Applied to both
`table_name` and `parquet_path` in the `CREATE VIEW` statement.

### CRITICAL 3: SQL injection in `exec_via_duckdb()`

**Problem:** `parquet_path` concatenated into `read_parquet()` SQL without escaping.

**Fix:** Applied `'` → `''` escaping to `parquet_path` before building the
DuckDB SQL string in `exec_select()`.

### MAJOR 4: No path traversal validation

**Problem:** Extracted parquet path passed directly to DuckDB without validation.

**Fix:** Added validation in `exec_select()` after `is_duckdb_table_func()`:
- Rejects paths containing `..`
- Rejects absolute paths (starting with `/`)
- Logs `ZEPTO_WARN` on rejection
- Returns `QueryResultSet` with error message

### MAJOR 5: `arrow_bridge.h` includes `executor.h`

**Problem:** Heavyweight transitive dependency just for `QueryResultSet` type.

**Fix:** Added 3-arg `arrow_columns_to_result(columns, num_rows, out)` overload
that takes `QueryResultSet&` as output parameter. The by-value 2-arg overload is
retained for backward compatibility (existing tests depend on it). Added comment
explaining the include rationale. New internal code uses the 3-arg version.

### MAJOR 7: "Zero-copy" claim is false

**Problem:** Header comment said "zero-copy" but implementation always copies
with `.assign()`.

**Fix:** Updated `arrow_bridge.h` header comment from "Zero-Copy Data Exchange"
to "Columnar Data Conversion". Removed all "zero-copy" claims from the header.

### MINOR 8: Concurrent view name collision

**Problem:** `exec_via_duckdb()` uses `pq_0` etc. view names; concurrent calls
could collide.

**Fix:** `exec_via_duckdb()` now holds `duckdb_mu_` for the entire call
(initialization + register + execute), not just initialization.

### MINOR 10: Unused `#include <cstring>` in `arrow_bridge.cpp`

**Fix:** Removed.

### MINOR 11: `is_duckdb_table_func()` uses `substr()` instead of `starts_with()`

**Fix:** Replaced `table_name.substr(0, prefix.size()) == prefix` with
`table_name.starts_with(prefix)` (C++20).

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/execution/duckdb_engine.h` | Added data fields to `DuckDBResult` |
| `src/execution/duckdb_engine.cpp` | Materialize data in `execute()`, SQL escape helpers |
| `include/zeptodb/execution/arrow_bridge.h` | Fixed comment, added 3-arg overload |
| `src/execution/arrow_bridge.cpp` | Added `executor.h` include, 3-arg impl, removed `<cstring>` |
| `src/sql/executor.cpp` | Data conversion, path validation, SQL escaping, mutex scope, `starts_with()` |

## Test Results

- All 1121 existing tests pass
- All 21 DuckDB/Arrow tests pass
- No regressions
