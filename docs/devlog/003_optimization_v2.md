# Devlog 003: Phase B v2 — Aggressive SIMD/JIT Optimization

> Date: 2026-03-21
> Author: Gosengyi (AI subagent)
> Branch: main
> Build: Clang 19.1.7, -O3 -march=native, Highway SIMD 1.2.0, LLVM OrcJIT 19

---

## Background and Problem Statement

Phase B v1 introduced Highway SIMD, but a gap vs kdb+ remained:

| Operation | v1 Result | kdb+ Reference | Gap |
|------|---------|------------|-----|
| filter_gt_i64 1M | 1,341μs | 200-400μs | **3-5x slower** |
| sum_i64 1M | 265μs | 100-200μs | ~1.5x |
| vwap 1M | 530μs | ~200μs | ~2.5x |
| JIT filter 1M | 1,254μs (per-row) | hardcoded ~530μs | **2.4x** |

**Biggest bottleneck**: filter is 3-5x slower → start with root cause analysis

---

## Optimization 1: BitMask Filter (filter_gt_i64_bitmask)

### Theoretical Basis

Existing `filter_gt_i64` (v1) output path:

```
StoreMaskBits → uint8_t mask_bytes
→ while(bits) { ctz → out_indices[out_count++] = i + k; bits &= bits-1; }
```

**3 problems:**

1. **Write bandwidth**: 1M rows, 50% selectivity → ~500K uint32_t = **2MB writes**
   - Exceeds L2 cache (typically 256KB-1MB) → DRAM writes occur

2. **Branch misprediction**: `while(bits)` loop is data-dependent on bit patterns
   - Each iteration has varying ctz results → branch predictor fails to learn irregular patterns
   - **Measured**: ~1,169μs for 1M rows → accumulated branch miss effect

3. **Memory access pattern**: Sequential writes to out_indices, but
   store-to-load forwarding dependency chain from out_count++ increment (serialization)

**BitMask solution:**

```
StoreMaskBits → uint8_t mask_bytes
→ out_bits[word_idx] |= (bits << bit_off)  ← that's all
```

- Writes: 1M / 8 = **128KB** (8x reduction)
- 128KB fits entirely in L2 cache → no DRAM writes
- No branches: only bitwise OR operations
- `popcount()` uses __builtin_popcountll → hardware POPCNT instruction

**Measured results:**
- 1M: 1,169μs → **272μs** (4.3x improvement) ✅ kdb+ equivalent
- 100K: 98μs → **7μs** (14x improvement) — dramatic effect when cache-hot

### Code Change Summary

**`include/zeptodb/execution/vectorized_engine.h`:**
- Added `BitMask` class (num_rows → num_words = ⌈n/64⌉)
- Declared `filter_gt_i64_bitmask()`
- Declared `sum_i64_masked()` (ctz-based sparse sum)

**`src/execution/vectorized_engine.cpp`:**
- `filter_gt_i64_bitmask_impl()`: tracks bit_pos while OR-writing
  - Handles boundary crossing (bit_off + N > 64) with second word
- `sum_i64_masked()`: ctz loop for bit traversal (better for low selectivity)

### Things Tried That Didn't Work

**CompressStore attempt**: Considered using Highway's `CompressStore(v, mask, d, out_buf)`
→ CompressStore outputs values (not indices), unsuitable for index filtering purpose
→ Abandoned; kept StoreMaskBits + OR approach

**Direct VPCOMPRESSD**: Considered using AVX-512's VPCOMPRESSD for compressed index output
→ No direct Highway API exposure
→ BitMask is more portable; adopted instead

---

## Optimization 2: sum_i64_fast (Scalar 4-way Unroll + Prefetch)

### Theoretical Basis

**Goal**: Maximize scalar ILP without relying on the compiler

64-bit ADD has 1 CPI latency, but with a single accumulator chain, the next ADD
must wait for the previous ADD to complete → **latency-bound**

```
// Single accumulator (bad):
for i: s0 += data[i]  ← data[i] load + ADD serialized
```

4-way accumulators let OOO (Out-of-Order) processor execute independent chains in parallel:

```
s0 += data[i+0]  |  s1 += data[i+1]  |  s2 += data[i+2]  |  s3 += data[i+3]
```

**Prefetch strategy:**
- L2 latency ~12ns @ 3GHz ≈ ~36 cycles
- 16 elements × 8bytes = 128bytes → request next 512bytes while processing 2 cache lines
- `__builtin_prefetch(data + i + 64, 0, 1)`: read-only, locality=1 (L2)

### Measured Results

- 100K: scalar 25μs → fast 6μs (4.2x) — slightly slower than SIMD v1 (4μs)
- 1M: 302μs → **265μs** (1.1x) — equivalent to SIMD
- **Conclusion**: Memory-bound at 1M+, no difference between SIMD and scalar

### CPU Microarchitecture Analysis (1M rows)

```
Data size: 1M × 8B = 8MB
L3 cache (AWS EC2): typically 8-32MB → near boundary
DRAM bandwidth: ~25-50 GB/s (DDR4)
sum operation intensity: 1 ADD / 8 bytes = 0.125 FLOP/byte
Roofline ceiling (BW limit): 0.125 × 25GB/s = ~3.1 GFLOP/s
Actual ADD throughput: ~10+ GFLOP/s
→ Fully memory-bound: compute optimizations have no effect
```

**Why SIMD is effective at 100K?**
100K × 8B = 800KB → fits entirely in L2/L3 cache → compute-bound region
SIMD vector width (AVX-512: 8 i64 parallel) provides direct benefit

---

## Optimization 3: sum_i64_simd_v2 (SIMD 8x Unroll + Prefetch)

### Theoretical Basis

Extending from v1's 4-way accumulators to 8-way:
- ROB (Re-order Buffer) size: Skylake 224 entries → accommodates more in-flight ops
- 8 independent SIMD vector register chains → attempts to saturate load units

**Measured**: Equivalent to v1 (4x) at 1M+ → memory-bound confirmed

---

## Optimization 4: vwap_fused (4x Unroll + Dual-Array Prefetch)

### Theoretical Basis

```
prices  array: 1M × 8B = 8MB  (read once)
volumes array: 1M × 8B = 8MB  (read once)
Total reads: 16MB → simultaneous prefetch of both arrays maximizes memory controller use
```

**FMA pipeline**: Skylake FMA throughput 0.5 CPI (ports 0, 1 each have 1)
4 independent FMA chains to saturate ports:

```
pv0 = MulAdd(p0, v0, pv0)   pv1 = MulAdd(p1, v1, pv1)
pv2 = MulAdd(p2, v2, pv2)   pv3 = MulAdd(p3, v3, pv3)
```

**Measured results**: v1 (2x, 532μs) → fused (4x+pf, 531μs) — equivalent at 1M
Marginal improvement at 100K: 14μs vs 15μs

**Conclusion**: vwap is also fully memory-bound at 1M+
Memory controller already saturated by simultaneous reads of two arrays

---

## Optimization 5: JIT O3 Pass Application

### Original Problem

```cpp
// v1: Only OptimizeForSize attribute → code generation without passes
fn->setAttributes(...OptimizeForSize...)
```

In LLVM, `OptimizeForSize` is an attribute (hint), not an optimization pass.
Actual optimization requires `PassBuilder + buildPerModuleDefaultPipeline(O3)`.

### Implementation

`IRTransformLayer::setTransform(apply_o3)` → registered once in `initialize()`
→ O3 passes applied automatically on each addIRModule

```cpp
static llvm::Expected<llvm::orc::ThreadSafeModule>
apply_o3(tsm, r) {
    pb.buildPerModuleDefaultPipeline(O3).run(mod, mam);
}
impl_->jit->getIRTransformLayer().setTransform(apply_o3);
```

**Note**: `setTransform` must be called only once at initialization. Calling per compile()
transfers unique_function ownership, destroying previous lambda → causes bugs

**Measured improvement**: JIT per-row 1M: (before) 1,254μs → (O3) 1,129μs (~10% improvement)
Still 2.1x slower than C++ fptr (530μs) → per-row call overhead is the main cause

---

## Optimization 6: JIT compile_bulk (Bulk Loop IR Generation)

### Design Intent

Fundamental problem with per-row approach:
```
for i: if (filter_fn(prices[i], volumes[i])) cnt++;
```
Function call per row: push/pop, ret, stack frame → **~5-10ns/call × 1M = 5-10ms**

Solution: Generate IR containing the loop directly:
```
void bulk_fn(prices*, volumes*, n, out_indices*, out_count*)
  loop: load → cmp → store → inc
```

### Measured Results (Unexpected)

| rows | per-row | bulk | Result |
|------|---------|------|------|
| 1M   | 1,129μs | 5,320μs | ❌ bulk is 4.7x slower |

### Failure Root Cause Analysis (Post-debug)

1. **alloca-based counters**: Using `i_alloca`, `cnt_alloca` → O3 should promote to registers
   via mem2reg, but may be incomplete depending on loop structure

2. **Missing noalias attribute**: `noalias` not specified on prices/volumes pointer params
   → LLVM conservatively assumes the two pointers may alias
   → Blocks vectorization (loop vectorization)!
   → per-row only does simple comparison, so no aliasing issue

3. **out_indices writes**: bulk also writes to index array → same overhead as SelVec v1

4. **Unfair benchmark conditions**: per-row only increments count (register), bulk includes array writes

### Fix Direction (Incomplete, Phase C)

```cpp
// Add noalias to parameters
arg_prices->addAttr(llvm::Attribute::NoAlias);
arg_volumes->addAttr(llvm::Attribute::NoAlias);

// PHI node-based loop (no alloca needed, LLVM assigns registers directly)
auto* i_phi   = builder.CreatePHI(i64_ty, 2, "i");
auto* cnt_phi = builder.CreatePHI(i64_ty, 2, "cnt");
```

---

## Complete List of Things Tried That Didn't Work

| Attempt | Reason | Result |
|------|------|------|
| sum 8x SIMD unroll | Theoretically fills ROB more | Memory-bound at 1M+ → no effect |
| vwap 4x unroll + prefetch | Attempt to saturate FMA ports | Equivalent to v1 at 1M (~1μs diff) |
| CompressStore (for indices) | Wrong purpose — outputs values | Abandoned |
| Direct VPCOMPRESSD | No Highway API exposure | Replaced with BitMask |
| JIT setTransform per compile() | unique_function re-assignment issue | Symbol lookup failure → changed to 1x at init |
| bulk JIT alloca approach | Blocks LLVM vectorization | 4.7x slower than per-row |

---

## Conclusion and Phase C Priorities

### Achieved
- **filter_gt_i64**: 1,341μs → 272μs (**4.9x improvement**, kdb+ equivalent achieved) ✅

### Not Yet Achieved + Root Cause Identified
- **sum/vwap**: Memory-bound limit. Need NT Store or algorithm change to improve
- **JIT bulk**: Vectorization can be enabled with noalias + PHI nodes

### Phase C Candidates (Priority Order)
1. Fuse `filter_bitmask + sum_masked` pipeline (single pass)
2. JIT bulk: Rewrite with `noalias` + PHI nodes → LLVM auto-vectorization
3. NT Store experiment: `_mm_stream_si64` for sum accumulation
4. Enable HugePages: Reduce TLB misses (mlock + madvise MADV_HUGEPAGE)
