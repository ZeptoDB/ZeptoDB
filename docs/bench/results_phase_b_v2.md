# Phase B v2 — Optimization Benchmark Results
# Run date: 2026-03-22
# Environment: Intel Xeon 6975P-C, 8 vCPU, 30GB RAM, Clang 19 Release -O3 -march=native

---

## Key Results Summary

| Operation | v1 (SIMD) | v2 (optimized) | Final speedup |
|---|---|---|---|
| filter_gt_i64 1M | 1,350μs | **272μs** | **4.96x improvement** ✅ |
| filter_gt_i64 100K | 114μs | **10μs** | **11.4x improvement** 🚀 |
| sum_i64 1M | 265μs | 264μs | 1.0x (already optimal) |
| vwap 100K | 19μs | **17μs** | 2.8x (vs scalar) |
| JIT filter 1M | 3,430μs → **1,292μs** (O3) | improved | **2.6x improvement** ✅ |

---

## Part 1: SIMD Comparison (vs v1 baseline)

### 100K rows
| Operation | Scalar | SIMD v1 | Speedup |
|---|---|---|---|
| sum_i64 | 25μs | **6μs** | **4.2x** |
| filter_gt_i64 | 330μs | 117μs | 2.8x |
| vwap | 48μs | 20μs | 2.4x |

### 1M rows
| Operation | Scalar | SIMD v1 | Speedup |
|---|---|---|---|
| sum_i64 | 310μs | 265μs | 1.2x |
| filter_gt_i64 | 3,262μs | 1,360μs | 2.4x |
| vwap | 587μs | 529μs | 1.1x |

### 10M rows
| Operation | Scalar | SIMD v1 | Speedup |
|---|---|---|---|
| sum_i64 | 3,034μs | 2,653μs | 1.1x |
| filter_gt_i64 | 32,791μs | 13,566μs | 2.4x |
| vwap | 9,714μs | 5,561μs | 1.7x |

---

## Part 2: BitMask Filter Optimization (biggest gain)

**Strategy:** SelectionVector (write index array) → BitMask (bit compression)

| rows | v1 SelectionVector | v2 BitMask | Speedup |
|---|---|---|---|
| 100K | 114μs | **10μs** | **11.4x** 🚀 |
| 1M | 1,350μs | **272μs** | **4.96x** ✅ |
| 10M | 13,631μs | **2,767μs** | **4.93x** ✅ |

**Analysis:**
- SelectionVector writes each passing index individually → cache misses + branch mispredictions
- BitMask compresses 64 rows into one uint64 → 64x cache efficiency, 1/64 writes
- 11x at 100K: maximum effect when data fits in L2 cache

**kdb+ comparison update:**
- filter 1M: 1,350μs → **272μs** (entered kdb+ ~100–300μs range) ✅

---

## Part 3: sum_i64 Optimization Results

**Strategy comparison:** Scalar → SIMD v1 (4x unroll) → fast (4-way accumulator) → SIMD v2 (8x + prefetch)

| rows | Scalar | SIMD v1 | fast | SIMD v2 |
|---|---|---|---|---|
| 100K | 25μs | **6μs (4.2x)** | 6μs (4.2x) | 7μs (3.6x) |
| 1M | 296μs | 269μs (1.1x) | 264μs (1.1x) | 267μs (1.1x) |
| 10M | 3,028μs | 2,656μs (1.1x) | 2,658μs (1.1x) | 2,655μs (1.1x) |

**Analysis:**
- 100K (L2 cache range): 4.2x — clear SIMD benefit
- 1M/10M: 1.1x — **memory-bandwidth bound** (all methods same speed)
- Conclusion: sum cannot be further optimized; memory subsystem is the bottleneck

---

## Part 4: VWAP Fused Optimization

**Strategy:** 2-pass (price scan, volume scan) → 1-pass fused + 4x unroll + prefetch

| rows | Scalar | SIMD v1 | fused(4x+pf) | Notes |
|---|---|---|---|---|
| 100K | 48μs | 19μs (2.5x) | **17μs (2.8x)** | L2 cache effect |
| 1M | 570μs | 530μs (1.1x) | 532μs (1.1x) | Bandwidth bound |
| 10M | 9,153μs | 5,568μs (1.6x) | 5,544μs (1.7x) | SIMD benefit retained |

---

## Part 5: JIT Optimization (O3 applied)

| rows | per-row O3 | bulk O3 | C++ lambda | Notes |
|---|---|---|---|---|
| 100K | 112μs | 511μs | 13μs | bulk counter-productive |
| 1M | **1,292μs** | 5,317μs | 532μs | per-row O3: 2.6x improvement |

**Analysis:**
- per-row O3: 3,430μs → 1,292μs (**2.6x improvement**) ✅
- bulk IR is counter-productive — LLVM cannot optimize the IR loop
- C++ lambda still 2.4x faster — JIT needs SIMD emit (remaining Phase B work)

---

## kdb+ vs ZeptoDB Final Comparison (Phase B v2)

| Operation | kdb+ reference | ZeptoDB v2 | Status |
|---|---|---|---|
| Ingestion | ~2–5M/sec | **5.52M/sec** | ✅ Superior |
| VWAP 1M | ~200–500μs | **532μs** | ⚠️ Close |
| filter 1M (bitmask) | ~100–300μs | **272μs** | ✅ In range |
| sum 1M | ~50–150μs | **264μs** | ⚠️ 2x behind (bandwidth bound) |
| JIT filter | q interpreter | 1,292μs | ⚠️ Improvement in progress |

---

## Conclusion

**Phase B v2 key achievements:**
1. **BitMask filter: up to 11x improvement** — entered kdb+ range
2. **JIT O3: 2.6x improvement** — still slower than C++ but direction confirmed
3. **sum/vwap: bandwidth bound** — hardware-level ceiling, limited further optimization

**Next steps (remaining Phase B):**
1. JIT SIMD vector IR emit → target C++ lambda performance
2. sum multi-column fusion (simultaneous price+volume processing)
3. filter+aggregate pipeline fusion (filter → sum single-pass)
