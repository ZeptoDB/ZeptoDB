# String Column Implementation Plan

> Status: ✅ Tier 1 Complete (2026-03-25) | Priority: P1 | 726 tests passing (29 string tests)

---

## Current State

- `ColumnVector` is fixed-size only (`void* data_`, arena-allocated contiguous array)
- `ColumnType::SYMBOL` exists (uint32) but is only used as `PartitionKey.symbol_id` (int32)
- `trades` table has no string column — symbol is partition key, not queryable as string
- `QueryResultSet.rows` is `vector<vector<int64_t>>` — no string cell support
- `string_rows` exists but is flat (EXPLAIN plan only), not per-cell

## Design: Two-Tier Approach

### Tier 1: Dictionary-Encoded String (LowCardinality) — Primary

For low-cardinality columns like symbol, exchange, side, currency.
This is what 95% of HFT/quant use cases need.

```
Storage:
  ┌─────────────────────────────────┐
  │ StringDictionary (per-partition) │
  │  0 → "AAPL"                     │
  │  1 → "GOOGL"                    │
  │  2 → "TSLA"                     │
  └─────────────────────────────────┘
           │
  ┌────────▼────────────────────────┐
  │ ColumnVector<uint32_t> codes    │
  │  [0, 1, 2, 0, 1, 0, 2, ...]    │  ← existing fixed-size column!
  └─────────────────────────────────┘

Query: WHERE symbol = 'AAPL'
  → dictionary lookup: 'AAPL' → code 0
  → scan_compare<uint32_t>(codes, ==, 0)   ← reuses existing SIMD path!
```

**Why this is brilliant for ZeptoDB:**
- `codes` column is `uint32_t` → fits existing `ColumnVector` with zero changes
- WHERE/GROUP BY/JOIN operate on integer codes → existing SIMD scan works
- Dictionary is small (hundreds of entries) → fits in L1 cache
- Only string↔code translation at INSERT and SELECT boundaries

### Tier 2: Variable-Length String (Offset+Data) — Future

For high-cardinality columns like order_id, client_name, comment.
Deferred — not needed for initial launch.

```
Storage:
  offsets:  [0, 5, 10, 15]        ← int64 array
  data:     [A,P,P,L,E,G,O,O,G,L,T,S,L,A,!]  ← byte buffer
```

---

## Implementation Plan (Tier 1 Only)

### Phase 1: Storage Layer

**1.1 StringDictionary class**

```cpp
// include/zeptodb/storage/string_dictionary.h
class StringDictionary {
    std::vector<std::string>                    strings_;   // id → string
    std::unordered_map<std::string_view, uint32_t> index_; // string → id

public:
    // Returns existing code or inserts new entry
    uint32_t intern(std::string_view s);

    // Lookup by code
    std::string_view lookup(uint32_t code) const;

    // Lookup by string (returns -1 if not found)
    int64_t find(std::string_view s) const;

    size_t size() const;
};
```

**1.2 Partition gets a dictionary**

```cpp
// In Partition class:
StringDictionary& string_dict() { return string_dict_; }

// One dictionary per partition (not global) — avoids cross-partition locking
```

**1.3 ColumnType::STRING added**

```cpp
enum class ColumnType : uint8_t {
    // ... existing ...
    STRING,  // dictionary-encoded, stored as uint32 codes
};
// column_type_size(STRING) = 4  (same as SYMBOL)
```

Files changed: `column_store.h`

### Phase 2: SQL Parser

**2.1 String literals in WHERE**

```sql
WHERE symbol = 'AAPL'           -- string comparison
WHERE exchange IN ('NYSE', 'NASDAQ')
WHERE symbol LIKE 'AA%'
```

Parser already handles single-quoted strings for LIKE. Extend to COMPARE:

```cpp
// In Expr:
std::string value_str;    // string literal value
bool is_string = false;   // flag for string comparison
```

**2.2 String literals in INSERT**

```sql
INSERT INTO trades (symbol, price, volume) VALUES ('AAPL', 150.25, 100)
```

Extend `InsertValue`:
```cpp
struct InsertValue {
    int64_t i = 0;
    double  f = 0.0;
    std::string s;
    enum Type { INT, FLOAT, STRING } type = INT;
};
```

**2.3 DDL type recognition**

```sql
CREATE TABLE trades (
    symbol STRING,          -- or VARCHAR, TEXT
    price  FLOAT64,
    volume INT64,
    ts     TIMESTAMP
)
```

Add to `ddl_type_from_str`: `"STRING" | "VARCHAR" | "TEXT" → ColumnType::STRING`

Files changed: `ast.h`, `parser.cpp`

### Phase 3: Executor — Write Path

**3.1 INSERT with string**

```
INSERT 'AAPL'
  → parser: InsertValue{.s="AAPL", .type=STRING}
  → exec_insert: dict.intern("AAPL") → code 0
  → TickMessage or direct append: append<uint32_t>(code)
```

For trades table (TickMessage path):
- TickMessage.symbol_id already is int32 — use dictionary to map string → id
- Need a global symbol dictionary (separate from partition dict)

For custom tables (non-trades):
- Direct `partition.string_dict().intern(s)` → `col->append<uint32_t>(code)`

**3.2 TickMessage symbol mapping**

```cpp
// In Pipeline or executor:
StringDictionary global_symbol_dict_;  // "AAPL" → 1, "GOOGL" → 2, ...

// INSERT INTO trades (symbol, price, ...) VALUES ('AAPL', 150.25, ...)
//   → global_symbol_dict_.intern("AAPL") → symbol_id = 1
//   → msg.symbol_id = 1  (existing int32 path, zero changes to TickMessage)
```

Files changed: `executor.cpp`, `pipeline.h`

### Phase 4: Executor — Read Path

**4.1 WHERE string comparison**

```
WHERE symbol = 'AAPL'
  → dict.find("AAPL") → code (or -1 if not found → empty result)
  → scan_compare<uint32_t>(codes, ==, code)   ← existing template!
```

No new scan code needed. Dictionary lookup at query start, then integer scan.

**4.2 SELECT string output**

```
SELECT symbol FROM trades
  → read uint32 code from column
  → dict.lookup(code) → "AAPL"
  → put in result
```

Need to extend `QueryResultSet` for string cells:

```cpp
// Option A: Per-cell string storage
struct QueryResultSet {
    // ... existing ...
    // String values: [row][col] → string (empty if not string column)
    std::vector<std::vector<std::string>> string_cells;
};

// Option B: Column-level string dictionary in result
struct QueryResultSet {
    // ... existing ...
    // Per-column dictionaries for string columns
    std::vector<std::shared_ptr<StringDictionary>> result_dicts;
    // rows still stores uint32 codes — HTTP layer resolves
};
```

Option B is better — avoids copying strings per row. HTTP layer does:
```cpp
if (col_type == STRING) {
    os << "\"" << result_dicts[c]->lookup(row[c]) << "\"";
} else { ... }
```

**4.3 GROUP BY string**

```sql
SELECT symbol, SUM(volume) FROM trades GROUP BY symbol
```

GROUP BY already works on int64 values. Dictionary codes are uint32 → works.
Result formatting resolves codes to strings.

**4.4 JOIN on string**

```sql
SELECT * FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol
```

Both sides use same dictionary (or dictionary mapping at join time).
Since partition key is symbol_id (int32), ASOF JOIN already matches by int.

Files changed: `executor.cpp`, `executor.h`, `http_server.cpp`

### Phase 5: HTTP Output

```json
{
  "data": [
    ["AAPL", 150.25, 100, 1711234567000000000],
    ["GOOGL", 2800.50, 50, 1711234568000000000]
  ],
  "columns": ["symbol", "price", "volume", "timestamp"],
  "types": ["STRING", "FLOAT64", "INT64", "TIMESTAMP_NS"]
}
```

Files changed: `http_server.cpp`

---

## What Does NOT Change

| Component | Why |
|-----------|-----|
| ColumnVector | Stores uint32 codes — already supports uint32 |
| ArenaAllocator | Codes are fixed-size, arena works as-is |
| TickMessage | symbol_id stays int32 — dictionary maps string↔id externally |
| SIMD scan | scan_compare<uint32_t> works on codes |
| Partition routing | PartitionKey.symbol_id stays int32 |
| HDB flush/Parquet | uint32 column + dictionary metadata |
| Python zero-copy | codes array is numpy-compatible uint32 |

## Performance Impact

| Operation | Before | After | Reason |
|-----------|--------|-------|--------|
| INSERT | — | +50ns/row | One hash lookup to intern string |
| WHERE = | — | +20ns/query | One dict.find() then existing int scan |
| SELECT | — | +10ns/row | One dict.lookup() per string cell |
| GROUP BY | No change | No change | Operates on integer codes |
| SIMD scan | No change | No change | uint32 codes, same path |

## File Change Summary

| File | Changes |
|------|---------|
| `include/zeptodb/storage/string_dictionary.h` | **NEW** — StringDictionary class |
| `include/zeptodb/storage/column_store.h` | Add `ColumnType::STRING` |
| `include/zeptodb/storage/partition_manager.h` | Add `StringDictionary` to Partition |
| `include/zeptodb/sql/ast.h` | Extend InsertValue, Expr for string |
| `src/sql/parser.cpp` | Parse string literals in WHERE/INSERT |
| `src/sql/executor.cpp` | String INSERT/SELECT/WHERE/GROUP BY |
| `src/server/http_server.cpp` | String JSON output |
| `include/zeptodb/sql/executor.h` | QueryResultSet string support |
| `src/core/pipeline.h` | Global symbol dictionary |
| `tests/unit/test_sql.cpp` | String column tests |

Estimated: ~800 lines of new code, ~200 lines modified.
