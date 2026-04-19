# 099 — Ingest path recovery: `store_tick` column-pointer caching

Date: 2026-04-19
Authors: `profile_ingest` → `apply_ingest_fix` (orchestrated sessions)
Status: merged
Related: 097 (MV fast-path + clang unification), 098 (PartitionKey packing + VWAP
closeout), 082 (multi-table partitioning).
Follow-up: closes the ingest-throughput portion of `BACKLOG.md` P7.

After 097/098 stabilised VWAP and hash/eq cost, the BENCH 1 ingest lane
still showed a flat **~5% per-tick regression across every batch size**
on x86_64 vs `875a4c3` baseline. This entry captures the root cause
and a minimal 10-line fix that closes the gap.

---

## 1. Profiling findings (from `profile_ingest`)

Run: `ip-172-31-21-146`, clang-19 `-O3 -march=native`, 1M ticks per batch.

| batch | baseline `875a4c3` | pre-stage HEAD | Δ      |
|------:|-------------------:|---------------:|-------:|
| 1     | 4.84 M/s           | 4.53 M/s       | −6.4%  |
| 64    | 5.10 M/s           | 4.83 M/s       | −5.3%  |
| 512   | 5.09 M/s           | 4.82 M/s       | −5.3%  |
| 4096  | 5.02 M/s           | 4.85 M/s       | −3.4%  |
| 65535 | 4.99 M/s           | 4.84 M/s       | −3.0%  |

Gap is flat across batch sizes → per-tick cost, not per-batch overhead.

`perf stat diff`: **instructions +12.9%**, IPC +3.6%, branch-miss
0.025→0.045% (absolute still negligible). Conclusion: we were
executing *more work per tick*, not stalling.

`perf record` top mover:

| Function | baseline | current | Δ (pp) |
|----------|---------:|--------:|-------:|
| `__memcmp_evex_movbe` (from `Partition::get_column`) | 14.42% | **20.85%** | **+6.43** |
| `Partition::get_column` self | 3.42% | 4.24% | +0.82 |
| everything else | — | — | within noise |

### Root cause

`ZeptoPipeline::store_tick` (`src/core/pipeline.cpp`) performed the
following per tick after the multi-table refactor in `5666f1b`:

```cpp
partition.get_column(COL_TIMESTAMP)->append<int64_t>(msg.recv_ts);
if (partition.get_column(COL_PRICE)->type() == ColumnType::FLOAT64)
    partition.get_column(COL_PRICE)->append<double>(msg.price_f);
else
    partition.get_column(COL_PRICE)->append<int64_t>(msg.price);
partition.get_column(COL_VOLUME   )->append<int64_t>(msg.volume);
partition.get_column(COL_MSG_TYPE )->append<int32_t>(...);
```

`Partition::get_column(const std::string&)` is a **linear scan** over
`std::vector<std::unique_ptr<ColumnVector>>` comparing `col->name() ==
name` → `memcmp`. The multi-table work introduced the `FLOAT64`
type-check branch which re-looks up `COL_PRICE` **twice** (once for
`->type()`, once for `->append<>`). Compiler cannot CSE across the
branch. Net: steady-state 6 `get_column` calls per tick, vs 4 in
baseline — a **50% increase in name-compare work per tick**.

---

## 2. Fix applied — cache the four column pointers locally

**File:** `src/core/pipeline.cpp` (only; 10 LOC net change; no header
touched, no ABI/API change, no test change).

```cpp
// AFTER
ColumnVector* ts_col  = partition.get_column(COL_TIMESTAMP);
ColumnVector* px_col  = partition.get_column(COL_PRICE);
ColumnVector* vol_col = partition.get_column(COL_VOLUME);
ColumnVector* mt_col  = partition.get_column(COL_MSG_TYPE);
ts_col->append<int64_t>(msg.recv_ts);
if (px_col->type() == ColumnType::FLOAT64)
    px_col->append<double>(msg.price_f);
else
    px_col->append<int64_t>(msg.price);
vol_col->append<int64_t>(msg.volume);
mt_col->append<int32_t>(static_cast<int32_t>(msg.msg_type));
```

Steady-state `get_column` count drops **6 → 4 per tick**. The
price-type re-lookup is eliminated because `px_col` is CSE-friendly
across the FLOAT64 branch as a local pointer.

**Safety:** the only eviction path inside `store_tick` evicts the
*oldest* partition, explicitly guarded by `oldest != &partition`, so
the cached pointers cannot dangle within one `store_tick` invocation.

Classification of the other two top items from the profile:

| Hotspot | Classification | Why not fixed here |
|---------|----------------|--------------------|
| `clock_gettime` vdso (20.20%) | **Inherent** | Needed for `recv_ts`, latency hist, WAL order; vdso is already ~3 ns. Lighter than baseline (−2.35 pp), not a regression contributor. |
| `memset` arena page-fault (4.43%) | **Inherent** | 4 KiB page first-touch zeroing; HugePages unavailable on bench host. Same as baseline (−0.60 pp). |
| `ensure_capacity` + memmove (~3%) | Future work | Small mild contributor; out of scope for this recovery. |

---

## 3. Measurement — x86_64 (5-run median)

| Batch | Baseline `875a4c3` | Pre-stage | **Post-stage** | Δ vs baseline |
|------:|-------------------:|----------:|---------------:|--------------:|
| 1     | 4.84 M/s           | 4.53 M/s  | **4.76 M/s**   | −1.7%         |
| **64**| **5.10 M/s**       | **4.83 M/s** | **5.06 M/s** | **−0.8%**    |
| 512   | 5.09 M/s           | 4.82 M/s  | **5.05 M/s**   | −0.8%         |
| 4096  | 5.02 M/s           | 4.85 M/s  | **5.05 M/s**   | +0.6%         |
| 65535 | 4.99 M/s           | 4.84 M/s  | **5.04 M/s**   | +1.0%         |

All five batch sizes now within ±2% of baseline. Batch=64 closes from
−5.3% to **−0.8%** — well inside the 5% GREEN band. Batches 4096 and
65535 actually overshoot baseline slightly (net +0.6–1.0%), attributed
to one fewer `get_column` call on every steady-state tick regardless
of batch.

## 4. Measurement — aarch64 / Graviton (3-run median)

Remote: `ec2-user@172.31.71.135`, Graviton clang 19.1.7, matching
compiler per 097.

| Batch | Post-stage |
|------:|-----------:|
| 1     | 4.03 M/s   |
| 64    | 3.97 M/s   |
| 512   | 3.99 M/s   |
| 4096  | 4.00 M/s   |
| 65535 | 3.99 M/s   |

Graviton shows the same fix-direction (pre-stage ~3.85–3.90 range from
094 tally; now 3.97–4.03). The arch ratio (x86 ~1.27× Graviton at
batch=64) is consistent with prior bench history.

## 5. Full matrix sanity

`./tools/run-full-matrix.sh --local --keep-going`:

| Stage              | Result | Wall |
|--------------------|--------|-----:|
| 1: build           | PASS   | 1 s  |
| 2: unit_x86        | PASS   | 73 s |
| 3: integration     | PASS   | 9 s  |
| 4: python          | PASS   | 0 s (skip) |
| 8: aarch64_unit_ssh| PASS   | 91 s |

**1361/1361 tests** pass on both x86_64 and aarch64. Filtered
ingest/pipeline/storage suite (89 tests) also green. No test file
touched.

## 6. Cross-reference

- **097** landed the MV fast-path + clang unification — removed the
  largest non-ingest regression.
- **098** packed `PartitionKey` into a single `uint64_t` with splitmix64
  hash — removed per-lookup hash cost in `PartitionManager`.
- **099 (this)** closes the last ingest gap by removing redundant
  `Partition::get_column` name-compares in `store_tick`.

Together, 097+098+099 bring BENCH 1 ingest fully back to baseline
across all batch sizes and VWAP query within the residual noted in P7.

## 7. Future work

| Item | Classification | Note |
|------|----------------|------|
| `ColumnVector::ensure_capacity` memmove (~3% self) | Simple, small | Grow by 2× is already the policy; could pre-reserve based on expected ticks/hour. |
| `memset` page-fault (~4.4%) | Inherent on this host | Re-enable 2 MiB HugePages on bench AMI; kernel fallback is 4 KiB zeroing. |
| `vdso clock_gettime` (~20%) | Inherent | Cannot remove without breaking `recv_ts` / WAL ordering. |
| Medium-fix version (named-slot column accessors) | Not needed | Simple fix recovered enough; keep `Partition` ABI unchanged. |

---

## Files changed

- `src/core/pipeline.cpp` — 10 LOC net: introduce 4 local pointer
  caches, remove 6 redundant `get_column` calls per tick.
- `docs/devlog/099_ingest_path_recovery.md` — this file.
- `docs/COMPLETED.md` — add closeout bullet.
- `docs/BACKLOG.md` — close ingest-throughput portion of P7.

No header, no public API, no test changes.
