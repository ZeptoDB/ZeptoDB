# Devlog 094 — Full-matrix orchestrator perf optimizations (code-level)

Date: 2026-04-18
Status: ✅ Completed (code); infra item #1 deferred.

## Context

The `--local` plan built up by devlogs 092 + 093 was wall-clock ~260 s. An
analysis of where the time went found the aarch64 SSH stage (stage 8) taking
**3.43×** as long as the x86 unit stage (stage 2) on the same commit:
~2.0× from the Graviton dev box being 4-core vs the local 8-core workstation,
~1.3× per-core speed difference, and a further ~1.3× tail-amplification
(a handful of slow tests — `WorkerPool.WaitIdle`, `Snapshot.TwoNodeSnapshot`,
`ZeptoPipelineTest.CompressionThrottling` — dominate the last few seconds of
`ctest -j4`).

Since stage 8 is the critical path, everything else that runs **after** it is
pure tail to total wall time. This devlog collapses that tail.

## Optimizations applied

| #   | Where                               | Change                                                                                           | Before                       | After                                      |
|-----|-------------------------------------|--------------------------------------------------------------------------------------------------|------------------------------|--------------------------------------------|
| #1  | infra (deferred)                    | Resize Graviton dev box 4c → 8c                                                                  | 4c                           | deferred                                   |
| #2  | `tools/run-full-matrix.sh`          | Parallelize stages 2+8+3+4 (previously 3+4 ran sequentially **after** the 2‖8 parallel block)    | 2‖8 then 3 then 4            | 2‖8‖3‖4 (one fork group)                   |
| #3  | `tools/run-full-matrix.sh` (stage 8) | Skip rsync entirely when `git rev-parse HEAD` matches the last-synced SHA cached under `~/.cache/zepto_matrix/last_sync_<host8>`; add `--info=stats2 --human-readable`; add `--checksum` only under new `--force-resync` | rsync every run (~5–15 s)   | rsync first run, skip on clean-tree reruns |
| #6  | `tools/run-full-matrix.sh`          | Remove stage 6 from `--eks` and `--all` shortcuts (keep `--stages=6` for back-compat, mark deprecated in `--help`). Stage 8 already covers the same aarch64 unit suite, for free, via persistent Graviton SSH. | `--eks`=1,2,8,3,4,6,7 / `--all`=1,2,8,3,4,5,6,7 | `--eks`=1,2,8,3,4,7 / `--all`=1,2,8,3,4,5,7 |
| #7  | `tools/run-full-matrix.sh` (stage 8) | Bump remote `ctest --timeout 180` → `300` (Graviton is 4c vs local 8c; `WorkerPool.WaitIdle` has been observed to hit 180.02 s)                                                     | 180 s per test               | 300 s per test (with inline comment)       |
| #8  | `tools/run-full-matrix.sh`          | Covered by #2 — verifies the final loop fires 2+8+3+4 in parallel, not sequentially.             | —                            | —                                          |
| #9  | `tests/k8s/run_eks_bench.sh`        | Parallelize amd64 chain (compat→HA+perf) ‖ arm64 chain (compat+HA+perf). They use disjoint namespaces so cluster-level state doesn't collide. | 3a → 3b → 3c (sequential)    | 3a+3b ‖ 3c                                 |
| #10 | `tests/k8s/run_eks_bench.sh`        | Skip Karpenter arm64 NodePool provision when ≥3 arm64 nodes are already Ready.                   | ~1–5 min every run           | short-circuits on warm cluster             |
| #11 | `tools/run-full-matrix.sh` (stage 5)| Fallback branch (`run_arch_bench.sh` without `--local-only`): cap each bench at 15 s not 60 s. Smoke-level only; real numbers come from `run_arch_bench.sh` directly. | `timeout 60`                 | `timeout 15`                               |

## Expected wall-time impact

| Scenario                       | Before   | After (code-level)  | Notes                                                            |
|--------------------------------|----------|---------------------|------------------------------------------------------------------|
| `--local` cold                 | ~260 s   | **~max(2,8,3,4)**   | Dominated by stage 8 (Graviton) until infra #1 lands.            |
| `--local` warm (no source change) | ~260 s | ~max(2,3,4) + ~1 s rsync-skip for 8 | stage 8 rsync elides, ninja+ctest still runs remotely.           |
| `--eks` full                   | ~20 min  | ~15–17 min          | #9 removes the amd64-arm64 serialization (~3 min), #10 elides Karpenter provision when warm (~1–5 min), #6 drops stage 6 altogether. |
| `--all` fallback bench         | ~3 min   | ~45 s               | #11 shrinks the 3×60 s bench fallback to 3×15 s.                 |

The deferred infra change (#1: Graviton 4c → 8c) is the one remaining lever
to close the 3.43× arm64 gap. Without it, stage 8 remains the critical path
and the `--local` wall-time floor is whatever stage 8 takes.

## Files touched

- `tools/run-full-matrix.sh` — #2, #3, #6 (shortcuts + help), #7, #8, #11
- `tests/k8s/run_eks_bench.sh` — #9, #10
- `docs/devlog/094_full_matrix_perf_optimizations.md` (this file, new)
- `docs/devlog/092_full_matrix_test_script.md` — stage 6 marked deprecated
- `docs/devlog/093_parallel_arm64_unit_stage.md` — "see also 094" footer
- `CONTRIBUTING.md` — `--eks` / `--all` examples updated (no stage 6)
- `docs/COMPLETED.md` — bullet added

## Non-changes (intentional)

- Stage 6 (`aarch64_unit` via EKS buildx+ECR+kubectl) is **kept** as an
  explicitly-selectable stage (`--stages=6`). It is the only reproducible-
  from-clean-image path and remains valuable for CI runners that cannot
  SSH into the persistent Graviton dev host. Removing it from the default
  plans does not delete the code path.
- Fail-fast semantics are unchanged. The N-way `run_stages_parallel_many`
  helper waits for all children, collates results into `RESULTS_*`, then
  exits with the first non-zero rc if `--keep-going` is off.
- The 2-way `run_stages_parallel` helper is kept as a back-compat wrapper
  over the N-way helper for callers that only have stages 2+8 selected
  (e.g. `--stages=1,2,8`).

## Verification

1. `./tools/run-full-matrix.sh --dry-run --all` → plan shows stages **1, 2, 8, 3, 4, 5, 7** (no 6). ✅
2. `./tools/run-full-matrix.sh --dry-run --eks` → plan shows **1, 2, 8, 3, 4, 7** (no 6). ✅
3. `./tools/run-full-matrix.sh --dry-run --stages=6` → plan shows **6** alone (back-compat). ✅
4. `grep -n` on `tests/k8s/run_eks_bench.sh`:
   - `EXISTING_ARM` / "skipping Karpenter provision" lines (item #10) — present. ✅
   - `PID_AMD` / `PID_ARM` / `wait $PID_AMD` / `wait $PID_ARM` (item #9) — present. ✅
5. `bash -n tools/run-full-matrix.sh && bash -n tests/k8s/run_eks_bench.sh` — both clean.

A live `./tools/run-full-matrix.sh --local` (first run + second run to
exercise the rsync-skip) is recommended to confirm per-stage timings and
to observe the new `── Source tree unchanged since last sync … ──` banner
in the stage-8 log on the second invocation. The first-run stage-8 log
should now include `--info=stats2 --human-readable` output and write the
marker file `$HOME/.cache/zepto_matrix/last_sync_<host8>`.
