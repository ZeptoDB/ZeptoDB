# Devlog 002: Highway SIMD + LLVM JIT Integration

**Date:** 2026-03-21
**Phase:** B (SIMD + JIT)
**Author:** APEX-DB Dev Team

---

## Goals
Replace Phase E scalar implementations with Highway SIMD and LLVM JIT to improve vectorized operation performance.

## Implementation Decisions

### Highway SIMD (v1.2.0)

**Using multi-target dispatch pattern:**
- `foreach_target.h` + `HWY_BEFORE_NAMESPACE` / `HWY_AFTER_NAMESPACE` macros
- Auto-recompiles for multiple targets: SSE4, AVX2, AVX-512, etc.
- Runtime `HWY_DYNAMIC_DISPATCH` selects optimal target for CPU
- Current server: AVX-512 (ice-lake-server class, AVX512F/BW/DQ/VL/VNNI)

**Implemented SIMD operations:**

1. **`sum_i64`**: 4 accumulators unrolled + `ReduceSum`. Independent accumulators eliminate pipeline dependencies, maximizing ILP.

2. **`filter_gt_i64`**: `Gt()` → mask → `StoreMaskBits()` → `__builtin_ctzll()` bit traversal. Replaces scalar `if` branches with mask-based predication to eliminate branch mispredictions.

3. **`vwap`**: `ConvertTo(i64→f64)` + `MulAdd(price, volume, acc)` pipeline. Unrolled with 2 accumulator pairs. f64 FMA has better throughput than `__int128` scalar.

**SelectionVector changes:**
- Added `set_size(n)` method — SIMD filter writes directly into indices array then sets size

### LLVM JIT Engine (OrcJIT v2)

**Architecture:**
```
Expression String → Parser (recursive descent) → AST → LLVM IR → OrcJIT → Native Code
```

**Parser grammar:**
```
expr    := or_expr
or_expr := and_expr ('OR' and_expr)*
and_expr := compare ('AND' compare)*
compare := COLUMN ['*' INT] CMP_OP INT
COLUMN  := 'price' | 'volume'
CMP_OP  := '>' | '>=' | '<' | '<=' | '==' | '!='
```

**Design decisions:**
- No external parser libraries (Flex/Bison, etc.) — overkill for simple expressions
- Using `LLJIT` (OrcJIT v2): more modern and thread-safe than `ExecutionEngine`
- Unique function name per compile() (`apex_filter_0`, `apex_filter_1`, ...)
- No optimization passes applied (can add via `PassBuilder` later)
- Return type: `bool (*)(int64_t price, int64_t volume)` function pointer — no virtual call needed

### Build Issue

**Resolved libstdc++ version conflict:**
- LLVM 19 .so requires `GLIBCXX_3.4.30` (libstdc++ 6.0.30+)
- Clang 19 defaults to GCC 11 toolchain's libstdc++ 6.0.29
- Fix: replace GCC 11's libstdc++.so symlink with system libstdc++ 6.0.33
- Link LLVM as shared library (`libLLVM-19.so`) (static .a has the same issue)

## Benchmark Results

### SIMD (100K rows, L1/L2 cache-hot)
| Operation | Scalar | SIMD | Speedup |
|-----------|--------|------|---------|
| sum_i64 | 25μs | 6μs | **4.2x** |
| filter_gt | 307μs | 117μs | **2.6x** |
| vwap | 51μs | 20μs | **2.5x** |

### SIMD (10M rows, memory-bound)
| Operation | Scalar | SIMD | Speedup |
|-----------|--------|------|---------|
| sum_i64 | 3065μs | 2656μs | 1.2x |
| filter_gt | 32457μs | 13951μs | **2.3x** |
| vwap | 9811μs | 5587μs | **1.8x** |

### JIT (10M rows)
- Compile time: ~2.6ms
- JIT vs C++ function pointer: 0.41x (JIT is 2.4x slower)
- JIT correctness: ✅ Results match C++ lambda

### Analysis
- **Cache-resident data (100K, ~800KB)**: SIMD maximally effective (2.5-4.2x). Exactly matches the DataBlock pipeline (8192 rows/block) scenario.
- **Memory-bound (10M, ~80MB)**: Pure reads (sum) are bandwidth-limited; compute-intensive ops (filter/vwap) still improve 1.8-2.3x.
- **JIT**: Produces correct code even without optimization passes. Expect C++-equivalent performance with `-O2` PassBuilder.

## Next Steps (Phase C/D)

1. **JIT optimization**: Apply PassBuilder with InstCombine + SROA + GVN → achieve C++ equivalent performance
2. **SIMD filter → gather sum pipeline**: Fused operator connecting filter results directly to sum
3. **DataBlock integration**: SIMD execution in 8192-row block pipeline (cache-resident size)
4. **JIT expression caching**: LRU cache to avoid recompilation of frequently used filters
5. **Columnar batch JIT**: Row-level function calls → vector-level IR generation (SIMD JIT)
