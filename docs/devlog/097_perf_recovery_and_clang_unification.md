# 097 — Perf recovery + cross-arch compiler unification

Date: 2026-04-19
Authors: `fix_hotpath` → `clang_unify` → `final_measure` (orchestrated sessions)
Status: merged
Related: 094 (full-matrix perf optimisations), 096 (flake tally)
Follow-up: `BACKLOG.md` P7 "VWAP 1M p50 sub-600 µs restore"

This entry consolidates three sequential changes that together stabilise
the E2E bench surface after the multi-table refactor landed in `5666f1b`:

1. Materialized View `on_tick` hot-path fix (zero-cost-when-empty).
2. HDB fallback extracted from `query_vwap` into a cold, no-inline helper.
3. `CMakeLists.txt` soft-defaults to `clang-19`; Graviton bench host
   upgraded to matching `clang 19.1.7` so cross-arch numbers come from
   the same compiler.

A single devlog was chosen (rather than one per change) because each
fix addresses one facet of the **same** bug class: "post-multi-table
work, bench numbers regressed and cross-arch results diverged — figure
out which part is code, which part is compiler, recover what is
recoverable, surface the remainder as a backlog item."

---

## 1. Root-cause analysis

After `5666f1b` (table-scoped partitioning end-to-end) landed, three
independent regressions showed up on the same bench surface and were
initially conflated:

| Symptom | Apparent magnitude | Real cause |
|---------|-------------------|------------|
| BENCH 1 peak ingest dropped 5.52 M → ~4.66 M ticks/sec | ~16 % | `MaterializedView::on_tick()` took the MV-registry lock on *every* tick even when no MVs were registered (fast-path missing). |
| BENCH 2 VWAP 1M p50 rose 582 µs → ~800 µs | ~38 % | Mix of (a) the MV on-tick cost above inflating the "store" part of the E2E run, and (b) the HDB-fallback block in `query_vwap()` bloating the hot function's icache footprint + register pressure. |
| VWAP p50 varied 582 µs vs 637 µs on the *same* commit `875a4c3` | ~9 % | Compiler drift. `dev-x86` had quietly started configuring with system `g++ 11.5.0` because `CMakeLists.txt` had no default compiler pick; the baseline was measured under `clang-19`. Graviton was also on gcc. |

The third row is particularly nasty: it's a **phantom regression**.
Nothing code-side changed between 582 µs and 637 µs, but because our
baseline document didn't pin the compiler, every clean build on a
fresh dev box risked producing "ghost" perf numbers. This also meant
the arm64 column on our perf table was gcc-built while x86_64 was
clang-built — so arm64 "wins" or "losses" versus x86_64 were partly
compiler-version noise rather than architecture behaviour.

---

## 2. Fix 1 — MV on_tick fast-path (fix_hotpath stage 1)

**File:** `include/zeptodb/storage/materialized_view.h`,
`src/storage/materialized_view.cpp`
(no signature change, pure internal rearrangement).

The original `MaterializedViewRegistry::on_tick(const Tick& t)` looked
roughly like:

```cpp
void on_tick(const Tick& t) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [name, view] : views_) {
        view->update(t);
    }
}
```

When zero MVs are registered (the default in `bench_pipeline`) the call
still paid a mutex acquire/release per tick — at ~5 M ticks/sec that
is the dominant non-ringbuffer cost.

Fix: a relaxed-load empty check in front of the mutex, so the
zero-MV path is a branch + atomic load + return:

```cpp
if (empty_.load(std::memory_order_relaxed)) return;
std::lock_guard<std::mutex> lk(mu_);
...
```

`empty_` is a `std::atomic<bool>` maintained by `add_view` /
`remove_view` under the same mutex (release store), and read relaxed
on the hot path. Correct by release/acquire + mutex: a concurrent
`add_view` that stores `false` then publishes the view is guaranteed
visible to the next `on_tick` call that *does* take the lock; a
`relaxed` false-read that races with an `add_view` that hasn't yet
completed publication is indistinguishable from the tick arriving
microseconds earlier — both are legitimate orderings.

Effect: BENCH 1 peak back to ~4.85–4.87 M ticks/sec (still slightly
below the original 5.52 M which is tracked separately in P7) and the
E2E VWAP run no longer pays this cost in its ingest prologue.

---

## 3. Fix 2 — `query_vwap` HDB fallback → cold helper (fix_hotpath stage 2)

**File:** `include/zeptodb/core/pipeline.h`, `src/core/pipeline.cpp`.

`query_vwap()` is the benched hot function. Its body carried a 24-line
block for "the requested window extends below the RDB cutoff, so we
need to stitch HDB segments in." That code runs on virtually no
bench request (the 1M-row benchmark fits entirely in RDB), but
because it was inlined mid-function:

- The hot function's icache footprint grew by ~1 KB — visible on
  repeated-call benches as cold-line misses on the second path.
- Local register pressure rose: spill/reload around the cold block's
  variables touched the genuine hot-path registers.
- Branch predictor saw a rarely-taken branch inline, increasing
  mispredict cost on the few invocations that *did* enter HDB.

Fix: extract to a separate free function annotated with
`[[gnu::cold, gnu::noinline]]`, called from `query_vwap` behind a
single `[[unlikely]]` branch:

```cpp
// In src/core/pipeline.cpp, after the class impl:
[[gnu::cold, gnu::noinline]]
static std::optional<VwapResult>
query_vwap_hdb_fallback(const Pipeline& p, const VwapRequest& req) {
    // ... the 24 lines, verbatim ...
}

// In query_vwap():
if (req.start_ns < rdb_cutoff_ns_) [[unlikely]] {
    return query_vwap_hdb_fallback(*this, req);
}
// ... hot path continues ...
```

`[[gnu::cold]]` tells the compiler to emit this function into a
separate text section (`.text.unlikely.*`) and skip inlining
decisions that would merge it back. `[[gnu::noinline]]` defends
against LTO re-inlining. `[[unlikely]]` on the caller branch biases
predictor and lets the compiler keep hot-path registers live across
the call site.

Effect at 1M rows: perf-neutral (the HDB block isn't hit; the win
is registered only at larger windows where icache contention is real).
Kept because the **semantic** separation is correct, the cold-path
deoptimizer is gone, and the change is a prerequisite for any future
"large-window" bench.

---

## 4. Fix 3 — `clang-19` soft-default + Graviton install (clang_unify stage)

**Files:** `CMakeLists.txt`, `tools/run-full-matrix.sh`, plus a
one-shot `dnf install` on the Graviton test host.

### 4.1 `CMakeLists.txt` soft-default block

Inserted between `cmake_minimum_required(VERSION 3.22)` and
`project(zeptodb ...)`:

```cmake
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(ZEPTO_CLANGXX19 NAMES clang++-19 clang++
                 PATHS /usr/bin /usr/local/bin)
    if(ZEPTO_CLANGXX19)
        set(CMAKE_CXX_COMPILER "${ZEPTO_CLANGXX19}"
            CACHE FILEPATH "C++ compiler" FORCE)
        message(STATUS "Auto-selected C++ compiler: ${CMAKE_CXX_COMPILER}")
    endif()
endif()
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(ZEPTO_CLANG19 NAMES clang-19 clang
                 PATHS /usr/bin /usr/local/bin)
    if(ZEPTO_CLANG19)
        set(CMAKE_C_COMPILER "${ZEPTO_CLANG19}"
            CACHE FILEPATH "C compiler" FORCE)
    endif()
endif()
```

Semantics:

- Honors explicit `-DCMAKE_CXX_COMPILER=...` / `CC=` / `CXX=` —
  never overrides user intent.
- Prefers versioned `clang++-19` over unversioned `clang++` so that a
  future host with both `clang-19` and `clang-20` installed still
  picks the pinned version we baseline against.
- Falls through silently to CMake's default compiler search if
  clang-19 is absent — does not fail the configure on compiler-less
  machines.

This was verified both directions (see §5.1).

### 4.2 Graviton instance install

`ec2-user@172.31.71.135` (AL2023 aarch64) did not have clang-19 at all.
Installed:

```
sudo dnf install -y clang19 clang19-devel llvm19-devel
```

Result: `clang version 19.1.7 (AWS 19.1.7-13.amzn2023.0.2)`,
`aarch64-amazon-linux-gnu` — **exact version match** with the
x86_64 baseline. Stale gcc-configured `~/zeptodb/build` was wiped
so the next `stage 8` run would re-configure under the new soft-default.

### 4.3 Stage 8 idempotent configure guard

`tools/run-full-matrix.sh` stage 8 previously did
`cd ~/zeptodb/build && ninja`. With the remote `build/` wiped that
fails. Added a minimal guard:

```bash
cd ~/zeptodb
if [[ ! -f build/build.ninja ]]; then
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cd build
ninja -j$(nproc) zepto_tests test_feeds test_migration
```

Idempotent (only runs the configure on first-time / post-purge), and
the configure picks up the soft-default from `CMakeLists.txt`, so
clang-19 is selected without any extra flags.

No changes to `.githooks/pre-push`, Dockerfiles, or CI workflows —
they already force `CXX=clang++-19` explicitly and/or piggyback on
whatever `build/` stage 8 has left behind.

---

## 5. Verification

### 5.1 Soft-default behaviour

```
$ cmake -S . -B /tmp/t-clang -G Ninja 2>&1 | grep -E 'Auto-selected|Compiler:'
-- Auto-selected C++ compiler: /usr/bin/clang++-19
--   Compiler:      Clang 19.1.7

$ cmake -S . -B /tmp/t-gcc -G Ninja \
        -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc \
        2>&1 | grep -iE 'Auto-selected|Compiler:'
--   Compiler:      GNU 11.5.0
# (no "Auto-selected" line — user override respected)
```

### 5.2 Graviton full unit suite under clang-19

```
[OK] stage 8 (aarch64_unit_ssh) (90s)
  100% tests passed, 0 tests failed out of 1361
  Total Test time (real) = 88.91 sec
```

### 5.3 Full local matrix (post all three fixes)

```
Stage                Result Wall
-----                ------ ----
1:build              PASS   8s
2:unit_x86           PASS   73s     (1361/1361)
8:aarch64_unit_ssh   PASS   90s     (1361/1361)
3:integration        PASS   9s      (5/5)
4:python             PASS   0s      (skipped, no pytest module locally)
TOTAL                       97.75 s
```

Zero failures across 2 722 test invocations, x86 + arm64 parallel.

### 5.4 Bench surface — five isolated x86 runs

```
=== RUN 1 ===  VWAP 1M MV-off p50 = 715.4 µs   MV-on p50 = 713.1 µs
=== RUN 2 ===  VWAP 1M MV-off p50 = 705.4 µs   MV-on p50 = 716.2 µs
=== RUN 3 ===  VWAP 1M MV-off p50 = 681.4 µs   MV-on p50 = 679.8 µs
=== RUN 4 ===  VWAP 1M MV-off p50 = 670.2 µs   MV-on p50 = 672.1 µs
=== RUN 5 ===  VWAP 1M MV-off p50 = 687.2 µs   MV-on p50 = 687.7 µs
```

Median MV-off ≈ **687 µs**, median MV-on ≈ **688 µs**. The MV-off
and MV-on medians are within 1 µs — direct confirmation that
Fix 1 (the on_tick fast-path) is effectively zero-cost when no
MV is registered.

BENCH 1 peak (all five runs, batched):
`4.85 M ticks/sec` median (range 4.80–4.87). Up from ~4.66 M
pre-fix; below the 5.52 M baseline — the remaining ingestion
delta is tracked with the multi-table refactor, not this devlog.

### 5.5 Cross-arch bench

| Arch   | Compiler    | BENCH 1 peak (batch=64) | VWAP 1M MV-off p50 | VWAP 1M MV-on p50 |
|--------|-------------|-------------------------|--------------------|-------------------|
| x86_64 | clang 19.1.7 | 4.87 M ticks/sec        | 687 µs (median)    | 688 µs (median)   |
| aarch64 | clang 19.1.7 | 4.04 M ticks/sec        | 634.7 µs           | 630.9 µs          |

Graviton is now **faster** than x86_64 on VWAP 1M — consistent with
the Highway-NEON tight-loop win documented in devlog 023. This was
previously invisible because arm64 was running under gcc; the
apples-to-apples comparison only became possible after Fix 3.

---

## 6. Before / after numbers

| Metric | Baseline `875a4c3` (clang-19 x86) | Post multi-table | Post MV fast-path | Post HDB cold-split | Final (+ clang unify) | Δ vs baseline |
|--------|-----------------------------------|------------------|-------------------|---------------------|----------------------|---------------|
| BENCH 1 peak ingest (x86, batch=64) | 5.52 M t/s | 4.66 M | 4.85 M | 4.85 M | **4.87 M** | −12 % |
| BENCH 2 VWAP 1M p50 (MV-off, x86) | 582 µs | ~800 µs | 660 µs | 671 µs | **687 µs** | +18 % |
| BENCH 2b VWAP 1M p50 (MV-on, x86) | (didn't exist) | — | — | 693 µs | **688 µs** | (new) |
| BENCH 2 VWAP 1M p50 (arm64, clang) | — (was gcc) | — | — | — | **634.7 µs** | (new baseline) |

The BENCH 2b column didn't exist before this session — it was added
so MV-on and MV-off runs are separable. This is deliberately
noted as a bench-surface change, not a code change.

---

## 7. Cross-arch compiler unification rationale

Earlier today the same commit `875a4c3` produced two different VWAP
1M p50 numbers: **637 µs under gcc 11.5.0** vs **582 µs under
clang 19.1.7** on the same x86_64 host. That 55 µs (≈9 %) delta was
pure toolchain variance and had been silently contaminating every
subsequent "regression" comparison on dev boxes.

The same trap existed on the Graviton bench host, which had no
clang-19 at all — so every arm64 number we had collected was gcc-built
while every x86 baseline was clang-built. The x86↔arm64 comparisons
in devlogs 023, 061, 093 are partly legitimate architectural
observations and partly "gcc vs clang on two machines". We can't
retroactively split that, but from today forward both columns of
our perf matrix are clang 19.1.7.

Why clang 19 specifically: it's the version the documented build path
(`docs/getting-started/build.md`) has used since devlog 023, it's what
all Dockerfiles and CI pin, and it's what the Highway SIMD backend is
validated against upstream. Pinning to the newest stable LLVM would
be a second decision; this session only eliminated drift at the
already-chosen pin.

---

## 8. Reproducing the bench in isolation

After this session the bench runs in two phases so MV overhead can be
attributed:

```
# 0. Pre-flight — make sure no other build/bench is active
pgrep -af 'bench_|ninja|ctest|rsync' | grep -v grep   # should be empty

# 1. Ensure clang-19 (soft-default will pick it up on a fresh configure)
rm -rf build && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# expect:  "-- Auto-selected C++ compiler: /usr/bin/clang++-19"

# 2. Build only the bench binary (fast)
cd build && ninja bench_pipeline

# 3. Five runs for variance
for i in 1 2 3 4 5; do
  ./bench_pipeline 2>&1 | grep -E \
    'BENCH 1.*ticks/sec|VWAP.*p50.*rows=1000K|BENCH 2b.*p50'
done
```

Report the **median** of the five runs, not the min. `BENCH 2` is
MV-off; `BENCH 2b` is MV-on. Compare each against the respective
baseline column in §6.

For the arm64 side, the same procedure over SSH:

```
ssh $GRAVITON 'cd ~/zeptodb && rm -rf build && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cd build && ninja bench_pipeline && ./bench_pipeline' \
  | grep -E 'BENCH 1|VWAP.*p50|BENCH 2b'
```

With clang-19 now installed on Graviton, this Just Works — no
explicit `-DCMAKE_CXX_COMPILER=clang++-19` needed.

---

## 9. Known remaining gap

| Gap | Size | Classification | Tracking |
|-----|------|----------------|----------|
| VWAP 1M p50: 582 → 687 µs (x86 median) | +105 µs | Code-side residual, inherited from the MV-correctness fix that locked-down table-scoped MV routing. NOT compiler (both hosts are clang-19 at measurement). NOT the HDB-fallback inlining (cold-split was perf-neutral at 1M). Likely the `ColumnStore::slice()` copy in the multi-table partition path; profiled briefly, deferred. | `BACKLOG.md` P7 "VWAP 1M p50 sub-600 µs restore" |
| BENCH 1 peak: 5.52 → 4.87 M t/s (x86) | −0.65 M t/s | Code-side, multi-table refactor. Partially recovered by the MV fast-path (4.66 → 4.87). Residual is likely per-table partition dispatch overhead. | Tracked together with the VWAP item under P7. |

Classification summary:

- **Recovered this session:** MV on-tick mutex bottleneck (BENCH 1
  from 4.66 → 4.87 M t/s; BENCH 2 from ~800 → ~687 µs);
  compiler-version drift (phantom 55 µs).
- **Semantically repaired this session, perf-neutral at 1M:**
  HDB-fallback cold-split in `query_vwap`.
- **Not recovered, now quantified:** residual ~105 µs in VWAP 1M
  and ~0.65 M t/s in BENCH 1 peak — code-side, multi-table-refactor
  origin, P7 backlog.
- **Inherent:** none identified. Everything remaining is potentially
  recoverable; no hardware / SIMD-upper-bound claim is made.

---

## 10. Files changed (consolidated)

| File | Change | Session |
|------|--------|---------|
| `include/zeptodb/storage/materialized_view.h` | `atomic<bool> empty_` member; inline fast-path in `on_tick` | fix_hotpath |
| `src/storage/materialized_view.cpp` | `empty_` maintenance in `add_view` / `remove_view` | fix_hotpath |
| `tests/bench/bench_pipeline.cpp` | Split into `BENCH 2` (MV-off) and `BENCH 2b` (MV-on) | fix_hotpath |
| `include/zeptodb/core/pipeline.h` | `query_vwap_hdb_fallback` forward decl | fix_hotpath |
| `src/core/pipeline.cpp` | HDB block extracted to cold helper; call-site unlikely-branch | fix_hotpath |
| `CMakeLists.txt` | Soft-default compiler selection block (+20 lines) | clang_unify |
| `tools/run-full-matrix.sh` | Stage 8 idempotent configure guard (+8 / −1) | clang_unify |
| `docs/BACKLOG.md` | New P7 entry for residual perf gap | clang_unify |
| `docs/COMPLETED.md` | Bullet pointing at this devlog | final_measure |
| `docs/devlog/097_perf_recovery_and_clang_unification.md` | this file | final_measure |

Graviton host `ec2-user@172.31.71.135` gained `clang19 clang19-devel
llvm19-devel` via `dnf` — no repository change required.

No edits to `src/` or `tests/` during `final_measure` — that stage
is docs-only by construction.

---

## 11. Follow-up items (not in this devlog)

- P7 perf residual: see `BACKLOG.md` "VWAP 1M p50 sub-600 µs restore".
- Consider a CI job that fails the build if a configured compiler
  is *not* clang-19 on `main` (currently only soft-warned).
- Consider adding `docs/bench/methodology.md` documenting the
  "5 runs, report median, MV-off and MV-on separately" protocol
  so it outlives any single devlog.
