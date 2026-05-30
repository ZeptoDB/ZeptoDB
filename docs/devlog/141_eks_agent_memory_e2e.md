# 141: EKS Agent Memory E2E

Date: 2026-05-30
Status: Complete

## Context

The existing EKS benchmark matrix verified Kubernetes scheduling, HA, and
cross-architecture mechanics, but it did not exercise the new Agent Memory HTTP
surface with real ZeptoDB images. The Agent Memory layer needed an EKS E2E that
stores memories and cache entries, performs retrieval, restarts the pod, and
confirms persisted state replays on both amd64 and arm64.

## Changes

- Added `tests/k8s/test_k8s_agent_memory.py`, which deploys real ZeptoDB bench
  images on the x86 and arm64 bench node pools and validates memory put/search,
  context assembly, tenant isolation, exact/semantic cache lookup, tombstones,
  stats, metrics, and restart persistence.
- Integrated Agent Memory E2E into `tests/k8s/run_eks_bench.sh` after the
  existing Kubernetes compat and HA/perf stages.
- Switched the Agent Memory E2E default persistence volume to node-local
  `hostPath` for the EKS Auto Mode bench cluster; PVC mode remains available for
  clusters with a supported CSI provisioner.
- Added port-forward retry handling for transient kubelet TLS/upgrade errors
  observed on arm64 nodes.
- Hardened HA04 pod-kill verification with endpoint preflight and a short
  service retry window to avoid single-probe EndpointSlice lag flakes.
- Added an amd64 nodeSelector to the x86 Kubernetes test values so the
  cross-arch harness keeps amd64 and arm64 tests isolated.

## Verification

- Built and pushed current bench images:
  - `060795905711.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb:bench-x86`
  - `060795905711.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb:bench-arm64`
- Local syntax checks:
  - `python3.13 -m py_compile tests/k8s/test_k8s_agent_memory.py tests/k8s/test_k8s_ha_perf.py`
  - `bash -n tests/k8s/run_eks_bench.sh`
- Agent-only EKS verification:
  - `python3.13 -u tests/k8s/test_k8s_agent_memory.py`
  - Result: `Agent Memory E2E Results: 2/2 passed, 0 failed`
- Final integrated EKS verification:
  - `./tests/k8s/run_eks_bench.sh --k8s-only`
  - Result directory: `/tmp/eks_bench_20260530_035421/`
  - amd64 compat: `27/27 passed`
  - amd64 HA/perf: `11/11 passed`
  - arm64 compat: `27/27 passed`
  - arm64 HA/perf: `11/11 passed`
  - Agent Memory E2E: `2/2 passed`
  - Final status: `ALL EKS TESTS PASSED`
- Post-run cluster sleep check:
  - NodePool CPU limits: `0`
  - Bench-labeled nodes: none
  - ZeptoDB pods: none

## Follow-ups

- Consider adding a dedicated `--agent-only` mode to `run_eks_bench.sh` for
  faster EKS Agent Memory iteration when the core K8s harness is already green.
