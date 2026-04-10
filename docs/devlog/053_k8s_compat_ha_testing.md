# Devlog 053: Kubernetes Compatibility & HA Testing

Date: 2026-04-08

## Summary

Built and executed a comprehensive Kubernetes compatibility, high-availability, and performance test suite against a live EKS cluster (K8s v1.32, 3 nodes).

## What Was Done

1. **Created test infrastructure**
   - Lightweight EKS cluster config (`tests/k8s/eks-compat-cluster.yaml`) — 3x t3.xlarge, ~$0.62/hr
   - Test Helm values (`tests/k8s/test-values.yaml`) — nginx stand-in, no hugepages, minimal resources

2. **Compatibility test suite** (`tests/k8s/test_k8s_compat.py`) — 27 tests
   - Helm lint/template validation (default, cluster mode, karpenter)
   - Pod lifecycle: running, ready, probes, labels, env vars, preStop
   - Networking: DNS, pod-to-pod, service routing, headless service
   - Operations: rolling update, rollback, scale up/down, PDB eviction

3. **HA + Performance test suite** (`tests/k8s/test_k8s_ha_perf.py`) — 11 tests
   - HA: 3-pod/3-node spread, node drain recovery, concurrent drain PDB block, pod kill with service continuity, zero-downtime rolling update, scale 3→5→3
   - Perf: pod startup latency, rolling update duration, network RTT, HTTP throughput, service failover time

4. **Results: 38/38 PASS**

## Key Performance Numbers

| Metric | Value |
|--------|-------|
| Pod startup latency | 5.2s avg |
| Rolling update (3 replicas) | 30.4s |
| Node drain recovery | 1.1s |
| Pod kill recovery | 9.3s |
| Service failover | 7.3s |

## Helm Chart Issues Found

1. **Deployment + single PVC** — `ReadWriteOnce` PVC shared by 3 replicas doesn't work across nodes. Should be StatefulSet with volumeClaimTemplates.
2. **HPA + spec.replicas conflict** — `helm upgrade` resets HPA-managed replica count.
3. **Hugepages not cleanly overridable** — Deep merge prevents removal in test values.

## Files

| File | Description |
|------|-------------|
| `tests/k8s/eks-compat-cluster.yaml` | EKS cluster config |
| `tests/k8s/test-values.yaml` | Test Helm values |
| `tests/k8s/test_k8s_compat.py` | Compatibility tests (27) |
| `tests/k8s/test_k8s_ha_perf.py` | HA + Performance tests (11) |
| `tests/k8s/run_k8s_compat.sh` | One-shot automation script |
| `docs/operations/K8S_TEST_REPORT.md` | Full test report |
