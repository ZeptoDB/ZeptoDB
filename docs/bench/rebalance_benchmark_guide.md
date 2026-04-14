# ZeptoDB Live Rebalancing Benchmark Guide

Last updated: 2026-04-11

---

## Overview

Measures the impact of live partition rebalancing on ingestion throughput and query latency. Runs from a loadgen pod against a live EKS cluster via HTTP.

The test has 4 phases:
1. **Baseline** — steady-state throughput/latency (30s)
2. **Rebalance Under Load** — trigger add_node/remove_node, measure impact
3. **Post-Rebalance** — verify recovery to baseline (30s)
4. **Data Integrity** — verify zero data loss across all symbols

---

## Prerequisites

- EKS cluster running (`./tools/eks-bench.sh wake`)
- Helm chart deployed with rebalance config
- Loadgen pod running
- `bench_rebalance` binary built

---

## Step-by-Step Execution

### 1. Wake EKS cluster

```bash
./tools/eks-bench.sh wake
./tools/eks-bench.sh status   # wait for all nodes ACTIVE
```

### 2. Deploy ZeptoDB with rebalance enabled

```bash
helm install zepto ./deploy/helm/zeptodb \
  -n zeptodb --create-namespace \
  -f deploy/helm/bench-rebalance-values.yaml

kubectl rollout status statefulset/zepto-zeptodb -n zeptodb --timeout=5m
```

### 3. Deploy loadgen pod

```bash
kubectl apply -f deploy/k8s/bench-loadgen.yaml
kubectl wait --for=condition=Ready pod/bench-loadgen -n zeptodb --timeout=120s
```

### 4. Build and run

```bash
# Option A: automated script
./deploy/scripts/run_rebalance_bench.sh --action add_node --node-id 4

# Option B: manual
cd build && cmake .. -G Ninja && ninja -j$(nproc) bench_rebalance
kubectl cp build/bench_rebalance zeptodb/bench-loadgen:/tmp/bench_rebalance
kubectl exec -n zeptodb bench-loadgen -- /tmp/bench_rebalance \
  --host zepto-zeptodb.zeptodb.svc.cluster.local --port 8123 \
  --symbols 100 --ticks-per-sec 10000 --query-qps 10 \
  --baseline-sec 30 --action add_node --node-id 4
```

### 5. Sleep cluster when done

```bash
./tools/eks-bench.sh sleep
```

---

## Expected Results

| Metric | Baseline | During Rebalance | Post-Rebalance |
|--------|----------|-----------------|----------------|
| Ingest ticks/sec | ~10,000 | >9,000 (<10% drop) | ~10,000 |
| Query p50 | <2ms | <5ms | <2ms |
| Query p99 | <10ms | <20ms | <10ms |
| Insert failures | <0.1% | <1% | <0.1% |
| Query failures | 0 | <5% | 0 |
| Data loss | 0 | 0 | 0 |

Pass criteria:
- Ingest throughput drop < 50% during rebalance
- Zero data loss (all symbols have rows)
- Post-rebalance metrics return to baseline levels

---

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `--host` | `zepto-zeptodb.zeptodb.svc.cluster.local` | Cluster service hostname |
| `--port` | `8123` | HTTP port |
| `--symbols` | `100` | Number of symbols to ingest |
| `--ticks-per-sec` | `10000` | Target ingestion rate |
| `--query-qps` | `10` | Concurrent query rate |
| `--baseline-sec` | `30` | Duration for baseline/post phases |
| `--action` | `add_node` | Rebalance action: `add_node` or `remove_node` |
| `--node-id` | `0` (auto) | Node ID for rebalance (0 = auto-detect) |

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "Could not reach cluster" | Verify pods are running: `kubectl get pods -n zeptodb` |
| "Failed to trigger rebalance" | Check rebalance is enabled in config; check `/admin/rebalance/status` |
| Rebalance timeout | Increase timeout; check pod logs: `kubectl logs -n zeptodb <pod>` |
| High insert failure rate | Check pod resource limits; verify network: `kubectl exec bench-loadgen -- ping <pod-ip>` |
| Binary not found | Build with: `cd build && ninja bench_rebalance` |

---

## Cost Estimate

| Resource | Cost/hr | Duration | Total |
|----------|---------|----------|-------|
| 3x r7i.2xlarge (data) | $1.50 × 3 | ~1 hr | $4.50 |
| 1x c7i.xlarge (loadgen) | $0.17 | ~1 hr | $0.17 |
| EKS control plane | $0.10 | ~1 hr | $0.10 |
| **Total** | | | **~$5/run** |

Use `./tools/eks-bench.sh sleep` to stop paying for instances between runs (control plane only: $0.10/hr).

---

## Related

- [EKS Multi-Node Benchmark](eks_multinode_benchmark.md) — full 7-scenario benchmark suite
- [Live Rebalancing Design](../design/phase_c_distributed.md) — architecture
- [Devlog 055: Live Rebalancing](../devlog/055_live_rebalancing.md) — implementation record
