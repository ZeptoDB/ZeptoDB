# Devlog 079: JIT SIMD Emit

Date: 2026-04-16

## Summary

Added `compile_simd()` to `JITEngine` — generates explicit `<4 x i64>` SIMD vector IR instead of relying on LLVM auto-vectorization.

## What Changed

### New API

```cpp
BulkFilterFn compile_simd(const std::string& expr);
```

Same signature as `compile_bulk()` (`BulkFilterFn`), but generates vector instructions:

- **Vector loads**: `load <4 x i64>` from prices/volumes arrays (align 8)
- **Vector compare**: `icmp sgt/sge/slt/sle/eq/ne <4 x i64>` with splatted threshold
- **Vector logic**: `and/or <4 x i1>` for AND/OR conditions
- **Vector multiply**: `mul <4 x i64>` with splatted multiplier
- **Mask extraction**: `bitcast <4 x i1> → i4`, `zext → i32`, cttz loop to extract matching indices

### Loop Structure

```
entry → vec_cond → vec_body → ext_loop → ext_body → ext_done → vec_inc → vec_cond
                 ↘ scalar_cond → scalar_body → scalar_store → scalar_inc → scalar_cond
                                                             ↘ exit
```

- Main loop: 4 elements per iteration via `<4 x i64>` vectors
- Scalar tail: handles remainder (n % 4) with existing `codegen_node()`

### Key Implementation Details

- New `codegen_node_vec()` static function for vector AST codegen
- Reuses existing parser and AST — zero changes to parsing
- Vector loads use `align 8` (not default 32) since input arrays are standard `int64_t*`
- cttz intrinsic (`llvm.cttz.i32`) for efficient bit extraction from mask
- O3 optimization pass applied via existing IR transform layer

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/execution/jit_engine.h` | Added `compile_simd()` declaration, updated header comment |
| `src/execution/jit_engine.cpp` | Added `codegen_node_vec()` + `compile_simd()` implementation |
| `tests/unit/test_jit_simd.cpp` | 8 new tests |
| `tests/CMakeLists.txt` | Conditional `test_jit_simd.cpp` under `ZEPTO_USE_JIT` |

## Tests

8 new tests (`JITSIMDEmit.*`):
- `SimpleGt` — basic `price > 100` with 8 elements (vector + vector)
- `AndCondition` — `price > 100 AND volume > 50`
- `OrCondition` — `price > 200 OR volume > 500`
- `Multiplier` — `volume * 10 > 500`
- `ScalarTail` — 7 elements (4 vector + 3 scalar tail)
- `EmptyInput` — n=0, null pointers
- `AllMatch` — 1024 elements, all pass
- `NoneMatch` — 1024 elements, none pass

All 1129 tests passing.
