# DuckDB Embedding — Columnar Analytical Offload

*Created: 2026-04-16*

## Overview

Embed DuckDB as an in-process analytical engine for offloading queries on cold/warm
Parquet data. Uses columnar data conversion between ZeptoDB and DuckDB via the
ArrowBridge layer.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  QueryExecutor::exec_select()                   │
│                                                 │
│  ┌──────────┐   cold/warm tier?   ┌──────────┐ │
│  │ Native   │ ←── NO ──────────── │  Router  │ │
│  │ Engine   │                     │          │ │
│  └──────────┘   YES ──────────→   │          │ │
│                                   └────┬─────┘ │
│                                        │       │
│  ┌─────────────────────────────────────▼─────┐ │
│  │  DuckDBEngine (lazy-initialized)            │ │
│  │  ┌─────────────────────────────────────┐  │ │
│  │  │ duckdb::DuckDB  (memory-limited)    │  │ │
│  │  │ register_parquet(name, path)        │  │ │
│  │  │ execute(sql) → DuckDBResult         │  │ │
│  │  └─────────────────────────────────────┘  │ │
│  └───────────────────────────────────────────┘ │
│                                                 │
│  ┌───────────────────────────────────────────┐ │
│  │  ArrowBridge                              │ │
│  │  columns_to_arrow_data()    — ColumnStore → Arrow │ │
│  │  arrow_columns_to_result() — Arrow → ResultSet   │ │
│  └───────────────────────────────────────────┘ │
└─────────────────────────────────────────────────┘
```

## Query Routing

Queries are routed to DuckDB when:

1. **Automatic**: The target table resolves to cold/warm tier Parquet files
   (detected via FlushManager tiering metadata)
2. **Explicit**: Table name matches `duckdb('path/to/parquet')` function syntax

All other queries use the native ZeptoDB engine (hot path, in-memory).

## Arrow Bridge

Columnar data conversion bridge between ZeptoDB ColumnStore and DuckDB result
sets. Data is copied between the two engines via typed column vectors
(int64/double/string). The ArrowBridge layer handles type mapping and
conversion to/from `QueryResultSet`.

Supported types: INT64, DOUBLE (FLOAT64), TIMESTAMP_NS, VARCHAR/STRING.

## Memory Isolation

DuckDB runs with a configurable memory budget (default 256 MB), separate from
ZeptoDB's arena allocator. Configured via `duckdb_memory_limit_mb` in
QueryExecutor config.

## Lifecycle

- **Lazy init**: DuckDB engine created on first use, not at startup
- **Thread safety**: One DuckDB connection per query execution (DuckDB handles
  internal parallelism)
- **Cleanup**: DuckDB instance destroyed with QueryExecutor

## Files

| File | Purpose |
|------|---------|
| `include/zeptodb/execution/duckdb_engine.h` | DuckDB wrapper header |
| `src/execution/duckdb_engine.cpp` | DuckDB wrapper implementation |
| `include/zeptodb/execution/arrow_bridge.h` | Arrow bridge header |
| `src/execution/arrow_bridge.cpp` | Arrow bridge implementation |
| `src/sql/executor.cpp` | Query routing integration |
| `include/zeptodb/sql/executor.h` | DuckDB member + config |
