# Devlog 061 — EKS amd64 vs arm64 Benchmark

Date: 2026-04-14

## What

Ran full K8s compatibility (27 tests) and HA+performance (11 tests) suites on both amd64 and arm64 (Graviton) EKS nodes. 76/76 PASS.

## Bugs Fixed (7)

- `get_pods()` / `wait_for_rollout()` default arguments evaluated at module load time — breaks monkey-patched RELEASE for multi-arch runs
- HA03 PDB test used `kubectl drain` which bypasses PDB when pods reschedule between sequential drains — switched to Eviction API
- PERF03 network latency failed after PERF02 rollback — added warmup request
- `setup()` in both test files didn't wait for pods on Karpenter-provisioned nodes

## Key Numbers

| Metric | amd64 | arm64 |
|--------|------:|------:|
| Pod startup | 4.92s | 6.04s |
| Rolling update (3r) | 25.13s | 25.38s |
| HTTP throughput | 155 req/s | 141 req/s |
| Service failover | 3.57s | 3.45s |

No meaningful operational difference. arm64 is production-ready with ~20% EC2 cost savings.

## Files

- Modified: `tests/k8s/test_k8s_compat.py`, `tests/k8s/test_k8s_ha_perf.py`
- Created: `tests/k8s/test-values-arm64.yaml`, `docs/bench/eks_arch_benchmark_report.md`
