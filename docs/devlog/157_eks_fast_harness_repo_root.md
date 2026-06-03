# 157: EKS Fast Harness Repo Root Handling

Date: 2026-06-02
Status: Complete

## Context

The fast cross-architecture EKS harness failed before cluster workload
deployment when invoked from the repository root. Stage 0 changed into `build/`
and then reused `$(dirname "$0")/../..` as the rsync source, which resolved to a
non-existent path under `build/`. The same local stage also used an unqualified
`wait`, which could reap the background Graviton smoke process before its
explicit status check.

## Changes

- `deploy/scripts/run_arch_comparison_fast.sh` now resolves `SCRIPT_DIR` and
  `REPO_ROOT` once from `BASH_SOURCE[0]` and uses that absolute root for local
  build, rsync, and stage orchestration paths.
- The local 3-node smoke cleanup now waits only for the node processes it
  started, preserving the background Graviton smoke process for the later
  explicit `wait "$REMOTE_PID"` check.

## Verification

- `bash -n deploy/scripts/run_arch_comparison_fast.sh`
- `./deploy/scripts/run_arch_comparison_fast.sh --skip-local --skip-build --dry-run`
  - Preflight passed against EKS cluster `zepto-bench`.
  - NodePools `zepto-bench-x86` and `zepto-bench-arm64` reached Ready.
  - Teardown reset both NodePool CPU limits.
- `./deploy/scripts/run_arch_comparison_fast.sh --skip-local --scenario smoke --arrow-smoke --symbols 10 --ticks 1000 --baseline 3 --rebalance-timeout 5 --bench-timeout 180`
  - x86_64 and arm64 bench images built and pushed.
  - Helm deployed both namespaces and both loadgen pods reached Ready.
  - Remote smoke passed on both architectures after transient kubelet TLS
    `kubectl exec` retries.
  - Arrow IPC ingest smoke passed on both architectures.
  - `bench_rebalance --scenario smoke` passed on both architectures.
  - Summary written to `/tmp/arch_fast_20260602_190306/summary.md`.
- `./tools/eks-bench.sh status`
  - `zepto-bench-x86` CPU limit: `0`
  - `zepto-bench-arm64` CPU limit: `0`
  - No bench-labeled nodes or ZeptoDB pods remained.

## Follow-ups

- Resolved in devlog 158: the full cross-arch
  `./deploy/scripts/run_arch_comparison_fast.sh --skip-local --skip-build --arrow-smoke`
  run now passes all scenarios on x86_64 and arm64 after table-aware
  coordinator SELECT routing, stable table ids, remote single-tick RPC
  drain-before-ACK, and stricter Stage 5 result validation.
