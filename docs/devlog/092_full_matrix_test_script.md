# Devlog 092 — Full test matrix orchestrator (`tools/run-full-matrix.sh`)

Date: 2026-04-18
Status: ✅ Completed

## Motivation

ZeptoDB's test surface had grown into seven disjoint runners:

| Layer | Entry point |
|---|---|
| Build | `ninja zepto_tests test_feeds test_migration zepto_http_server zepto_data_node zepto-cli` |
| C++ unit (x86) | `ctest -j$(nproc) -E 'Benchmark\.\|K8s'` |
| Integration | `tests/integration/*.sh` (three scripts) |
| Python | `pytest tests/python/` (7 modules) |
| Native bench | `tests/bench/run_arch_bench.sh` (+ `bench_*` binaries) |
| aarch64 unit | `tools/run-aarch64-tests.sh` (Graviton via EKS) |
| EKS K8s + bench | `tests/k8s/run_eks_bench.sh` (compat / HA / perf + engine bench) |

Running everything before a release meant stitching them by hand, remembering to wake/sleep the EKS bench cluster exactly once, and tracking which step failed. This devlog introduces a single orchestrator — `tools/run-full-matrix.sh` — that composes the existing scripts without reimplementing them.

## Enumerated matrix

Scanned from the source of truth (no curated list):

### 1. `tests/CMakeLists.txt` → `add_executable` targets
- `zepto_tests` (35 unit sources under `tests/unit/`, +1 JIT source when `ZEPTO_USE_JIT`)
- `test_feeds` (FIX parser + NASDAQ ITCH)
- `test_migration` (q→SQL, q→Python, HDB loader, ClickHouse, DuckDB, TimescaleDB)
- `bench_feeds` (opt-in, requires `google/benchmark`)

At `ctest -N` time this currently materialises ~1374 test cases.

### 2. `tests/integration/*.sh`
- `test_http_hdb.sh`
- `test_http_tenant.sh`
- `test_multiprocess.sh`

### 3. `tests/python/` pytest suites
- `test_arrow_integration.py`, `test_client_ddl.py`, `test_fast_ingest.py`,
  `test_ingest_batch.py`, `test_pandas_integration.py`, `test_polars_integration.py`,
  `test_streaming.py`, `test_table_aware_ingest.py`

### 4. `tests/k8s/`
- `test_k8s_compat.py` (27 compat tests, amd64 + arm64)
- `test_k8s_ha_perf.py` (11 HA+perf tests, amd64 + arm64)
- `test_k8s_seoul.py`, `run_arm64_tests.py`, `run_k8s_compat.sh` (invoked by `run_eks_bench.sh`)

### 5. `tests/bench/*` binaries (opt-in)
- `bench_pipeline`, `bench_simd_jit`, `bench_sql`, `bench_parallel`, `bench_hdb`,
  `bench_cxl`, `bench_planner`, `bench_planner_benefit`, `bench_rebalance`, `bench_python.py`
- Meta-runner: `tests/bench/run_arch_bench.sh`

### 6. Cross-arch
- `tools/run-aarch64-tests.sh` — builds `linux/arm64` test image via buildx, pushes to ECR, runs `/build/build/tests/zepto_tests` on a Graviton node. Requires `REPO` or `SKIP_PUSH=1`.

### 7. EKS full
- `tests/k8s/run_eks_bench.sh` — wake → provision arm64 NodePool → run amd64 compat+HA → arm64 compat+HA → native engine bench → sleep. Supports `--keep`, `--skip-wake`, `--k8s-only`, `--engine-only`.

## Stage table

| # | Name | Default | Cost | Backing command |
|---|------|:-:|:-:|---|
| 1 | `build` | ON | — | `ninja -j$(nproc) zepto_tests test_feeds test_migration zepto_http_server zepto_data_node zepto-cli` |
| 2 | `unit_x86` | ON | — | `ctest -j$(nproc) -E 'Benchmark\.\|K8s' --output-on-failure --timeout 180` (excl. `Benchmark.*` and `K8s*` unit tests) |
| 3 | `integration` | ON | — | each executable `tests/integration/*.sh`, fail on first non-zero |
| 4 | `python` | ON | — | `pytest tests/python/ -x --timeout=120` (skipped with WARN if pytest or `zeptodb` module unavailable) |
| 5 | `bench_local` | OFF | — | `tests/bench/run_arch_bench.sh --skip-build --local-only` if available, else `bench_sql`/`bench_pipeline`/`bench_parallel` with 60 s cap |
| 6 | `aarch64_unit` | OFF *(deprecated, see [094](094_full_matrix_perf_optimizations.md))* | ~$0.50 | `tools/run-aarch64-tests.sh` (needs `--repo=<ecr>` or `SKIP_PUSH=1`). Stage 8 now covers the same aarch64 unit suite for free; stage 6 is retained only for CI runners without SSH access to the persistent Graviton dev host and must be requested explicitly via `--stages=6`. |
| 7 | `eks_full` | OFF | ~$1.00 add'l | `tests/k8s/run_eks_bench.sh --skip-wake --keep` |
| 8 | `aarch64_unit_ssh` | ON *(added in devlog 093)* | $0 | rsync + ssh + ninja + ctest against persistent Graviton host (runs in parallel with stage 2) |

Cost figures are derived from the EKS bench cluster hourly rate (~$4.60/hr) and typical wall times; see `tools/eks-bench.sh` and `tests/k8s/run_eks_bench.sh` header comments.

## CLI usage

```
tools/run-full-matrix.sh [options]

  --stages=LIST     Comma-separated stage numbers or names (default: 1,2,3,4)
                    Special: all, local (=1,2,3,4), eks (=1,2,3,4,6,7)
  --local           Shorthand for --stages=1,2,3,4
  --eks             Shorthand for --stages=1,2,3,4,6,7
  --all             Shorthand for --stages=1,2,3,4,5,6,7
  --keep-going      Don't fail-fast; run all selected stages
  --skip-build      Remove stage 1 from the plan
  --repo=URL        ECR repo for aarch64 image (stage 6/7)
  --dry-run         Print the plan without executing
  -h|--help
```

### Examples

```bash
# Pre-commit local matrix (stages 1-4, fail-fast)
./tools/run-full-matrix.sh --local

# Pre-release: local + cloud (excludes local benches, adds aarch64 + EKS)
./tools/run-full-matrix.sh --eks --repo=<account>.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb

# Kitchen-sink: everything including local benches
./tools/run-full-matrix.sh --all --repo=<account>.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb

# What would run?
./tools/run-full-matrix.sh --dry-run --all
```

## Wake/sleep coordination

When stages 6 and 7 are both in the plan, the orchestrator:

1. Touches `$LOG_DIR/.eks_awake` before running stage 6 and calls `tools/eks-bench.sh wake` once.
2. `tools/run-aarch64-tests.sh` installs its own `trap … sleep` on EXIT, so the cluster goes to sleep when stage 6 finishes. To prevent stage 7 from taking `--skip-wake` against a sleeping cluster, stage 7 re-wakes the cluster unconditionally on entry (wake is idempotent). `run_eks_bench.sh` is then passed `--skip-wake --keep` so its internal cleanup leaves the nodes up.
3. A global `trap … EXIT INT TERM` installed only when stage 6/7 is selected runs `tools/eks-bench.sh sleep` exactly once on any exit path (success, failure, Ctrl-C) — this owns the final cluster state.

## Behaviour

- Banner prints selected stages, total estimated cost in USD, fail-fast mode, and log directory `/tmp/zepto_full_matrix_<timestamp>/`.
- Each stage: `step()` banner → run via `bash -c` with `tee` to `stage_<n>_<name>.log` → captured exit code + wall time.
- Fail-fast (default): on non-zero exit, print summary and exit with the stage's return code.
- `--keep-going`: run all stages regardless, exit non-zero if any failed.
- Summary table at end: stage name, PASS/FAIL, wall seconds.

## Constraints respected

- Bash, `set -euo pipefail`.
- No new dependencies beyond what the composed scripts already require.
- ~260 lines; does not reimplement any existing runner — all heavy lifting is delegated.
- Respects `run_eks_bench.sh` flags (`--skip-wake --keep`) so the orchestrator owns wake/sleep.

## Verification

- `./tools/run-full-matrix.sh --dry-run --all` → plan with 7 stages, \$1.50 total.
- `./tools/run-full-matrix.sh --local --dry-run` → plan with 4 stages, \$0.00 total.
- `./tools/run-full-matrix.sh --local --keep-going` → build 0 s (cached), unit_x86 74 s (1374 tests, 1 flaky re-passed), integration 9 s (3 scripts, 5 tests each pass), python 0 s (skipped with WARN because `zeptodb` module not installed in system python). Total wall ~83 s.
- Stages 6 and 7 were not executed during verification per cost guardrail.

## Docs updated

- `docs/devlog/092_full_matrix_test_script.md` (this file)
- `docs/COMPLETED.md` — entry added
- `CONTRIBUTING.md` — "Running the full test matrix" subsection added

## Files

- `tools/run-full-matrix.sh` (new, executable, 261 lines)
