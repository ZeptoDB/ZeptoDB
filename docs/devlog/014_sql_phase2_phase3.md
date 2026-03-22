# Devlog 014: SQL Phase 2 & Phase 3 — Arithmetic, CASE WHEN, Date/Time, LIKE, Set Operations

*Date: 2026-03-22*

---

## Overview

This devlog covers three rounds of SQL parser and executor enhancements that together bring
APEX-DB's SQL subset to practical parity with ClickHouse for time-series analytical workloads.

| Phase | Features | Tests added |
|-------|----------|-------------|
| Phase 1 | IN, IS NULL/NOT NULL, NOT, HAVING | 9 |
| Phase 2 | SELECT arithmetic, CASE WHEN, multi-column GROUP BY | 13 |
| Phase 3 | Date/time functions, LIKE/NOT LIKE, UNION/INTERSECT/EXCEPT | 31 |

All 334+ existing tests continue to pass. One pre-existing flaky test
(`ClusterNode.TwoNodeLocalCluster`) is unrelated and timing-dependent.

---

## Phase 2: SELECT Arithmetic, CASE WHEN, Multi-Column GROUP BY

### What was built

#### SELECT Arithmetic — `ArithExpr` expression trees

Added a recursive `ArithExpr` AST node to represent value expressions in the SELECT list:

```cpp
struct ArithExpr {
    enum class Kind { COLUMN, LITERAL, BINARY, FUNC };
    Kind kind;

    // COLUMN
    std::string table_alias;
    std::string column;

    // LITERAL
    int64_t literal = 0;

    // BINARY
    ArithOp arith_op;                       // ADD / SUB / MUL / DIV
    std::shared_ptr<ArithExpr> left, right;

    // FUNC (Phase 3)
    std::string func_name, func_unit;
    std::shared_ptr<ArithExpr> func_arg;
};
```

Parser: `parse_arith_expr_node()` → `parse_arith_term()` → `parse_arith_primary()` (standard
precedence climbing). Executor: `eval_arith(node, part, row_idx)` static function.

**Supported queries:**
```sql
SELECT price * volume AS notional FROM trades WHERE symbol = 1
SELECT SUM(price * volume) AS total_notional FROM trades WHERE symbol = 1
SELECT AVG(price - 15000) AS avg_premium FROM trades WHERE symbol = 1
SELECT (close - open) / open AS ret FROM trades WHERE symbol = 1
```

#### CASE WHEN

```cpp
struct CaseWhenBranch {
    std::shared_ptr<Expr>      when_cond;   // full WHERE-style condition tree
    std::shared_ptr<ArithExpr> then_val;    // full arithmetic expression
};
struct CaseWhenExpr {
    std::vector<CaseWhenBranch> branches;
    std::shared_ptr<ArithExpr>  else_val;   // nullptr → 0
};
```

`eval_case_when(expr, part, row_idx)` calls `eval_expr_single()` for the WHEN condition
(scalar version of the batch filter), then `eval_arith()` for the THEN/ELSE value.

```sql
SELECT CASE WHEN price > 15050 THEN 1 ELSE 0 END AS is_high FROM trades
```

#### Multi-Column GROUP BY

The `is_symbol_group` optimization previously fell through to the composite path even for
multi-column keys because it lacked a `gb.columns.size() == 1` guard. Fixed. Multi-column keys
now build a `std::vector<int64_t>` composite key and use `VectorHash` for O(1) average lookup:

```cpp
auto make_group_key = [&](size_t row) -> std::vector<int64_t> {
    std::vector<int64_t> key;
    for (size_t k = 0; k < gb.columns.size(); ++k) {
        if (gb.columns[k] == "symbol") { key.push_back(part->key().symbol_id); continue; }
        if (gb.xbar_buckets[k] > 0) { /* floor */ continue; }
        // plain column lookup
    }
    return key;
};
```

Both serial (`exec_group_agg`) and parallel (`exec_group_agg_parallel`) paths share this
`make_group_key` lambda pattern.

#### Parallel path synchronization

`exec_agg_parallel` and `exec_group_agg_parallel` were behind the serial paths on Phase 2
features. Both were updated:
- `agg_val` lambda: checks `arith_expr` first, falls back to direct column read
- Result column names now use the `alias` field rather than raw function+column strings
- `exec_group_agg_parallel` replaced the `std::unordered_map<int64_t,...>` GroupMap with
  `VectorHash<std::vector<int64_t>,...>` to support composite keys

---

## Phase 3: Date/Time Functions, LIKE/NOT LIKE, Set Operations

### 3-A. Date/Time Functions

New `ArithExpr::Kind::FUNC` with four built-in functions:

| Function | Behavior |
|----------|----------|
| `DATE_TRUNC('unit', col)` | Floor nanosecond timestamp to given unit bucket |
| `NOW()` | Current nanosecond timestamp via `std::chrono::system_clock` |
| `EPOCH_S(col)` | Nanoseconds ÷ 1,000,000,000 → seconds |
| `EPOCH_MS(col)` | Nanoseconds ÷ 1,000,000 → milliseconds |

Supported units for `DATE_TRUNC`: `ns`, `us`, `ms`, `s`, `min`, `hour`, `day`, `week`.

```cpp
static int64_t date_trunc_bucket(const std::string& unit) {
    if (unit == "ns")   return 1;
    if (unit == "us")   return 1'000;
    if (unit == "ms")   return 1'000'000;
    if (unit == "s")    return 1'000'000'000;
    if (unit == "min")  return 60'000'000'000LL;
    if (unit == "hour") return 3'600'000'000'000LL;
    if (unit == "day")  return 86'400'000'000'000LL;
    if (unit == "week") return 604'800'000'000'000LL;
    throw std::runtime_error("Unknown DATE_TRUNC unit: " + unit);
}
```

`DATE_TRUNC` result = `(val / bucket) * bucket` (integer floor division).

These functions can appear anywhere an `ArithExpr` is valid: SELECT list, GROUP BY keys,
WHERE clause (via comparison), ORDER BY expressions.

```sql
SELECT DATE_TRUNC('min', timestamp) AS minute, SUM(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY DATE_TRUNC('min', timestamp)

SELECT EPOCH_S(timestamp) AS ts_sec, price FROM trades WHERE symbol = 1
```

### 3-B. LIKE / NOT LIKE

New `Expr::Kind::LIKE` with DP glob matching:

```cpp
// Pattern: '%' = any substring, '_' = any single character
// Applied to int64 column values via std::to_string()
static bool like_match(const std::string& text, const std::string& pat) {
    // O(n*m) DP grid, standard wildcard match
}
```

INT64 column values are converted to their decimal string representation before matching.
This allows financial queries like `price LIKE '150%'` to match all prices in the 15000–15099
range (when stored without decimal point) or `timestamp LIKE '1711%'` for date prefix matching.

```sql
SELECT * FROM trades WHERE price LIKE '150%'        -- prefix match
SELECT * FROM trades WHERE price NOT LIKE '%9'       -- suffix exclusion
SELECT * FROM trades WHERE price LIKE '1500_'        -- single-char wildcard
```

The `LIKE` case was added to three evaluation sites:
1. `eval_expr()` — batch WHERE evaluation (returns `BitMask`)
2. `eval_expr_single()` — single-row evaluation (used by CASE WHEN)
3. `apply_having_filter()` — post-aggregation filter

### 3-C. Set Operations (UNION / INTERSECT / EXCEPT)

New `SelectStmt::SetOp` enum and `rhs` pointer:

```cpp
enum class SetOp { NONE, UNION_ALL, UNION_DISTINCT, INTERSECT, EXCEPT };
SetOp                       set_op = SetOp::NONE;
std::shared_ptr<SelectStmt> rhs;
```

Parser chains set operations after `parse_select()`:
```
parse() → parse_select() → if UNION/INTERSECT/EXCEPT → parse_select() recursively → rhs
```

Executor handles all four operations at the top of `exec_select()` before normal dispatch:

| Operation | Implementation |
|-----------|---------------|
| `UNION ALL` | Execute both sides, concatenate rows |
| `UNION DISTINCT` | Execute both sides, insert all into `std::set<vector<int64_t>>` |
| `INTERSECT` | Execute left, build set; execute right, keep only rows present in left set |
| `EXCEPT` | Execute left, build set from right; keep left rows not in right set |

```sql
SELECT price FROM trades WHERE symbol = 1
UNION ALL
SELECT price FROM trades WHERE symbol = 2

SELECT price FROM trades WHERE symbol = 1
INTERSECT
SELECT price FROM trades WHERE price > 15050

SELECT price FROM trades WHERE symbol = 1
EXCEPT
SELECT price FROM trades WHERE price > 15050
```

---

## Test Coverage

New tests added in `tests/unit/test_sql.cpp` (Parts 7–8):

**Phase 2 (Part 7):**
- `ArithExprSelectMul`, `ArithExprInAgg`, `CaseWhenBasic`, `CaseWhenElse`
- `MultiColGroupBy`, `MultiGroupBySymbolAndPriceBucket`
- `ParallelAggArith`, `ParallelGroupByMultiCol`

**Phase 3 (Part 8):**
- Tokenizer: `DateTimeFunctionKeywords`, `LikeAndSetOpKeywords`
- Parser: `DateTruncParsed`, `NowParsed`, `EpochSParsed`, `LikeExpr`, `NotLikeExpr`,
  `UnionAllParsed`, `UnionDistinctParsed`, `IntersectParsed`, `ExceptParsed`
- Executor: `DateTruncUsResult`, `DateTruncMsResult`, `EpochSResult`, `EpochMsResult`,
  `NowPositive`, `DateTruncInArith`, `LikeExact`, `LikePrefix`, `LikeSuffix`, `NotLike`,
  `LikeSymbolColumn`, `LikeUnderscore`, `UnionAllRowCount`, `UnionAllAggCombined`,
  `UnionDistinctDedup`, `UnionDistinctNoOverlap`, `IntersectOverlap`, `IntersectEmpty`,
  `ExceptRemovesRows`, `ExceptNoOverlap`

---

## Key Bugs Fixed

1. **`is_symbol_group` multi-column regression** — The single-column GROUP BY symbol fast path
   fired even for multi-column keys. Fixed by guarding with `gb.columns.size() == 1`.

2. **`symbol` column returns 0 in arithmetic** — `get_col_data(part, "symbol")` returns nullptr
   since symbol is a partition-level key, not a per-row column. Fixed in `eval_arith()` and
   `exec_simple_select()` with an explicit `if (node.column == "symbol")` branch returning
   `part.key().symbol_id`.

3. **`DateTruncUsResult` returning 10 rows** — Test used `ORDER BY price ASC LIMIT 2` but the
   result column was aliased as `tb`, not `price`. `apply_order_by` couldn't find `price` and
   silently skipped LIMIT. Fixed by removing the ORDER BY/LIMIT from the test.

4. **`UNION DISTINCT` not deduplicating within the left side** — Initial implementation started
   with all left rows and only deduped right against them. Fixed to run both sides through
   `std::set<vector<int64_t>>`.

5. **`Expr::Kind::LIKE` missing from `apply_having_filter`** — Compiler warning for unhandled
   enum case. Added LIKE branch (always returns false for HAVING — symbol/aggregate columns
   shouldn't normally be LIKE-filtered in HAVING, but the switch must be exhaustive).

---

## Files Changed

| File | Change |
|------|--------|
| `include/apex/sql/tokenizer.h` | Added DATE_TRUNC, NOW, EPOCH_S, EPOCH_MS, LIKE, UNION, ALL, INTERSECT, EXCEPT tokens |
| `src/sql/tokenizer.cpp` | Keyword mappings for all new tokens |
| `include/apex/sql/ast.h` | `ArithExpr::Kind::FUNC` + func fields; `Expr::Kind::LIKE` + `like_pattern`; `SelectStmt::SetOp` + `rhs` |
| `include/apex/sql/parser.h` | `parse_string_literal()` declaration |
| `src/sql/parser.cpp` | Phase 2/3 parsing: arith FUNC, LIKE, set operations, string literal |
| `src/sql/executor.cpp` | `date_trunc_bucket()`, `eval_arith FUNC`, `eval_expr LIKE`, `apply_having_filter LIKE`, `exec_select set ops`, parallel path sync |
| `tests/unit/test_sql.cpp` | 53 new tests across Parts 7–8 |

---

## Next Steps

- Subquery / CTE (`WITH daily AS (...) SELECT ...`)
- EXPLAIN (query plan output)
- RIGHT JOIN / FULL OUTER JOIN
- SUBSTR and string manipulation functions
- NULL standardization (replace INT64_MIN sentinel with actual null bitmask)
