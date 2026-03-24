# Devlog 021 — DDL / Schema Management

**Date:** 2026-03-23
**Status:** Complete
**Tests:** 615/615 pass (+8 new DDL tests)

---

## Background

ZeptoDB previously had no user-visible schema management:
- The `store_tick()` path always wrote exactly 4 hardcoded columns: `timestamp`, `price`, `volume`, `msg_type`
- There was no concept of "table" beyond the implicit all-symbols partition store
- Memory retention was unbounded — partitions from any era remained in memory forever

This is a critical gap for production use: operators need to define schemas, manage column sets, and enforce retention policies to bound memory consumption.

---

## What Was Built

### 1. `SchemaRegistry` (header-only)

`include/zeptodb/storage/schema_registry.h`

Thread-safe (shared_mutex) per-pipeline store mapping `table_name → TableSchema`.

```cpp
struct ColumnDef {
    std::string name;
    ColumnType  type;
};

struct TableSchema {
    std::string            table_name;
    std::vector<ColumnDef> columns;
    int64_t                ttl_ns = 0;  // 0 = no retention limit
};
```

API: `create()`, `drop()`, `exists()`, `get()`, `add_column()`, `drop_column()`, `set_ttl()`, `min_ttl_ns()`, `list_tables()`

`SchemaRegistry schema_registry_` is a member of `ZeptoPipeline` (accessible via `pipeline.schema_registry()`).

### 2. DDL AST nodes

`include/zeptodb/sql/ast.h`

```cpp
struct DdlColumnDef { std::string column; std::string type_str; };

struct CreateTableStmt { std::string table_name; std::vector<DdlColumnDef> columns; bool if_not_exists; };
struct DropTableStmt   { std::string table_name; bool if_exists; };
struct AlterTableStmt  { enum class Action { ADD_COLUMN, DROP_COLUMN, SET_TTL }; ... };

struct ParsedStatement {
    enum class Kind { SELECT, CREATE_TABLE, DROP_TABLE, ALTER_TABLE };
    Kind kind;
    std::optional<SelectStmt>      select;
    std::optional<CreateTableStmt> create_table;
    std::optional<DropTableStmt>   drop_table;
    std::optional<AlterTableStmt>  alter_table;
};
```

### 3. Parser: `parse_statement()` + DDL dispatch

`Parser::parse_statement(sql)` → `ParsedStatement`

Detects DDL by checking the first token's uppercase value (`CREATE`, `DROP`, `ALTER`) without adding new tokenizer keywords — avoids conflicts with user column names.

`Parser::parse()` is preserved for backward compatibility (wraps `parse_statement()` and extracts the `SelectStmt`; throws if DDL is given).

DDL grammar supported:
```sql
CREATE TABLE [IF NOT EXISTS] name (col TYPE [, col TYPE ...])
DROP TABLE [IF EXISTS] name
ALTER TABLE name ADD [COLUMN] col TYPE
ALTER TABLE name DROP [COLUMN] col
ALTER TABLE name SET TTL n DAYS|HOURS
```

### 4. DDL Executor

`QueryExecutor::execute()` dispatches to `exec_create_table()`, `exec_drop_table()`, `exec_alter_table()` based on `ParsedStatement::kind`.

DDL results return `QueryResultSet::string_rows = {"Table 'x' created"}` — same format as EXPLAIN — so HTTP clients and zepto-cli display them consistently.

### 5. TTL Eviction (two-phase)

**Immediate eviction** on `ALTER TABLE SET TTL`:
- `exec_alter_table()` calls `pipeline_.evict_older_than_ns(now - ttl_ns)`
- `evict_older_than_ns()` calls `PartitionManager::evict_older_than()` then rebuilds `partition_index_` to remove stale raw pointers

**Continuous eviction** via `FlushManager`:
- `FlushManager::set_ttl(int64_t ttl_ns)` — atomic setter
- `flush_loop()` checks `ttl_ns_` every `check_interval_ms` and calls `pm_.evict_older_than(now - ttl)` — forward-only, no data skew

`PartitionManager::evict_older_than(cutoff_ns)` — removes partitions with `key.hour_epoch < cutoff_ns` under mutex.

**Critical fix:** `evict_older_than_ns()` in `ZeptoPipeline` rebuilds `partition_index_` after eviction. Without this, `total_stored_rows()` would dereference freed partitions (UAF / UB). Discovered and fixed during test run.

---

## SQL Examples

```sql
-- Create a table
CREATE TABLE trades (time TIMESTAMP, sym SYMBOL, price INT64, size INT64);

-- Create only if it doesn't already exist
CREATE TABLE IF NOT EXISTS trades (time TIMESTAMP);

-- Evolve schema: add a column
ALTER TABLE trades ADD COLUMN venue SYMBOL;

-- Evolve schema: remove a column
ALTER TABLE trades DROP COLUMN venue;

-- Set 30-day retention policy (evicts old partitions immediately + continuously)
ALTER TABLE trades SET TTL 30 DAYS;

-- Set 6-hour retention (useful for ultra-recent-only workloads)
ALTER TABLE trades SET TTL 6 HOURS;

-- Remove table
DROP TABLE IF EXISTS trades;
```

---

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/storage/schema_registry.h` | New header-only `SchemaRegistry` |
| `include/zeptodb/storage/partition_manager.h` | Added `evict_older_than(cutoff_ns)` |
| `src/storage/partition_manager.cpp` | Implemented `evict_older_than()` |
| `include/zeptodb/sql/ast.h` | Added DDL AST: `DdlColumnDef`, `CreateTableStmt`, `DropTableStmt`, `AlterTableStmt`, `ParsedStatement` |
| `include/zeptodb/sql/parser.h` | Added `parse_statement()` + DDL private methods |
| `src/sql/parser.cpp` | Implemented `parse_statement()`, DDL dispatch, `parse_create_table()`, `parse_drop_table()`, `parse_alter_table()`, `parse_ddl_column_def()` |
| `include/zeptodb/core/pipeline.h` | Added `#include schema_registry.h`; `schema_registry_` member; `schema_registry()` accessor; `evict_older_than_ns()` declaration |
| `src/core/pipeline.cpp` | Implemented `evict_older_than_ns()` with `partition_index_` rebuild |
| `include/zeptodb/storage/flush_manager.h` | Added `set_ttl()` / `ttl_ns()` + `ttl_ns_` atomic |
| `src/storage/flush_manager.cpp` | TTL eviction in `flush_loop()` |
| `include/zeptodb/sql/executor.h` | Added DDL method declarations |
| `src/sql/executor.cpp` | `exec_create_table()`, `exec_drop_table()`, `exec_alter_table()` + updated `execute()` to use `parse_statement()` |
| `tests/unit/test_sql.cpp` | 8 new DDL tests |
| `BACKLOG.md` | Marked CREATE/DROP TABLE, retention policy, schema evolution complete |

---

## Tests (8 new)

| Test | Verifies |
|------|----------|
| `CreateTable_Basic` | Schema stored correctly; 4 columns; table visible in registry |
| `CreateTable_IfNotExists` | IF NOT EXISTS skips error; without it → error |
| `DropTable_Basic` | Table removed from registry after DROP |
| `DropTable_IfExists` | IF EXISTS skips error; without it → error |
| `AlterTable_AddColumn` | Column appended to schema; count goes from 2 → 3 |
| `AlterTable_DropColumn` | Column removed; dropping again → error |
| `AlterTable_SetTTL_Days` | TTL stored as 30×86400×1e9 nanoseconds |
| `AlterTable_SetTTL_Evicts_OldPartitions` | Epoch-0 partition evicted; current-day partition survives |

---

## Lessons Learned

1. **`partition_index_` is a raw-pointer cache** — any operation that removes partitions from `PartitionManager` must also invalidate this cache. Discovered via UB/garbage value in `total_stored_rows()` test. Fix: `evict_older_than_ns()` rebuilds `partition_index_` after eviction.

2. **DDL keywords as IDENT** — adding `CREATE`, `ALTER`, `DROP`, `TABLE`, `COLUMN` as tokenizer keywords would conflict with user column names (e.g., a column named `table`). Checking `to_upper(current().value)` on `IDENT` tokens in context is the correct approach for a context-sensitive grammar.

3. **Type stored as string in AST** — `DdlColumnDef::type_str` (not `ColumnType`) avoids adding a `storage::ColumnType` dependency in the parser/AST layer. The executor converts at execution time via `ddl_type_from_str()`.

---

## Next Steps

- `SHOW TABLES` / `DESCRIBE TABLE` — introspection SQL
- Schema-aware `INSERT INTO` — enforce schema on ingest rather than hardcoded 4 columns
- `ALTER TABLE SET TTL 0` — disable TTL (clear retention)
- HDB Compaction — merge small `.bin` files from old partitions
