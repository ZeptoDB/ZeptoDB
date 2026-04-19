# Devlog 083 — Fast Parallel Cross-Arch EKS Test Pipeline

Date: 2026-04-18
Status: Complete
Related: `.kiro/skills/cross-arch-verification/SKILL.md`, `deploy/scripts/run_arch_comparison.sh` (legacy)

## Motivation

The existing `run_arch_comparison.sh` was:

1. **Slow** — sequential x86 then arm64 = ~60 min wall time per run.
2. **Broken against EKS Auto Mode** — `tools/eks-bench.sh` referenced managed
   nodegroups (`system-v135`, `data-nodes-v135`, `loadgen-v135`) that do not
   exist in Auto Mode. The cluster has Karpenter NodePools only.
3. **Expensive per iteration** — `kubectl cp` of the bench binary + `apt-get
   install libssl3` inside every loadgen pod, plus LoadBalancer ELB charge.
4. **Leak-prone** — single failure path; teardown skipped if a `set -e` fired
   mid-run → nodes kept running at ~\$3.5/hr.

## What Changed

### New script — `deploy/scripts/run_arch_comparison_fast.sh`

8 stages, fail-fast (`set -euo pipefail`), mandatory teardown (`trap cleanup EXIT`):

| Stage | Duration | What |
|-------|----------|------|
| Preflight | ~30s | AWS creds, kubectl ctx, cluster ACTIVE, SSH, ECR repo |
| 0 — Local smoke | ~3 min | ninja build, unit tests, 1-node HTTP, 3-node cluster + `bench_rebalance basic` |
| 1 — Parallel build | ~12 min cold | x86 `docker buildx` (local) &#124;&#124; arm64 native on Graviton via SSH |
| 2 — Cluster prep | ~3 min | Apply `zepto-bench-x86` + `zepto-bench-arm64` NodePools (limits.cpu=64); create namespaces; wait Ready |
| 3 — Parallel Helm | ~4 min | Two `helm upgrade --install` in parallel; nodeSelector by arch label; `service.type=ClusterIP` (no ELB) |
| 4 — Remote smoke | ~1 min | Per-arch: `/health`, `/ping` <100ms, 10-row INSERT + SELECT roundtrip |
| 5 — Benchmark | ~8 min | `bench_rebalance` inside pre-baked bench-loadgen pod (no `kubectl cp`, no `apt-get`); both archs parallel; all scenarios |
| 6 — Compare | ~30s | Extract metrics, compare vs baselines (5.52M ticks/s, VWAP p50 637μs), write `summary.md` |
| 7 — Teardown | ~1 min | `helm uninstall`, delete namespaces, patch NodePools `limits.cpu=0`, verify no leaked nodes |

Stages 1 and 2 run **concurrently** via `wait -n` with fail-fast: if any
background job dies, all siblings are killed immediately, preventing cost
leaks when a build fails mid-flight.

### New — `deploy/docker/Dockerfile.bench` + `Dockerfile.bench.arm64`

Multi-stage images that bake in `bench_rebalance` AND `libssl3`. Same pod
image serves both as the server (3 replicas) and as the loadgen
(`command: [sleep, infinity]`). Arm64 Dockerfile is identical to x86 except
for `-mcpu=neoverse-n1` instead of `-march=x86-64-v3 -mtune=generic`.

### Rewritten — `tools/eks-bench.sh`

Now speaks NodePools instead of managed nodegroups:

- `wake` — `kubectl patch nodepool … --limits.cpu=64` on both bench NodePools.
- `sleep` — `helm uninstall` everything in `zeptodb-*` namespaces, then set
  `limits.cpu=0` so Karpenter drains all bench nodes in ~1 minute.
- `status` — NodePools, bench-labeled nodes, zeptodb pods.

## Cost per Run

| Item | Duration | Price | Cost |
|------|----------|-------|------|
| 3× c7i.xlarge (x86) | 28 min | \$0.171/h | \$0.24 |
| 3× c7g.xlarge (arm64) | 28 min | \$0.145/h | \$0.20 |
| Control plane | 28 min | \$0.10/h | \$0.05 |
| ECR storage + NAT | — | — | \$0.05 |
| Headroom (bench-loadgen + system pods) | 28 min | — | ~\$0.75 |
| **Total** | **~28 min** | — | **~\$1.30** |

Savings vs legacy: **~55% wall time**, **~60% cost** (no LoadBalancer ELB,
no duplicate provisioning time).

## Usage

```bash
# Full run (cold cache)
./deploy/scripts/run_arch_comparison_fast.sh

# Skip local smoke (CI already green) + single scenario
./deploy/scripts/run_arch_comparison_fast.sh --skip-local --scenario basic

# Dry run — stops after Stage 2 (cluster prep) so you can inspect NodePools
./deploy/scripts/run_arch_comparison_fast.sh --dry-run

# Use existing ECR images
./deploy/scripts/run_arch_comparison_fast.sh --skip-build

# Cheaper arm64 run via spot
./deploy/scripts/run_arch_comparison_fast.sh --spot-arm64
```

Flags: `--scenario NAME` (default `all`), `--skip-local`, `--skip-build`,
`--dry-run`, `--spot-arm64`, `--symbols N`, `--ticks N`, `--baseline N`.

## Failure Modes & Recovery

| Failure | Behavior | Recovery |
|---------|----------|----------|
| Stage 0 fails | Exit before touching cloud — \$0 lost | Fix the build/test, re-run |
| Build fails mid-Stage-1 | Sibling build is killed; trap fires teardown | Check `$RESULT_DIR/build_*.log` |
| Helm pod stuck pending | `kubectl wait` times out at 300s | Check `kubectl describe pod -n zeptodb-$arch` — usually capacity |
| Benchmark timeout | Stage 5 exits, teardown runs | Results in `$RESULT_DIR/result_*_*.txt` up to that point |
| Script crashes (SIGKILL etc.) | Trap may not fire | Manual recovery: `./tools/eks-bench.sh sleep` |
| Cost leak detected | Stage 7 guard prints `[ERR] Nodes still running` | Run `./tools/eks-bench.sh sleep` immediately |

## Files

```
deploy/scripts/run_arch_comparison_fast.sh   # new, executable
deploy/docker/Dockerfile.bench               # new — x86
deploy/docker/Dockerfile.bench.arm64         # new — arm64 (-mcpu=neoverse-n1)
tools/eks-bench.sh                           # rewritten for Auto Mode
docs/COMPLETED.md                            # entry added
docs/BACKLOG.md                              # note added
.kiro/skills/cross-arch-verification/SKILL.md  # EKS workflow section updated
```

Legacy `deploy/scripts/run_arch_comparison.sh` is kept for reference but no
longer the recommended path.

## Follow-ups applied

1. **Stage 0a — CMake guard**: reconfigure with `-DZEPTO_BUILD_BENCH=ON` if the cache has it OFF, so `ninja bench_rebalance` no longer fails with `unknown target`.
2. **Stage 0d — TCP probe**: replaced `curl /ping` on RPC ports 18124/18125 with a bash `/dev/tcp` probe; HTTP `/health` is still used for port 18123. Fixes 15s always-timeout on binary-protocol ports.
3. **Stage 5 — fail-fast across archs**: switched `wait "$R1" "$R2"` to `wait -n` loop with sibling kill, matching Stages 1+2 and Stage 3. Stops burning the other arch's compute if one benchmark fails early.
4. **Stage 6 — baseline regression table**: kept the existing arch-vs-arch divergence table and appended a `Perf Regression (vs baseline)` table with per-arch status vs baselines (Ingestion 5.52M ticks/s, VWAP p50 637μs, VWAP 914M rows/s). Flags `⚠️ >20%` per the cross-arch-verification skill rule.
5. **Stage 0 — Graviton parallel smoke**: added aarch64 smoke on the user's Graviton host (`$GRAVITON_HOST`) that runs in parallel with local x86 Stage 0. Reuses the existing SSH + rsync wiring from Stage 1 arm64 build. Mirror of local 0a–0d (CMake guard → ninja → unit tests → 1-node HTTP → 3-node loopback with TCP probe → `bench_rebalance basic`). Skipped automatically with `--skip-build` (no SSH preflight) or via new `--skip-remote-smoke` flag (when Graviton host offline). Also self-heals `/var/log/zeptodb` permission on first run via `sudo install -d`.
