# Devlog 027: String Column Support (Dictionary-Encoded)

**Date:** 2026-03-25
**Status:** ✅ Complete (Tier 1 — LowCardinality dictionary encoding)

---

## Summary

Added dictionary-encoded string support to ZeptoDB. Users can now write `WHERE symbol = 'AAPL'` instead of integer IDs. Internally, strings are interned to `uint32` codes via `StringDictionary`, so storage and query performance are identical to integer columns.

## Motivation

- #1 user-facing gap: every demo required explaining "symbol 1 means AAPL"
- kdb+ uses symbols (interned strings) natively — matching this was required for kdb+ replacement claim
- Low-cardinality columns (symbol, exchange, side, currency) cover 95%+ of HFT string use cases

## Architecture

```
INSERT ... VALUES ('AAPL', 150.25, 100)
         │
         ▼
   ┌─────────────┐
   │ Parser       │  TokenType::STRING → InsertValue::STRING
   └──────┬──────┘
          ▼
   ┌─────────────┐
   │ Executor     │  StringDictionary::intern("AAPL") → uint32 code
   └──────┬──────┘
          ▼
   ┌─────────────┐
   │ ColumnStore  │  Stored as uint32 (same as SYMBOL)
   └──────┬──────┘
          ▼
   ┌─────────────┐
   │ HTTP Output  │  symbol_dict->lookup(code) → "AAPL" in JSON
   └─────────────┘
```

## Changes

| Component | File | Change |
|-----------|------|--------|
| StringDictionary | `include/zeptodb/storage/string_dictionary.h` | New — `intern()`, `lookup()`, `find()` |
| ColumnType | `include/zeptodb/storage/column_store.h` | Added `STRING` (uint32 dict code) |
| Partition | `include/zeptodb/storage/partition_manager.h` | Added `string_dict_` member |
| Pipeline | `include/zeptodb/core/pipeline.h` | Added global `symbol_dict_` |
| AST | `include/zeptodb/sql/ast.h` | `InsertValue::STRING`, `Expr::value_str/is_string` |
| Parser | `src/sql/parser.cpp` | String literal in INSERT values and WHERE |
| Executor | `src/sql/executor.cpp` | `intern()` on INSERT, `find()` on WHERE, DDL `STRING/VARCHAR/TEXT` |
| QueryResultSet | `include/zeptodb/sql/executor.h` | `symbol_dict` pointer for output resolution |
| HTTP Server | `src/server/http_server.cpp` | Resolve symbol codes to `"AAPL"` in JSON |
| Coordinator | `src/cluster/query_coordinator.cpp` | String WHERE falls through to scatter-gather |

## Key Decisions

1. **Global dictionary per Pipeline** (not per-partition) — same string always maps to same code, enabling partition routing by symbol_id
2. **`std::unordered_map<std::string, uint32_t>`** for index — initial version used `string_view` keys pointing into the `strings_` vector, which caused dangling pointers on vector reallocation. Fixed to own the key strings.
3. **Distributed: scatter-gather for string WHERE** — `extract_symbol_filter()` can't resolve string→code without a global registry, so string queries go Tier B. Each node resolves locally. Future: global symbol registry for Tier A direct routing.
4. **Backward compatible** — integer symbol IDs (`WHERE symbol = 1`) still work unchanged.

## Bug Found & Fixed

`StringDictionary` initially used `string_view` as hash map key, pointing into a `vector<string>`. When the vector grew and reallocated, all existing keys became dangling pointers. Same string inserted twice would get different codes. Fixed by using `string` keys.

## Test Coverage (29 tests)

**Unit (23 tests — `test_sql.cpp`):**
- INSERT with string symbol, multi-row INSERT
- WHERE string filter, not-found → empty result
- COUNT, SUM, AVG, MIN/MAX, VWAP with string WHERE
- GROUP BY symbol, ORDER BY, LIMIT
- Float price + string symbol combo
- SELECT *, SELECT symbol (dict resolution)
- Mixed int/string symbol coexistence
- Dictionary consistency (10x same symbol → dict size 1)
- Arithmetic expressions with string WHERE
- FIRST/LAST with string symbol
- Empty string, case sensitivity, 50 symbols

**Distributed (6 tests — `test_coordinator.cpp`):**
- Two-node scatter-gather COUNT with string INSERT
- Two-node string WHERE filter (AAPL on both nodes)
- Two-node SUM, VWAP with string WHERE
- String not found across cluster → 0
- Mixed int (legacy TickMessage) + string symbol across nodes

## Performance

No regression — benchmarks unchanged:
- xbar GROUP BY 1M rows: 11.20ms
- EMA 1M rows: 2.15ms
- Window JOIN 100K×100K: 11.00ms

Dictionary lookup is O(1) hash, intern is O(1) amortized. Zero overhead on query path since stored codes are uint32 (same as before).

## What's NOT Covered (Tier 2 — Future)

- Variable-length strings (free text, log messages)
- String comparison operators (`<`, `>`, `LIKE` on string columns)
- String functions on STRING columns (`UPPER`, `LOWER`, `CONCAT`)
- HDB persistence of dictionary (Parquet string column)
- Global symbol registry for distributed Tier A routing
