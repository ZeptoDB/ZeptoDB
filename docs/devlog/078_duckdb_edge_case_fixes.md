# 066 — DuckDB Embedding Edge Case Fixes

**Date:** 2026-04-16
**Status:** Complete

## Summary

Fixed 5 edge cases in the DuckDB embedding implementation that caused crashes, data corruption, or incorrect query results.

## Changes

### Fix #1: NULL crash in `DuckDBEngine::execute()`
- **File:** `src/execution/duckdb_engine.cpp`
- **Problem:** `vec.GetValue(r).GetValue<T>()` throws `InternalException` on NULL values. Any Parquet file with NULL cells would crash.
- **Fix:** Check `val.IsNull()` before extracting typed values. Push sentinel values for NULLs:
  - int64: `INT64_MIN` (ZeptoDB NULL convention)
  - double: `NaN`
  - string: `""`

### Fix #2: Timestamp unit mismatch
- **File:** `src/execution/duckdb_engine.cpp`
- **Problem:** DuckDB TIMESTAMP returns microseconds, but ZeptoDB expects nanoseconds.
- **Fix:** Added type hints 3/4/5 for TIMESTAMP/TIMESTAMP_MS/TIMESTAMP_SEC with appropriate multipliers (1000/1000000/1000000000). TIMESTAMP_NS falls through to plain int64 (already nanoseconds).
- **Header:** Updated `duckdb_engine.h` comment to document new hint values.

### Fix #3: String serialization in `exec_via_duckdb()`
- **File:** `src/sql/executor.cpp`
- **Problem:** DuckDB VARCHAR columns were typed as `ColumnType::STRING`, but the HTTP serializer only picks up strings for `ColumnType::SYMBOL` columns.
- **Fix:** Changed `col.type = storage::ColumnType::STRING` to `storage::ColumnType::SYMBOL` in the `case 2:` block.

### Fix #4: Lost WHERE/ORDER BY/LIMIT
- **File:** `src/sql/executor.cpp`
- **Problem:** DuckDB SQL was hardcoded as `SELECT * FROM read_parquet('path')`, discarding the user's WHERE, ORDER BY, LIMIT, and column selection.
- **Fix:** Route DuckDB result through `exec_select_virtual()` which applies all SQL clauses from the original AST on the in-memory result set.

### Fix #5: Whitespace-only path rejection
- **File:** `src/sql/executor.cpp`
- **Problem:** `duckdb(' ')` passed validation but failed at DuckDB with an unhelpful error.
- **Fix:** Added whitespace-only check immediately after `is_duckdb_table_func()` returns true, before path traversal validation.

## Build & Test

- Build: `ninja -j$(nproc) zepto_tests` — success (0 errors)
- Tests: 1121/1121 passed, 0 failures
