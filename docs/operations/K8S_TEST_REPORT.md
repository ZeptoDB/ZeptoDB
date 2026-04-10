# ZeptoDB Kubernetes Compatibility & HA Test Report

Date: 2026-04-08
Cluster: EKS `zepto-k8s-compat` (us-east-1), K8s v1.32.12
Nodes: 3x t3.xlarge (4 vCPU, 16 GiB)
Image: nginx:1.27-alpine (stand-in for ZeptoDB)

---

## 1. Test Summary

| Suite | Tests | Passed | Failed |
|-------|-------|--------|--------|
| Compatibility (T01–T27) | 27 | 27 | 0 |
| HA (HA01–HA06) | 6 | 6 | 0 |
| Performance (PERF01–PERF05) | 5 | 5 | 0 |
| **Total** | **38** | **38** | **0** |

---

## 2. Compatibility Tests (T01–T27)

### Static Validation

| ID | Test | Result | Notes |
|----|------|--------|-------|
| T01 | Helm lint | PASS | No errors, 1 info (icon recommended) |
| T02 | Helm template (default values) | PASS | All resources render correctly |
| T03 | Helm template (cluster + karpenter) | PASS | RPC/heartbeat ports, NodePool/EC2NodeClass rendered |

### Deployment Basics

| ID | Test | Result | Notes |
|----|------|--------|-------|
| T04 | Pods reach Running state | PASS | 2/2 running |
| T05 | Pods pass readiness probe | PASS | 2/2 ready |
| T06 | Service has endpoints | PASS | 2 addresses |
| T07 | Headless service (clusterIP=None) | PASS | Pod discovery works |
| T08 | ConfigMap contains expected keys | PASS | port, worker_threads, analytics_mode, data_dir |
| T09 | PodDisruptionBudget exists | PASS | minAvailable=1 |

### Pod Spec Validation

| ID | Test | Result | Notes |
|----|------|--------|-------|
| T10 | Anti-affinity (hostname spread) | PASS | preferredDuringScheduling rule present |
| T11 | Standard K8s labels | PASS | name, instance, version |
| T12 | Environment variables | PASS | POD_NAME, POD_NAMESPACE, POD_IP, APEX_WORKER_THREADS |
| T13 | preStop lifecycle hook | PASS | Graceful shutdown enabled |
| T14 | RollingUpdate strategy | PASS | maxUnavailable=0 |
| T15 | ConfigMap checksum annotation | PASS | Config change triggers rollout |
| T25 | terminationGracePeriodSeconds | PASS | 15s |
| T26 | Resource requests/limits | PASS | Both set |

### Networking

| ID | Test | Result | Notes |
|----|------|--------|-------|
| T22 | Headless DNS resolution | PASS | Resolves to pod IPs |
| T23 | Pod-to-pod connectivity | PASS | Direct IP communication |
| T24 | ClusterIP service routing | PASS | Service routes to backend pods |

### Lifecycle Operations

| ID | Test | Result | Notes |
|----|------|--------|-------|
| T16 | Rolling update execution | PASS | 2 ready after image tag change |
| T17 | Pod delete auto-recovery | PASS | Deployment recreates pod |
| T18 | PDB blocks eviction | PASS | Second eviction rejected |
| T19 | Helm rollback | PASS | Previous revision restored |
| T20 | Scale up (2→3) | PASS | 3 ready |
| T21 | Scale down (3→2) | PASS | 2 running |
| T27 | No warning events | PASS | Clean |

---

## 3. HA Tests (HA01–HA06)

| ID | Test | Result | Details |
|----|------|--------|---------|
| HA01 | 3 pods on 3 nodes | PASS | Each pod on a separate node (anti-affinity working) |
| HA02 | Node drain + recovery | PASS | Pod migrated, service stayed available, recovery=1.1s |
| HA03 | PDB blocks concurrent drain | PASS | Second drain rejected by PDB |
| HA04 | Pod kill + service continuity | PASS | Service reachable during kill, recovery=9.3s |
| HA05 | Rolling update zero-downtime | PASS | 20 probes during rollout, 0 failures |
| HA06 | Scale 3→5→3 | PASS | Scale-up=1.3s, scale-down clean |

---

## 4. Performance Benchmarks

| Metric | Value | Unit | Notes |
|--------|-------|------|-------|
| Pod startup latency (avg) | 5.22 | sec | Schedule + pull + ready (3 samples, stdev=0.02s) |
| Rolling update duration (3 replicas) | 30.36 | sec | Full rollout with maxUnavailable=0 |
| Node drain recovery | 1.11 | sec | Time until 3 pods ready after drain |
| Pod kill recovery | 9.33 | sec | Time until replacement pod ready |
| Service failover time | 7.25 | sec | Time until service consistently routes around dead pod |
| Scale 3→5 time | 1.26 | sec | Time until 5 pods ready |
| Pod-to-pod RTT (avg) | 1721 | ms | Includes kubectl exec overhead (~1.5s) |
| Pod-to-pod RTT (min) | 1580 | ms | Lower bound with kubectl overhead |
| HTTP sequential throughput | 50.79 | req/s | 100 sequential requests pod-to-pod |

### Notes on Performance Numbers

- **Pod-to-pod RTT** includes `kubectl exec` overhead (~1.5s per call). Actual network latency is sub-millisecond within the same VPC. For accurate network benchmarks, use an in-pod tool like `wrk` or `hey`.
- **HTTP throughput** is limited by sequential `wget` calls. Real throughput with concurrent connections would be orders of magnitude higher.
- **Pod startup latency** of ~5s is typical for EKS with `IfNotPresent` pull policy (image already cached). First pull would add 5–15s.
- **Service failover** of ~7s aligns with readinessProbe configuration (periodSeconds=5, failureThreshold=2 = 10s max detection + endpoint removal).

---

## 5. Helm Chart Issues Found (Static Analysis)

### Issue 1: Deployment + Single PVC (Shared Storage)

**Severity:** Medium (production impact)

The chart uses a `Deployment` with a single `PersistentVolumeClaim`. With `ReadWriteOnce` access mode, only one node can mount the volume. When replicas > 1, pods on different nodes cannot mount the same PVC.

**Recommendation:** Use `StatefulSet` with `volumeClaimTemplates` for per-pod storage, or switch to `ReadWriteMany` (requires EFS or similar).

### Issue 2: HPA + spec.replicas Conflict

**Severity:** Low

When HPA is enabled, the Deployment still sets `spec.replicas` from `values.yaml`. Every `helm upgrade` resets the replica count to the Helm value, overriding HPA's scaling decisions.

**Recommendation:** Conditionally omit `spec.replicas` when `autoscaling.enabled=true`.

### Issue 3: Hugepages Not Overridable via Values

**Severity:** Low

`resources.requests.hugepages-2Mi` in default `values.yaml` cannot be removed via override values — Helm deep-merges maps. Test environments without hugepages must explicitly set `hugepages-2Mi: "0"`.

**Recommendation:** Move hugepages into a separate conditional block controlled by `performanceTuning.hugepages.enabled`.

---

## 6. Test Execution

### Prerequisites

```bash
# Tools
kubectl v1.26+
helm v3.x
eksctl v0.200+
python 3.13
```

### Run All Tests

```bash
# Create cluster (if needed)
eksctl create cluster -f tests/k8s/eks-compat-cluster.yaml

# Scale to 3 nodes for HA tests
eksctl scale nodegroup --cluster=zepto-k8s-compat --name=test-nodes --nodes=3 --region=us-east-1

# Compatibility tests (27 scenarios)
python3.13 tests/k8s/test_k8s_compat.py

# HA + Performance tests (11 scenarios)
python3.13 tests/k8s/test_k8s_ha_perf.py

# Cleanup
python3.13 tests/k8s/test_k8s_compat.py --cleanup
python3.13 tests/k8s/test_k8s_ha_perf.py --cleanup
eksctl delete cluster -f tests/k8s/eks-compat-cluster.yaml --disable-nodegroup-eviction
```

### Estimated Cost

| Resource | Cost/hr |
|----------|---------|
| EKS control plane | $0.10 |
| 3x t3.xlarge On-Demand | $0.50 |
| EBS gp3 (if PVC enabled) | ~$0.02 |
| **Total** | **~$0.62/hr** |

Test duration: ~10 min (both suites). Total cost per run: ~$0.10 + cluster creation time.

---

## 7. Test File Index

| File | Description |
|------|-------------|
| `tests/k8s/eks-compat-cluster.yaml` | EKS cluster config (3x t3.xlarge) |
| `tests/k8s/test-values.yaml` | Lightweight Helm values for testing |
| `tests/k8s/test_k8s_compat.py` | Compatibility test suite (27 tests) |
| `tests/k8s/test_k8s_ha_perf.py` | HA + Performance test suite (11 tests) |
| `tests/k8s/run_k8s_compat.sh` | One-shot script (create → test → delete) |
| `docs/operations/K8S_TEST_REPORT.md` | This document |
