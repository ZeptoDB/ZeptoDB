# ZeptoDB Multi-Node Benchmark Guide

Last updated: 2026-03-24

---

## 1. Goals

Measure ZeptoDB's distributed performance across real network boundaries:

| # | What | Why |
|---|------|-----|
| B1 | Distributed ingestion throughput | How fast can N nodes absorb ticks? |
| B2 | Scatter-gather query latency | Coordinator overhead vs single-node |
| B3 | Distributed VWAP / GROUP BY | Partial aggregation + merge cost |
| B4 | ASOF JOIN cross-node | Worst-case: data on different nodes |
| B5 | Failover recovery time | Node death → query resumes on replica |
| B6 | WAL replication lag | Async replication throughput under load |
| B7 | Linear scalability | 1→2→4→8 nodes, same dataset |

---

## 2. Infrastructure

### 2.1 Instance Selection

| Role | Count | Instance | vCPU | RAM | Network | Storage |
|------|-------|----------|------|-----|---------|---------|
| **Data Node** | 3 (min 2) | `r7i.2xlarge` | 8 | 64 GB | 12.5 Gbps | 500 GB gp3 |
| **Coordinator** | 1 | `c7i.xlarge` | 4 | 8 GB | 12.5 Gbps | 50 GB gp3 |
| **Load Generator** | 1 | `c7i.2xlarge` | 8 | 16 GB | 12.5 Gbps | 50 GB gp3 |

Total: 5 instances. Estimated cost: ~$4.50/hr ($108/day).

**Why r7i for data nodes:**
- Intel Xeon (Sapphire Rapids) — AVX-512 for Highway SIMD
- 64 GB RAM — holds 1B+ ticks in-memory
- 12.5 Gbps network — enough for TCP RPC scatter-gather

**For RDMA/EFA testing (recommended: `m7a.4xlarge` Spot):**
- 4× `m7a.4xlarge` Spot — RDMA read+write (Nitro v4), ~$2.25/2h total
- Full guide: `docs/bench/rdma_efa_benchmark.md`

**For Graviton comparison (optional):**
- Add 3x `r8g.2xlarge` data nodes (ARM, Highway SVE)
- Run same benchmarks, compare x86 vs ARM

### 2.2 Network Topology

```
┌─────────────────────────────────────────────────────┐
│  VPC: 10.0.0.0/16                                   │
│  Placement Group: cluster (same AZ, low latency)    │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│  │ Data-0   │  │ Data-1   │  │ Data-2   │          │
│  │ 10.0.1.10│  │ 10.0.1.11│  │ 10.0.1.12│          │
│  │ :8123    │  │ :8123    │  │ :8123    │          │
│  │ :8223 RPC│  │ :8223 RPC│  │ :8223 RPC│          │
│  │ :9100 HB │  │ :9100 HB │  │ :9100 HB │          │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘          │
│       │              │              │                │
│       └──────────┬───┴──────────────┘                │
│                  │                                    │
│           ┌──────┴──────┐    ┌──────────────┐        │
│           │ Coordinator │    │ Load Gen     │        │
│           │ 10.0.1.20   │    │ 10.0.1.30    │        │
│           │ :8123       │    │ (bench client)│        │
│           └─────────────┘    └──────────────┘        │
└─────────────────────────────────────────────────────┘
```

### 2.3 Provisioning

```bash
# Terraform or CLI — all in same placement group
aws ec2 run-instances \
  --count 3 \
  --instance-type r7i.2xlarge \
  --placement "GroupName=zepto-bench,Strategy=cluster" \
  --image-id ami-0abcdef1234567890 \
  --subnet-id subnet-xxx \
  --security-group-ids sg-xxx \
  --key-name zepto-bench \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Role,Value=data-node}]'

# Coordinator + Load Gen: same placement group, smaller instances
```

Security group rules:
- TCP 8123 (HTTP API) — within VPC
- TCP 8223 (RPC) — within VPC
- UDP 9100 (heartbeat) — within VPC
- TCP 22 (SSH) — your IP only

---

## 3. Build & Deploy

```bash
# On each node:
git clone https://github.com/zeptodb/zeptodb.git
cd zeptodb

# Install deps (AL2023)
sudo dnf install -y clang19 clang19-devel llvm19-devel \
  highway-devel numactl-devel lz4-devel ninja-build

# Build
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DCMAKE_CXX_FLAGS="-march=native -O3"
ninja -j$(nproc)

# Tune (on data nodes)
sudo ../scripts/tune_bare_metal.sh
```

---

## 4. Cluster Startup

### 4.1 Data Nodes

```bash
# Node 0 (seed)
./zepto_server --port 8123 --node-id 0 \
  --cluster-seeds "" \
  --rpc-port 8223 --heartbeat-port 9100

# Node 1
./zepto_server --port 8123 --node-id 1 \
  --cluster-seeds "10.0.1.10:8223" \
  --rpc-port 8223 --heartbeat-port 9100

# Node 2
./zepto_server --port 8123 --node-id 2 \
  --cluster-seeds "10.0.1.10:8223,10.0.1.11:8223" \
  --rpc-port 8223 --heartbeat-port 9100
```

### 4.2 Coordinator

```bash
./zepto_server --port 8123 --mode coordinator \
  --data-nodes "10.0.1.10:8223,10.0.1.11:8223,10.0.1.12:8223"
```

### 4.3 Verify

```bash
# From load gen
curl http://10.0.1.20:8123/health
curl http://10.0.1.20:8123/admin/nodes
curl http://10.0.1.20:8123/admin/cluster

# Test query
curl -X POST http://10.0.1.20:8123/ -d 'SELECT 1'
```

---

## 5. Benchmark Scenarios

### B1: Distributed Ingestion Throughput

**Goal:** Measure total cluster ingestion rate (ticks/sec) as nodes scale.

```bash
# Load gen script: ingest N ticks across M symbols
# Symbols are hash-routed to different nodes automatically

for NODES in 1 2 3; do
  echo "=== $NODES nodes ==="
  ./bench_distributed_ingest \
    --coordinator 10.0.1.20:8123 \
    --symbols 100 \
    --ticks-per-symbol 1000000 \
    --batch-size 512 \
    --threads 4
done
```

**Expected metrics:**
- Ticks/sec per node
- Total cluster ticks/sec
- Linear scalability factor (ideal: Nx for N nodes)
- Ingestion latency p50/p99/p999

**Baseline:** Single-node = 5.52M ticks/sec. Target: >10M with 2 nodes, >15M with 3.

### B2: Scatter-Gather Query Latency

**Goal:** Measure coordinator overhead for fan-out queries.

```bash
# Pre-load: 10M ticks across 100 symbols (distributed across nodes)

# Single-symbol query (Tier A: direct routing, no scatter)
curl -w "\ntime_total: %{time_total}s\n" \
  -X POST http://10.0.1.20:8123/ \
  -d 'SELECT count(*), avg(price) FROM trades WHERE symbol = 42'

# All-symbol query (Tier B: scatter-gather)
curl -w "\ntime_total: %{time_total}s\n" \
  -X POST http://10.0.1.20:8123/ \
  -d 'SELECT symbol, count(*), vwap(price, volume) FROM trades GROUP BY symbol'
```

**Measure:**
- Tier A (single-node) latency vs direct single-node query
- Tier B (scatter-gather) latency breakdown:
  - SQL parse time
  - Fan-out time (network)
  - Remote execution time (per node)
  - Merge time (coordinator)
  - Total end-to-end

**Expected overhead:** Tier A: <100μs routing overhead. Tier B: <2ms for 3-node scatter-gather.

### B3: Distributed Aggregation

**Goal:** Verify partial aggregation correctness and performance.

```sql
-- VWAP (decomposed: SUM(price*volume), SUM(volume) per node → merge)
SELECT vwap(price, volume) FROM trades WHERE symbol = 1;

-- GROUP BY with multiple aggregates
SELECT symbol, count(*), sum(volume), avg(price), min(price), max(price)
FROM trades GROUP BY symbol ORDER BY symbol;

-- COUNT(DISTINCT) (fetch-and-compute at coordinator)
SELECT COUNT(DISTINCT symbol) FROM trades;

-- HAVING (strip at scatter, apply post-merge)
SELECT symbol, sum(volume) AS vol FROM trades
GROUP BY symbol HAVING vol > 1000000;
```

**Verify:** Results must match single-node execution exactly (no precision loss).

### B4: Distributed ASOF JOIN

**Goal:** Worst-case cross-node join performance.

```sql
-- Trades on node 0, quotes on node 1 (different symbols route differently)
SELECT t.price, q.bid, q.ask
FROM trades t ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
WHERE t.symbol = 1;
```

**Measure:** Latency vs single-node ASOF JOIN. Expected: 2-5x slower due to data transfer.

### B5: Failover Recovery

**Goal:** Measure time from node failure to query resumption.

```bash
# 1. Continuous query loop (1 query/sec)
while true; do
  curl -s -o /dev/null -w "%{http_code} %{time_total}s\n" \
    -X POST http://10.0.1.20:8123/ \
    -d 'SELECT count(*) FROM trades WHERE symbol = 1'
  sleep 1
done

# 2. Kill data node 2
ssh 10.0.1.12 'kill -9 $(pgrep zepto_server)'

# 3. Observe:
#    - How many queries fail?
#    - Time until queries succeed again (via replica)?
#    - HealthMonitor detection time (suspect_timeout=3s, dead_timeout=10s)
```

**Target:** <15s total recovery (10s dead detection + 5s re-routing).

---

## Related

- **EKS-based benchmark (recommended for cloud):** `docs/bench/eks_multinode_benchmark.md`
  - Uses existing Helm chart, ~$12 total cost, `eksctl delete` cleanup
- **Bare-metal guide above** is for dedicated hardware with RDMA/EFA

---

### B6: WAL Replication Lag

**Goal:** Measure async replication throughput under sustained ingestion.

```bash
# Ingest at max rate on node 0
# Monitor replication lag on node 1 (replica)

# On node 0: ingest 10M ticks
./bench_distributed_ingest --target 10.0.1.10:8123 --ticks 10000000

# On node 1: check lag
curl http://10.0.1.11:8123/stats | jq '.replication_lag_ticks'
```

**Target:** Replication lag < 10,000 ticks at 5M ticks/sec sustained.

### B7: Linear Scalability

**Goal:** Same dataset, increasing node count. Measure query latency.

```
Dataset: 30M ticks, 300 symbols

Config A: 1 node  (all 30M ticks)
Config B: 2 nodes (15M each)
Config C: 3 nodes (10M each)

Query: SELECT symbol, vwap(price, volume) FROM trades GROUP BY symbol
```

**Expected:**
| Nodes | Query Latency | Speedup |
|-------|--------------|---------|
| 1 | baseline | 1.0x |
| 2 | ~55% of baseline | 1.8x |
| 3 | ~40% of baseline | 2.5x |

Sub-linear due to scatter-gather overhead + merge cost.

---

## 6. Benchmark Script

```bash
#!/bin/bash
# scripts/bench_multinode.sh — run from load gen instance
set -euo pipefail

COORD="10.0.1.20:8123"
RESULTS_DIR="bench_results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

echo "=== ZeptoDB Multi-Node Benchmark ==="
echo "Coordinator: $COORD"
echo "Results: $RESULTS_DIR"

# Health check
echo "[1/7] Health check..."
for node in 10.0.1.{10,11,12,20}; do
  status=$(curl -s -o /dev/null -w "%{http_code}" "http://$node:8123/health")
  echo "  $node: $status"
done

# Cluster info
echo "[2/7] Cluster info..."
curl -s "http://$COORD:8123/admin/cluster" | tee "$RESULTS_DIR/cluster.json"
echo

# B1: Ingestion
echo "[3/7] Ingestion benchmark..."
./build/bench_distributed_ingest \
  --coordinator "$COORD" \
  --symbols 100 --ticks 10000000 --batch 512 \
  2>&1 | tee "$RESULTS_DIR/b1_ingestion.txt"

# B2: Query latency
echo "[4/7] Query latency..."
for i in $(seq 1 100); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST "http://$COORD:8123/" \
    -d 'SELECT count(*), vwap(price, volume) FROM trades WHERE symbol = 1'
done | tee "$RESULTS_DIR/b2_tier_a.txt"

for i in $(seq 1 100); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST "http://$COORD:8123/" \
    -d 'SELECT symbol, count(*), vwap(price, volume) FROM trades GROUP BY symbol'
done | tee "$RESULTS_DIR/b2_tier_b.txt"

# B3: Distributed aggregation correctness
echo "[5/7] Aggregation correctness..."
curl -s -X POST "http://$COORD:8123/" \
  -d 'SELECT symbol, count(*), sum(volume), avg(price), vwap(price, volume) FROM trades GROUP BY symbol ORDER BY symbol' \
  | tee "$RESULTS_DIR/b3_agg.json"

# B7: Scalability (requires manual node add/remove)
echo "[6/7] Scalability test — manual step"
echo "  Run queries with 1, 2, 3 nodes and record results"

echo "[7/7] Done. Results in $RESULTS_DIR"
```

---

## 7. Result Recording

Save all results to `docs/bench/results_multinode.md` with:

```markdown
# Multi-Node Benchmark Results
# Date: YYYY-MM-DD
# Environment: 3x r7i.2xlarge + 1x c7i.xlarge coordinator
# Network: 12.5 Gbps, same AZ placement group
# ZeptoDB version: X.Y.Z

## B1: Ingestion
| Nodes | Total ticks/sec | Per-node | Scalability |
|-------|----------------|----------|-------------|
| 1     | X M/s          | X M/s    | 1.0x        |
| 2     | X M/s          | X M/s    | X.Xx        |
| 3     | X M/s          | X M/s    | X.Xx        |

## B2: Query Latency
...
```

---

## 8. Cost Optimization

- Use **Spot Instances** for load gen and coordinator (70% savings)
- Use **On-Demand** for data nodes (avoid mid-benchmark termination)
- Total benchmark run: ~2 hours → **~$9 total cost**
- Terminate all instances immediately after benchmark
- Save AMI with pre-built ZeptoDB for re-runs

```bash
# Cleanup
aws ec2 terminate-instances --instance-ids i-xxx i-yyy i-zzz
```

---

## 9. Checklist

### Pre-benchmark
- [ ] All instances in same placement group (same AZ)
- [ ] Security groups allow TCP 8123/8223, UDP 9100 within VPC
- [ ] ZeptoDB built with `-O3 -march=native` on each node
- [ ] `tune_bare_metal.sh` run on data nodes
- [ ] Cluster formed: `/admin/nodes` shows all nodes ACTIVE
- [ ] Single-node baseline recorded first

### During benchmark
- [ ] Monitor CPU/memory/network on all nodes (`htop`, `sar`, `iftop`)
- [ ] Check for errors in zepto_server logs
- [ ] Verify aggregation correctness (B3) matches single-node results
- [ ] Record network latency between nodes: `ping -c 100 10.0.1.11`

### Post-benchmark
- [ ] Save all results to `docs/bench/results_multinode.md`
- [ ] Update README.md performance table with distributed numbers
- [ ] Update BACKLOG.md
- [ ] Terminate instances
- [ ] Commit results
