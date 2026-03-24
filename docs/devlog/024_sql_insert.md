# Devlog 024: SQL INSERT

**Date:** 2026-03-24
**Phase:** SQL DML

## Summary

Implemented SQL INSERT INTO. ClickHouse-compatible INSERT is now available via the HTTP API.

## Supported Syntax

```sql
-- Basic (default column order: symbol, price, volume, timestamp)
INSERT INTO trades VALUES (1, 15000, 100, 1000000000)

-- Multi-row
INSERT INTO trades VALUES (1, 15050, 200, 2000000000), (2, 16000, 300, 3000000000)

-- Column specification (timestamp auto-generated with current time if omitted)
INSERT INTO trades (symbol, price, volume) VALUES (1, 15100, 150)

-- HTTP API (ClickHouse compatible)
curl -X POST http://localhost:8123/ \
  -d "INSERT INTO trades VALUES (1, 15000, 100, 1000000000)"
```

## Implementation

| Layer | File | Change |
|-------|------|--------|
| Tokenizer | `tokenizer.h/cpp` | Added `INSERT`, `INTO`, `VALUES` tokens |
| AST | `ast.h` | `InsertStmt` struct, `ParsedStatement::Kind::INSERT` |
| Parser | `parser.h/cpp` | `parse_insert()`, column list / multi-row parsing |
| Executor | `executor.h/cpp` | `exec_insert()` → `pipeline_.ingest_tick()` + `drain_sync()` |
| HTTP | No changes | Automatically works via existing `executor_.execute(sql)` path |

## Design Decisions

1. **drain_sync() call**: Guarantees drain after INSERT so SELECT is immediately available
2. **Automatic timestamp generation**: Uses `now()` nanoseconds when timestamp is omitted from column list
3. **TickMessage mapping**: column name → TickMessage field mapping (symbol, price, volume, timestamp)
4. **No HTTP changes needed**: Existing `POST /` → `executor_.execute(sql)` path handles INSERT as well

## Test Results

- All existing 766/766 tests passed (no regression)
- Manual integration tests: single row, multi-row, column list, SELECT verification all passed

## Business Impact

- Compatible with existing INSERT pipelines from ClickHouse migration customers
- Removes first barrier for PoC (`curl -d "INSERT INTO ..."`)
- Enables integration with external tools such as Kafka Connect HTTP Sink
