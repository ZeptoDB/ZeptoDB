# Devlog 025: SQL UPDATE / DELETE

**Date:** 2026-03-24
**Phase:** SQL DML

## Summary

Implemented SQL UPDATE SET WHERE and DELETE FROM WHERE. Together with INSERT, all 3 DML operations are now complete.

## Supported Syntax

```sql
-- UPDATE (in-place modification)
UPDATE trades SET price = 15200 WHERE symbol = 1 AND price > 15100

-- UPDATE (multiple columns)
UPDATE trades SET price = 15200, volume = 500 WHERE symbol = 1

-- DELETE (in-place compaction)
DELETE FROM trades WHERE symbol = 1 AND price < 15000

-- DELETE (all rows for a symbol)
DELETE FROM trades WHERE symbol = 1
```

## Implementation

| Component | Change |
|-----------|--------|
| Tokenizer | Added `UPDATE`, `SET`, `DELETE_KW` tokens |
| AST | `UpdateStmt` (assignments + where), `DeleteStmt` (where) |
| Parser | `parse_update()`, `parse_delete()`, WHERE parsing fix (`match` vs `check`) |
| Executor | `exec_update()` in-place span modification, `exec_delete()` compaction + `set_size()` |
| ColumnVector | Added `set_size()` (for DELETE compaction) |

## Key Design Decisions

1. **UPDATE = in-place span write**: `as_span<int64_t>()[idx] = new_value` — zero-copy, O(matched rows)
2. **DELETE = compact + shrink**: Shift non-deleted rows forward → `set_size(new_size)` — arena memory is not reclaimed (append-only allocator)
3. **Symbol partition filtering**: Extract symbol condition via `has_where_symbol()` → scan only the relevant partition (symbol is a partition key, not a column)
4. **ALTER TABLE SET conflict resolution**: Adding the `SET` token broke the ALTER TABLE parser → Fixed to recognize `TokenType::SET` as an action as well

## Bug Fixes

- `SET` keyword being recognized as TokenType::SET caused ALTER TABLE SET TTL parsing to fail → Parser updated to also accept SET token
- WHERE token not consumed in UPDATE/DELETE WHERE parsing → Changed `check()` to `match()`
- `symbol` is a partition key, not a column → Added partition-level filtering

## Test Results

- All existing 766/766 tests passed
- Manual integration tests: UPDATE 2 rows, DELETE 2 rows, symbol isolation verification passed
