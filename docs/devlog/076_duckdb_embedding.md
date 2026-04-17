# Devlog 076: DuckDB Embedding — Columnar Analytical Offload

*Date: 2026-04-16*

## Summary

Embedded DuckDB as an in-process analytical engine for offloading queries on
cold/warm Parquet data. Uses columnar data conversion via the ArrowBridge layer.

## What Was Implemented

### DuckDB Engine Wrapper (`duckdb_engine.h/.cpp`)
- `DuckDBEngine` class with pimpl pattern to isolate DuckDB headers
- `execute(sql)` — run SQL, return result metadata
- `register_parquet(name, path)` — register Parquet file as virtual table
- Lazy initialization, configurable memory limit (default 256MB)
- Thread-safe: one DuckDB connection per query

### Arrow Bridge (`arrow_bridge.h/.cpp`)
- `columns_to_arrow_data()` — convert ZeptoDB ColumnStore to flat vectors
- `arrow_columns_to_result()` — convert column data back to QueryResultSet
- Supports INT64, FLOAT64, TIMESTAMP_NS, INT32, FLOAT32, SYMBOL types

### Query Router Integration (`executor.h/.cpp`)
- Added `DuckDBEngine` member to QueryExecutor (lazy-initialized)
- Added `exec_via_duckdb()` method for Parquet query offload
- Added `is_duckdb_table_func()` for explicit `duckdb('path')` syntax
- Config: `enable_duckdb_offload`, `duckdb_memory_limit_mb`
- Routing in `exec_select()`: after CTE check, before partition scan

### CMake Integration
- `ZEPTO_ENABLE_DUCKDB` option (default ON, can be disabled)
- DuckDB v1.2.1 via FetchContent
- New source files added to `zepto_execution` library
- DuckDB linked and `ZEPTO_ENABLE_DUCKDB=1` compile definition set

## Key Design Decisions

1. **Pimpl pattern** — DuckDB headers only included in `.cpp`, not leaked to consumers
2. **Conditional compilation** — All DuckDB code behind `#ifdef ZEPTO_ENABLE_DUCKDB`
3. **Lazy init** — DuckDB engine created on first use, not at startup
4. **Memory isolation** — DuckDB gets its own memory budget, separate from ZeptoDB arena
5. **FetchContent** — DuckDB pulled as full source (includes Parquet extension)

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/execution/duckdb_engine.h` | New — DuckDB wrapper header |
| `src/execution/duckdb_engine.cpp` | New — DuckDB wrapper implementation |
| `include/zeptodb/execution/arrow_bridge.h` | New — Arrow bridge header |
| `src/execution/arrow_bridge.cpp` | New — Arrow bridge implementation |
| `include/zeptodb/sql/executor.h` | Modified — DuckDB member, destructor, config |
| `src/sql/executor.cpp` | Modified — DuckDB routing, exec_via_duckdb |
| `CMakeLists.txt` | Modified — DuckDB FetchContent, option, linking |
| `docs/design/duckdb_embedding.md` | New — Design doc |

## Test Results

- All 1100 existing tests pass (DuckDB ON and OFF)
- No regressions
