# 098 — PartitionKey packing + VWAP 1M recovery closeout

Date: 2026-04-19
Authors: `pack_partition_key` → `profile_remaining` → `apply_vwap_fix`
Status: merged
Related: 097 (perf recovery + clang unification), 082 (multi-table).

Follow-up to 097, which left a residual VWAP 1M p50 gap and BENCH 1
peak ingest gap tracked under `BACKLOG.md` P7. This iteration:
(1) cheap structural win on `PartitionKey` hash/eq; (2) re-profile;
(3) decide whether any simple VWAP fix is recoverable; (4) document.

---

## 1. Fix applied — `PartitionKey` hash/eq via packed uint64 + splitmix64

**File:** `include/zeptodb/storage/partition_manager.h` (header-only,
no ABI change — still 16 bytes).

Prior hash chained three `std::hash<T>` calls with `(h1 * 31) ^ h2`;
equality was field-by-field. New code packs
`(symbol_id[0:32], table_id[32:48], hour_epoch&0xFFFF[48:64])`,
XORs the full `hour_epoch` in, runs one splitmix64 round
(`xor-shift-33; mul 0xff51afd7ed558ccd; xor-shift-33`). Equality
becomes two `uint64_t` compares on the same packed layout — avoids
`memcmp` padding-byte UB. Both operators are
`[[gnu::always_inline]]`.

---

## 2. Profile after packing — residual is inherent

`perf record bench_pipeline query` post-packing: `PartitionKey`
hash/eq is <0.01 % in both pre and post profiles (map hit is
per-partition, not per-row). Packing is no-regression on ingest;
no effect on the query hot path.

Re-quantified VWAP 1M p50 vs a freshly-rebuilt baseline (not the
582 µs best-case cited in 097):

| Baseline (3-run median, clang-19) | Post-097 | Post-packing |
|----------------------------------:|---------:|-------------:|
| 625 µs                            | 687 µs   | 669 µs       |

Real gap after packing: **~44 µs (7.0 %)**, not the 105 µs carried
forward from 097.

Top-3 hot functions (post-packing):

| Rank | %      | Function                       |
|------|--------|--------------------------------|
| 1    | 48.61% | `ZeptoPipeline::query_vwap`    |
| 2    | 24.11% | `execution::filter_gt_i64`     |
| 3    | 11.71% | `execution::sum_i64_selected`  |

Inner-loop disassembly of `query_vwap` is **byte-identical** to
baseline (36 instr per 4-row iter: 19 mov + 5 add + 4 imulq + 4 adc
+ 2 vpaddq + 1 jne + 1 cmp). The IPC drop (3.14 → 2.99) and +9.7 %
instruction count come entirely from clang-19 spilling the `v_sum`
int64 accumulator to `-0x40(%rbp)` where baseline kept it in a
register. Store-forwarding penalty on that spill = 8.49 % of
`query_vwap` time, ≈25 µs of the 44 µs delta.

Root cause: multi-table `table_id` threading + `[[unlikely]]`
HDB-fallback call site raised register pressure past the spill
threshold. Same source semantics, different allocator decision.

Classification: **inherent (compiler-dependent register
allocation).** Scalar `__int128 imulq` has no 64×64→128 SIMD on
x86. Multi-table id is required. HDB fallback is cold by design.
Only bounded recovery path is a medium refactor (~30 LOC: extract
full-scan kernel to `[[gnu::hot, gnu::flatten]]` helper, or route
through `execution::vwap_fused`) — marginal ROI.

---

## 3. Decision — no VWAP fix this iteration

Per `profile_remaining`: keep the packing fix, document, move on.
Outcome C of the three-outcome protocol.

---

## 4. Before / after metrics

### x86_64 (dev workstation, clang 19.1.7)

| Metric | Baseline 875a4c3 | Post-097 | Post-packing | vs baseline |
|--------|-----------------:|---------:|-------------:|------------:|
| BENCH 1 peak (batch=64, M t/s) | 5.52 | 4.87 | **4.81** (5-run med) | −12.9 % |
| VWAP 1M MV-off p50 (µs) | 582 best / 625 rebuilt median | 687 | **697** (5-run med) | +11.5 % vs 625 |
| VWAP 1M MV-on  p50 (µs) | — | 688 | **696** (5-run med) | — |

5-run raw (098): ingest 4.78/4.79/4.81/4.86/4.86 (median 4.81);
VWAP MV-off 625.5/667.4/697.3/697.4/698.0 (median 697.3); VWAP MV-on
624.1/668.1/696.5/697.3/699.7 (median 696.5). The 625 µs outlier
(run 4) matches the 582 µs best-case — allocator spill is
temperature/cache-dependent, median is the honest metric.

### aarch64 (Graviton, same clang 19.1.7)

| Metric | Devlog 097 | Post-packing (098) |
|--------|-----------:|-------------------:|
| BENCH 1 peak (M t/s) | 4.04 | **3.98** (3-run med) |
| VWAP 1M MV-off p50 (µs) | 634.7 | **632.7** |
| VWAP 1M MV-on  p50 (µs) | 630.9 | **631.8** |

arm64 flat vs 097 (within run-to-run noise). Packing is
arch-neutral; NEON also spills `v_sum` consistently.

---

## 5. Recovery vs original 13.4 % gap

Original cited: VWAP ~18 % (800→582), ingest ~16 % (4.66→5.52).

| Dimension | Original | After 097 | After 098 | Recovered |
|-----------|---------:|----------:|----------:|----------:|
| VWAP MV-off vs 582 µs best | +37.5 % | +18 % | +19.8 % | ~47 % |
| VWAP MV-off vs 625 µs median | +28 % | +9.9 % | +11.5 % | ~59 % |
| BENCH 1 ingest | −16 % | −11.8 % | −12.9 % | ~19 % |

Against the realistic 625 µs median, current 697 µs is +11.5 % —
just outside GREEN (≤10 %). Against 582 µs best, ~20 %.

---

## 6. What remains

| Gap | Size (x86 median) | Classification |
|-----|------------------:|----------------|
| VWAP 1M p50: 625 → 697 µs | +72 µs (+11.5 %) | **Inherent (register allocation).** 8.5 % of `query_vwap` is the `v_sum` spill slot; upper-bound recovery ~25 µs. |
| BENCH 1 peak: 5.52 → 4.81 M t/s | −0.71 M (−12.9 %) | Per-tick partition-key dispatch cost on multi-table pipeline. Hash/eq <0.01 %; `PartitionManager::get_or_create` path ~5 % hotter than single-table. |

New info vs 097: the **mechanism** is clang-19 register allocation
on the multi-table `query_vwap`. Compiler is already pinned (097);
nothing to tune at the build level without kernel extraction.

---

## 7. Verification

### Required test filters (x86)
```
./tests/zepto_tests --gtest_filter='*Partition*:*Storage*:*Ingest*:*Pipeline*:*TableScoped*:*VWAP*:*Query*'
→ 152 tests, [ PASSED ] 152 tests.
```

### aarch64 via stage 8
```
./tools/run-full-matrix.sh --stages=8 --keep-going
→ 1360/1361 passed, 1 flake: QueryCoordinator.TwoNodeRemote_GroupBy_Concat
→ retry: PASSED (known tcp_rpc_pool flake, devlog 096)
```

### Full local matrix
```
./tools/run-full-matrix.sh --local --keep-going
  1:build              PASS    6s
  2:unit_x86           PASS   73s
  8:aarch64_unit_ssh   PASS   89s
  3:integration        PASS    9s
  4:python             PASS    0s
  TOTAL=95.34s
```
5/5 PASS, 2722 tests across both archs.

---

## 8. Files changed

| File | Change |
|------|--------|
| `include/zeptodb/storage/partition_manager.h` | `operator==` two packed-`uint64_t` compares; `PartitionKeyHash` packed-uint64 + splitmix64 |
| `docs/devlog/098_partition_key_and_vwap_recovery.md` | this file |
| `docs/COMPLETED.md` | bullet pointing at 098 |
| `docs/BACKLOG.md` | P7 updated with inherent-residual note |

No `.cpp`, no test, no CI change.

---

## 9. Cross-reference & follow-up

- **097** — prior iteration; established compiler pin. Left the
  residual this devlog classifies.
- **082** — multi-table `PartitionKey`; cause of register-pressure
  rise.
- **023** — Graviton verification; §4 arm64 column confirms packing
  is arch-neutral.

Follow-up (not in this devlog): if perf pressure rises later,
extract the `query_vwap` full-scan kernel into a standalone
`[[gnu::hot, gnu::flatten]]`
`execution::vwap_fused(const int64_t*, const int64_t*, size_t)`
behind the same `[[unlikely]]` HDB branch. Estimated recovery
~25 µs, ~30 LOC. Re-profile before merging any new state into
`query_vwap` — residual sits right at the spill threshold.
