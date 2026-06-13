# 161: P3 Agent Memory Agent-Only EKS Harness

Date: 2026-06-04
Status: Complete

## Context

P3 Agent Memory follow-up work needs a fast EKS verification loop. The existing
`tests/k8s/run_eks_bench.sh` always ran the full compat/HA/performance harness
before Agent Memory E2E, which made targeted Agent Memory changes expensive to
validate after the core cluster checks were already green.

## Changes

- Added `--agent-only` to `tests/k8s/run_eks_bench.sh`.
- `--agent-only` sets the run to K8s-only, skips the general amd64/arm64
  compat/HA/performance chains, and runs only
  `tests/k8s/test_k8s_agent_memory.py`.
- The mode still preserves EKS wake, node-readiness checks, image repo/tag
  overrides, cleanup, result directory output, `--keep`, and final summary
  reporting.
- Added validation for incompatible flag combinations:
  `--agent-only --engine-only` and `--agent-only --skip-agent-e2e`.
- Updated Agent Memory design docs, BACKLOG, COMPLETED, and the original EKS
  Agent Memory devlog follow-up.

## Verification

- `bash -n tests/k8s/run_eks_bench.sh`
- `tests/k8s/run_eks_bench.sh --help`
- `tests/k8s/run_eks_bench.sh --agent-only --engine-only`
- `tests/k8s/run_eks_bench.sh --agent-only --skip-agent-e2e`
- `git diff --check`

The incompatible flag probes returned the expected validation failures without
touching the cluster.

## Follow-ups

- Agent Memory snapshot failure/latency metrics were completed in devlog 162.
