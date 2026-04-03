# ZeptoDB Multi-Node Benchmark on EKS

Last updated: 2026-03-24

---

## Overview

EKS-based multi-node benchmark. Uses the existing Helm chart (`helm/zeptodb/`) to deploy a 3-node cluster and measure 7 distributed performance scenarios.

### Why EKS

- Helm chart already exists → cluster setup with a single `helm install` command
- Placement Group + Pod Anti-Affinity → guarantees node distribution
- PDB prevents pod eviction during benchmarks
- Clean teardown with `eksctl delete cluster` when finished
- Cost: ~$6/hr (2-hour bench = ~$12)

---

## 1. EKS Cluster Provisioning

### 1.1 eksctl config

```yaml
# eks-bench-cluster.yaml
apiVersion: eksctl.io/v1alpha5
kind: ClusterConfig

metadata:
  name: zepto-bench
  region: ap-northeast-2   # or your preferred region
  version: "1.29"

managedNodeGroups:
  # Data nodes: 3x r7i.2xlarge (AVX-512, 64GB RAM)
  - name: data-nodes
    instanceType: r7i.2xlarge
    desiredCapacity: 3
    minSize: 3
    maxSize: 3
    volumeSize: 100
    volumeType: gp3
    labels:
      role: data-node
    tags:
      purpose: zepto-bench
    # Same AZ for minimal network latency
    availabilityZones: ["ap-northeast-2a"]

  # Load generator: 1x c7i.xlarge
  - name: loadgen
    instanceType: c7i.xlarge
    desiredCapacity: 1
    minSize: 1
    maxSize: 1
    volumeSize: 50
    labels:
      role: loadgen
    availabilityZones: ["ap-northeast-2a"]
```

```bash
# Create cluster (~15 min)
eksctl create cluster -f eks-bench-cluster.yaml

# Verify
kubectl get nodes -L role
```

### 1.2 ECR Image Push

```bash
# Create ECR repo
ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
REGION=ap-northeast-2
REPO=$ACCOUNT.dkr.ecr.$REGION.amazonaws.com/zeptodb

aws ecr create-repository --repository-name zeptodb --region $REGION

# Build & push
aws ecr get-login-password --region $REGION | docker login --username AWS --password-stdin $REPO
docker build -t zeptodb:bench .
docker tag zeptodb:bench $REPO:bench
docker push $REPO:bench
```

---

## 2. Deploy ZeptoDB Cluster

### 2.1 Benchmark values override

```yaml
# deploy/helm/bench-values.yaml
replicaCount: 3

image:
  repository: "${ACCOUNT}.dkr.ecr.${REGION}.amazonaws.com/zeptodb"
  tag: "bench"

resources:
  requests:
    cpu: "6"
    memory: "48Gi"
  limits:
    cpu: "8"
    memory: "60Gi"

cluster:
  enabled: true
  rpcPortOffset: 100
  heartbeatPort: 9100

persistence:
  enabled: true
  storageClass: gp3
  size: 100Gi

autoscaling:
  enabled: false    # fixed 3 replicas for benchmark

podAntiAffinity:
  enabled: true
  weight: 100       # spread across nodes

nodeSelector:
  role: data-node

config:
  workerThreads: 6
  parallelThreshold: 50000
```

### 2.2 Install

```bash
# Deploy data nodes
helm install zepto ./deploy/helm/zeptodb \
  -n zeptodb --create-namespace \
  -f deploy/helm/bench-values.yaml

# Wait for all pods ready
kubectl rollout status deployment/zepto-zeptodb -n zeptodb --timeout=5m

# Verify cluster
kubectl get pods -n zeptodb -o wide
kubectl exec -n zeptodb deploy/zepto-zeptodb -- \
  curl -s localhost:8123/admin/cluster
```

### 2.3 Deploy Load Generator

```yaml
# deploy/k8s/bench-loadgen.yaml
apiVersion: v1
kind: Pod
metadata:
  name: bench-loadgen
  namespace: zeptodb
spec:
  nodeSelector:
    role: loadgen
  containers:
    - name: loadgen
      image: "${ACCOUNT}.dkr.ecr.${REGION}.amazonaws.com/zeptodb:bench"
      command: ["sleep", "infinity"]
      resources:
        requests:
          cpu: "3"
          memory: "8Gi"
  restartPolicy: Never
```

```bash
kubectl apply -f deploy/k8s/bench-loadgen.yaml
kubectl wait --for=condition=Ready pod/bench-loadgen -n zeptodb --timeout=120s
```

---

## 3. Benchmark Scenarios

### Service discovery

```bash
# Inside loadgen pod, data nodes are reachable via headless service:
#   zepto-zeptodb-headless.zeptodb.svc.cluster.local:8123
# Or via LoadBalancer:
#   zepto-zeptodb.zeptodb.svc.cluster.local:8123

SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"
HEADLESS="zepto-zeptodb-headless.zeptodb.svc.cluster.local:8123"
```

### B1: Distributed Ingestion Throughput

```bash
kubectl exec -n zeptodb bench-loadgen -- bash -c '
SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"
START=$(date +%s%N)
for i in $(seq 1 100000); do
  curl -s -o /dev/null -X POST "http://$SVC/" \
    -d "INSERT INTO trades VALUES ($((i % 100)), $((1000000000 + i)), $((15000 + i % 1000)), $((100 + i % 50)), 0)"
done
END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))
echo "100K inserts in ${ELAPSED}ms → $(( 100000 * 1000 / ELAPSED )) inserts/sec"
'
```

For actual benchmarks, use the C++ `bench_distributed_ingest` binary:

```bash
kubectl exec -n zeptodb bench-loadgen -- \
  ./bench_distributed_ingest \
    --host "$SVC" \
    --symbols 100 \
    --ticks-per-symbol 100000 \
    --batch-size 512
```

**Metrics to measure:**
| Metric | Target |
|--------|--------|
| Total ticks/sec (3 nodes) | >12M |
| Per-node ticks/sec | >4M |
| Ingestion p99 latency | <500μs |

### B2: Scatter-Gather Query Latency

```bash
kubectl exec -n zeptodb bench-loadgen -- bash -c '
SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"

echo "=== Tier A: Single-symbol (direct routing) ==="
for i in $(seq 1 200); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST "http://$SVC/" \
    -d "SELECT count(*), vwap(price, volume) FROM trades WHERE symbol = 1"
done | awk "{sum+=\$1; n++} END {printf \"avg=%.3fms p99=%.3fms (n=%d)\n\", sum/n*1000, 0, n}"

echo "=== Tier B: All-symbol (scatter-gather) ==="
for i in $(seq 1 200); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST "http://$SVC/" \
    -d "SELECT symbol, count(*), vwap(price, volume) FROM trades GROUP BY symbol"
done | awk "{sum+=\$1; n++} END {printf \"avg=%.3fms (n=%d)\n\", sum/n*1000, n}"
'
```

**Metrics to measure:**
| Query Type | Target |
|-----------|--------|
| Tier A (single-node routing) | <1ms overhead vs direct |
| Tier B (3-node scatter-gather) | <5ms total |
| Tier B breakdown: fan-out | <1ms |
| Tier B breakdown: merge | <1ms |

### B3: Distributed Aggregation Correctness

```bash
kubectl exec -n zeptodb bench-loadgen -- bash -c '
SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"

echo "--- VWAP ---"
curl -s -X POST "http://$SVC/" \
  -d "SELECT vwap(price, volume) FROM trades WHERE symbol = 1"

echo "--- GROUP BY multi-agg ---"
curl -s -X POST "http://$SVC/" \
  -d "SELECT symbol, count(*), sum(volume), avg(price), vwap(price, volume) FROM trades GROUP BY symbol ORDER BY symbol"

echo "--- COUNT(DISTINCT) ---"
curl -s -X POST "http://$SVC/" \
  -d "SELECT COUNT(DISTINCT symbol) FROM trades"

echo "--- HAVING ---"
curl -s -X POST "http://$SVC/" \
  -d "SELECT symbol, sum(volume) AS vol FROM trades GROUP BY symbol HAVING vol > 100000"
'
```

**Verification:** Compare with single-node results. Numbers must match exactly.

### B4: Distributed ASOF JOIN

```bash
kubectl exec -n zeptodb bench-loadgen -- bash -c '
SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"

for i in $(seq 1 100); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST "http://$SVC/" \
    -d "SELECT t.price, q.bid, q.ask FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp WHERE t.symbol = 1"
done | awk "{sum+=\$1; n++} END {printf \"ASOF JOIN avg=%.3fms (n=%d)\n\", sum/n*1000, n}"
'
```

### B5: Failover Recovery

```bash
# Terminal 1: continuous query loop
kubectl exec -n zeptodb bench-loadgen -- bash -c '
SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"
while true; do
  CODE=$(curl -s -o /dev/null -w "%{http_code}" -m 2 \
    -X POST "http://$SVC/" \
    -d "SELECT count(*) FROM trades WHERE symbol = 1")
  echo "$(date +%H:%M:%S) $CODE"
  sleep 1
done
'

# Terminal 2: kill one pod
kubectl delete pod -n zeptodb $(kubectl get pods -n zeptodb -o name | head -1) --grace-period=0 --force

# Observe: how many 5xx/timeout before recovery?
# Target: <15s gap (HealthMonitor dead_timeout=10s + pod restart)
```

### B6: WAL Replication Lag

```bash
# Sustained ingestion + check replication stats
kubectl exec -n zeptodb bench-loadgen -- bash -c '
SVC="zepto-zeptodb.zeptodb.svc.cluster.local:8123"

# Ingest for 30 seconds
timeout 30 ./bench_distributed_ingest --host "$SVC" --symbols 50 --ticks-per-symbol 1000000

# Check each node stats
for pod in $(kubectl get pods -n zeptodb -l app.kubernetes.io/name=zeptodb -o name); do
  echo "--- $pod ---"
  kubectl exec -n zeptodb $pod -- curl -s localhost:8123/stats
  echo
done
'
```

### B7: Linear Scalability

```bash
# Scale down to 1, run query, scale to 2, run query, scale to 3
for REPLICAS in 1 2 3; do
  echo "=== $REPLICAS replicas ==="
  kubectl scale deployment zepto-zeptodb -n zeptodb --replicas=$REPLICAS
  kubectl rollout status deployment/zepto-zeptodb -n zeptodb --timeout=3m
  sleep 10  # stabilize

  # Re-ingest (data redistributes)
  kubectl exec -n zeptodb bench-loadgen -- \
    ./bench_distributed_ingest --host "$SVC" --symbols 100 --ticks-per-symbol 100000

  # Query benchmark
  kubectl exec -n zeptodb bench-loadgen -- bash -c '
    for i in $(seq 1 100); do
      curl -s -o /dev/null -w "%{time_total}\n" \
        -X POST "http://zepto-zeptodb.zeptodb.svc.cluster.local:8123/" \
        -d "SELECT symbol, vwap(price, volume) FROM trades GROUP BY symbol"
    done | awk "{sum+=\$1; n++} END {printf \"avg=%.3fms\n\", sum/n*1000}"
  '
done
```

---

## 4. Result Template

After the benchmark is complete, record results in `docs/bench/results_multinode.md`:

```markdown
# Multi-Node Benchmark Results (EKS)
# Date: YYYY-MM-DD
# Cluster: EKS 1.29, 3x r7i.2xlarge (data), 1x c7i.xlarge (loadgen)
# Region: ap-northeast-2, single AZ placement
# ZeptoDB: vX.Y.Z, Clang 19, -O3 -march=native

## B1: Ingestion
| Nodes | Total ticks/sec | Per-node | Scale factor |
|-------|----------------|----------|--------------|
| 1     |                |          | 1.0x         |
| 2     |                |          |              |
| 3     |                |          |              |

## B2: Query Latency
| Query Type | 1 node | 3 nodes | Overhead |
|-----------|--------|---------|----------|
| Tier A    |        |         |          |
| Tier B    |        |         |          |

## B3: Aggregation Correctness
| Function | Single-node | 3-node | Match? |
|----------|------------|--------|--------|
| VWAP     |            |        |        |
| COUNT    |            |        |        |
| AVG      |            |        |        |

## B5: Failover
- Detection time:
- Recovery time:
- Queries failed:

## B7: Scalability
| Nodes | GROUP BY latency | Speedup |
|-------|-----------------|---------|
| 1     |                 | 1.0x    |
| 2     |                 |         |
| 3     |                 |         |
```

---

## 5. Cleanup

```bash
# Remove ZeptoDB
helm uninstall zepto -n zeptodb

# Delete EKS cluster (removes all nodes + VPC)
eksctl delete cluster --name zepto-bench --region ap-northeast-2

# Delete ECR image (optional)
aws ecr delete-repository --repository-name zeptodb --force --region ap-northeast-2
```

Total cost estimate: **~$12** (4 nodes × $1.50/hr × 2 hours).

### Related Guides
- **RDMA / EFA benchmark (bare-metal, lowest latency):** `docs/bench/rdma_efa_benchmark.md`
- **Bare-metal TCP benchmark:** `docs/bench/multinode_benchmark_guide.md`

---

## 6. Checklist

### Pre-benchmark
- [ ] EKS cluster created, all nodes Ready
- [ ] Docker image pushed to ECR
- [ ] `helm install` successful, 3 pods Running
- [ ] `/admin/cluster` shows all nodes
- [ ] Load generator pod running
- [ ] Single-node baseline recorded (scale to 1 first)

### During benchmark
- [ ] `kubectl top pods -n zeptodb` — monitor CPU/memory
- [ ] `kubectl logs -f -n zeptodb <pod>` — watch for errors
- [ ] B3 correctness verified (numbers match single-node)
- [ ] Network latency checked: `kubectl exec bench-loadgen -- ping <pod-ip>`

### Post-benchmark
- [ ] Results saved to `docs/bench/results_multinode.md`
- [ ] README.md updated with distributed numbers
- [ ] BACKLOG.md updated
- [ ] `eksctl delete cluster` executed
- [ ] ECR cleaned up
