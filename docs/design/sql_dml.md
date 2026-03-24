# ZeptoDB SQL DML Design

Last updated: 2026-03-24

---

## Overview

ZeptoDB supports full SQL DML (Data Manipulation Language) via the HTTP API and SQL executor. All DML statements are executed through the same `POST /` endpoint used for SELECT queries, maintaining ClickHouse wire protocol compatibility.

```
Client                    ZeptoDB
  │                         │
  │  POST / body="INSERT.." │
  │ ───────────────────────>│
  │                         │  Tokenizer → Parser → AST
  │                         │  Executor dispatch by Kind
  │                         │  INSERT: ingest_tick() + drain_sync()
  │                         │  UPDATE: in-place span write
  │                         │  DELETE: compact + set_size()
  │  {"inserted": 3}        │
  │ <───────────────────────│
```

---

## Supported Statements

### INSERT INTO

```sql
-- Default column order: symbol, price, volume, timestamp
INSERT INTO trades VALUES (1, 15000, 100, 1711234567000000000)

-- Multi-row
INSERT INTO trades VALUES (1, 15050, 200, 1711234568000000000),
                          (2, 16000, 300, 1711234569000000000)

-- Column list (timestamp auto-generated if omitted)
INSERT INTO trades (symbol, price, volume) VALUES (1, 15100, 150)
```

**Response:** `{"columns":["inserted"],"rows":[[3]]}`

### UPDATE

```sql
-- Single column
UPDATE trades SET price = 15200 WHERE symbol = 1 AND price > 15100

-- Multiple columns
UPDATE trades SET price = 15200, volume = 500 WHERE symbol = 1 AND price > 15100
```

**Response:** `{"columns":["updated"],"rows":[[2]]}`

### DELETE

```sql
-- Conditional delete
DELETE FROM trades WHERE symbol = 1 AND price < 15000

-- Delete all rows for a symbol
DELETE FROM trades WHERE symbol = 1
```

**Response:** `{"columns":["deleted"],"rows":[[5]]}`

---

## Architecture

### Processing Pipeline

```
SQL String
    │
    ▼
┌──────────┐
│ Tokenizer│  INSERT/INTO/VALUES/UPDATE/SET/DELETE tokens
└────┬─────┘
     ▼
┌──────────┐
│  Parser  │  InsertStmt / UpdateStmt / DeleteStmt AST nodes
└────┬─────┘
     ▼
┌──────────┐
│ Executor │  Dispatch by ParsedStatement::Kind
└────┬─────┘
     │
     ├── INSERT → pipeline_.ingest_tick() + drain_sync()
     ├── UPDATE → find_partitions() → eval_where() → span[idx] = val
     └── DELETE → find_partitions() → eval_where() → compact + set_size()
```

### AST Nodes

```cpp
// INSERT
struct InsertStmt {
    std::string              table_name;
    std::vector<std::string> columns;              // optional
    std::vector<std::vector<int64_t>> value_rows;  // each row
};

// UPDATE
struct UpdateAssign {
    std::string column;
    int64_t     value;
};
struct UpdateStmt {
    std::string                table_name;
    std::vector<UpdateAssign>  assignments;
    std::optional<WhereClause> where;
};

// DELETE
struct DeleteStmt {
    std::string                table_name;
    std::optional<WhereClause> where;
};
```

---

## Implementation Details

### INSERT

1. Parse column list (optional) or use default order: `symbol, price, volume, timestamp`
2. For each value row, construct a `TickMessage`:
   - Map column names to TickMessage fields
   - If `timestamp` is omitted, use `std::chrono::system_clock::now()` in nanoseconds
3. Call `pipeline_.ingest_tick(msg)` for each row
4. Call `pipeline_.drain_sync()` to ensure data is stored before returning
5. Return count of inserted rows

### UPDATE (In-Place Modification)

```
For each partition:
    1. Skip if symbol filter doesn't match partition key
    2. eval_where() → matching row indices
    3. For each assignment (col = val):
       span = col->as_span<int64_t>()
       for idx in matching: span[idx] = val
```

- Zero-copy: directly writes to arena memory via span
- O(matched_rows) per column per partition
- No memory allocation needed

### DELETE (In-Place Compaction)

```
For each partition:
    1. Skip if symbol filter doesn't match partition key
    2. eval_where() → matching row indices
    3. Build delete mask: del_mask[idx] = true
    4. For each column:
       Shift kept rows down (compact in-place)
       col->set_size(new_size)
```

- Arena memory is not freed (append-only allocator)
- Logical size shrinks via `set_size()`
- Freed space is not reusable until partition is reclaimed

### Symbol Partition Filtering

ZeptoDB stores `symbol` as a partition key, not as a column within the partition. This means `WHERE symbol = X` cannot be evaluated by `eval_where()` against column data.

Solution: Before iterating partitions, extract the symbol filter using `has_where_symbol()` and skip non-matching partitions at the partition level.

```cpp
int64_t sym_filter = -1;
has_where_symbol(sel, sym_filter, "");

for (auto* part : partitions) {
    if (sym_filter >= 0 && part->key().symbol_id != sym_filter)
        continue;  // skip non-matching partition
    // ... eval_where for remaining conditions
}
```

---

## HTTP API

All DML uses the same endpoint as SELECT:

```bash
# INSERT
curl -X POST http://localhost:8123/ \
  -d "INSERT INTO trades VALUES (1, 15000, 100, 1711234567000000000)"

# UPDATE
curl -X POST http://localhost:8123/ \
  -d "UPDATE trades SET price = 15200 WHERE symbol = 1 AND price > 15100"

# DELETE
curl -X POST http://localhost:8123/ \
  -d "DELETE FROM trades WHERE symbol = 1 AND price < 15000"
```

Response format (JSON):
```json
{
  "columns": ["inserted"],
  "types": ["INT64"],
  "data": [[3]],
  "rows": 1,
  "time_us": 45.2
}
```

---

## Distributed DML Routing

When QueryCoordinator receives a DML statement, it processes it through a different path than SELECT:

```
QueryCoordinator.execute_sql(sql)
    │
    ├── INSERT → extract symbol → route to owning node
    │            (no symbol → first node)
    │
    ├── UPDATE/DELETE → extract symbol → route to owning node
    │                   (no symbol → broadcast all nodes, sum results)
    │
    ├── CREATE/DROP/ALTER → broadcast all nodes
    │
    └── SELECT → existing scatter/gather logic
```

This ensures:
- Prevents the bug where INSERT duplicates data across all nodes
- UPDATE/DELETE is applied only to the correct node based on symbol
- DDL (CREATE TABLE, MATERIALIZED VIEW, etc.) is applied consistently across all nodes

---

## Limitations

| Limitation | Reason | Workaround |
|-----------|--------|------------|
| Integer values only | ColumnVector stores int64_t | Use fixed-point (price × 10000) |
| No RETURNING clause | Not implemented | Follow with SELECT |
| DELETE doesn't free arena memory | Append-only arena allocator | Partition reclaim on flush/eviction |
| No transaction / rollback | Single-threaded DML execution | Application-level retry |
| UPDATE/DELETE without WHERE affects all rows in all partitions | By design | Always include WHERE clause |

---

## Compatibility

| Feature | ClickHouse | ZeptoDB |
|---------|-----------|---------|
| INSERT INTO ... VALUES | ✅ | ✅ |
| INSERT INTO ... SELECT | ✅ | ❌ (backlog) |
| ALTER TABLE UPDATE | ✅ (mutation) | ✅ (in-place) |
| ALTER TABLE DELETE | ✅ (mutation) | ✅ (in-place) |
| UPDATE ... SET | ❌ | ✅ |
| DELETE FROM ... WHERE | ❌ | ✅ |

Note: ClickHouse uses `ALTER TABLE ... UPDATE/DELETE` (async mutations). ZeptoDB uses standard SQL `UPDATE/DELETE` (synchronous, in-place).
