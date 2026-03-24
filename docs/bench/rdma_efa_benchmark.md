# ZeptoDB RDMA / AWS EFA Benchmark Guide

Last updated: 2026-03-24

---

## Overview

ZeptoDB의 UCX transport를 AWS EFA(Elastic Fabric Adapter) 위에서 실행하여 RDMA one-sided read/write 성능을 측정한다. TCP RPC 대비 레이턴시/처리량 차이를 정량화하고, HFT 워크로드에서의 실제 이점을 검증한다.

### TCP vs RDMA — 왜 측정해야 하는가

```
TCP path:   app → syscall → kernel TCP stack → NIC → wire → NIC → kernel → syscall → app
RDMA path:  app → NIC (kernel bypass) → wire → NIC → remote memory (no remote CPU)
```

| | TCP RPC | UCX/EFA RDMA |
|---|---|---|
| Latency | 50–100μs | 1–15μs |
| CPU overhead | High (syscall, copy) | Near-zero (kernel bypass) |
| Remote CPU | Required (recv + process) | Not required (one-sided) |
| Use case | General queries | Tick ingestion hot path |

---

## 1. Infrastructure

### 1.1 Instance Selection

EFA + RDMA read/write를 모두 지원하는 가장 작고 저렴한 인스턴스를 선택한다.

**Recommended: `m7a.4xlarge` (Spot)**

| Role | Instance | vCPU | RAM | Network | On-Demand | Spot (~70% off) |
|------|----------|------|-----|---------|-----------|-----------------|
| Data Node (×3) | `m7a.4xlarge` | 16 | 64 GB | 6.25 Gbps | $0.927/hr | ~$0.28/hr |
| Load Gen (×1) | `m7a.4xlarge` | 16 | 64 GB | 6.25 Gbps | $0.927/hr | ~$0.28/hr |

**Total cost: ~$1.12/hr (Spot) → 2시간 벤치 = ~$2.25**

**Why `m7a.4xlarge`:**
- Nitro v4 → RDMA read + write 모두 지원 (EFA 지원 인스턴스 중 최저가)
- AMD EPYC (Zen 4) — AVX-512 for Highway SIMD
- 16 vCPU, 64 GB RAM — ZeptoDB 3-node 클러스터에 충분
- m7a는 범용 패밀리라 Spot 재고 풍부, 중단 확률 낮음
- Coordinator는 별도 인스턴스 없이 Data Node 중 하나가 겸임

**Alternative options:**

| Option | Instance | RDMA R/W | Network | Spot/hr (×4) | 총 비용 (2h) | 비고 |
|--------|----------|----------|---------|-------------|-------------|------|
| **A (추천)** | `m7a.4xlarge` | ✅ R+W | 6.25 Gbps | ~$1.12 | **~$2.25** | 최저가 RDMA R/W |
| B | `hpc7g.4xlarge` | Read만 | 200 Gbps | $6.72 (OD) | ~$13.44 | 고대역폭, ARM, Spot 불가 |
| C | `c5n.9xlarge` | ❌ SRD만 | 50 Gbps | ~$2.33 | ~$4.66 | RDMA 아님, EFA SRD만 |
| D | `m6i.32xlarge` | ✅ R+W | 50 Gbps | ~$5.90 | ~$11.80 | 고대역폭 필요 시 |

> **Note:** `c5n` 시리즈는 Nitro v3이라 EFA는 지원하지만 RDMA read/write는 불가.
> 진짜 one-sided RDMA (ucp_put/ucp_get)를 테스트하려면 Nitro v4+ 필수.

### 1.2 Network Topology

```
┌──────────────────────────────────────────────────────┐
│  VPC: 10.0.0.0/16                                     │
│  Placement Group: cluster (MANDATORY for EFA)         │
│  Single AZ: us-east-1a                                │
│                                                        │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐     │
│  │ Data-0      │ │ Data-1      │ │ Data-2      │     │
│  │ m7a.4xlarge │ │ m7a.4xlarge │ │ m7a.4xlarge │     │
│  │ (Spot)      │ │ (Spot)      │ │ (Spot)      │     │
│  │ eth0: TCP   │ │ eth0: TCP   │ │ eth0: TCP   │     │
│  │ efa0: RDMA  │ │ efa0: RDMA  │ │ efa0: RDMA  │     │
│  │ :8123 HTTP  │ │ :8123 HTTP  │ │ :8123 HTTP  │     │
│  │ :8223 RPC   │ │ :8223 RPC   │ │ :8223 RPC   │     │
│  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘     │
│         │  EFA fabric    │               │             │
│         └────────────────┴───────────────┘             │
│                          │                             │
│                   ┌──────┴──────┐                      │
│                   │ Load Gen    │                      │
│                   │ m7a.4xlarge │                      │
│                   │ (Spot)      │                      │
│                   └─────────────┘                      │
└──────────────────────────────────────────────────────┘
```

### 1.3 Provisioning

```bash
# 1. Create placement group
aws ec2 create-placement-group \
  --group-name zepto-efa-bench \
  --strategy cluster \
  --region us-east-1

# 2. Launch 4x m7a.4xlarge Spot with EFA
aws ec2 run-instances \
  --count 4 \
  --instance-type m7a.4xlarge \
  --instance-market-options '{"MarketType":"spot","SpotOptions":{"SpotInstanceType":"one-time"}}' \
  --placement "GroupName=zepto-efa-bench" \
  --network-interfaces '[{
    "DeviceIndex": 0,
    "SubnetId": "subnet-xxx",
    "Groups": ["sg-xxx"],
    "InterfaceType": "efa",
    "AssociatePublicIpAddress": true
  }]' \
  --image-id ami-0abcdef1234567890 \
  --key-name zepto-bench \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=zepto-efa-bench}]' \
  --region us-east-1
```

Security group (sg-xxx):
- All traffic within security group (EFA requires this)
- TCP 22 from your IP

---

## 2. Environment Setup

### 2.1 EFA Driver + Libfabric

```bash
# On ALL nodes (AL2023)
# Install EFA software
curl -O https://efa-installer.amazonaws.com/aws-efa-installer-latest.tar.gz
tar xf aws-efa-installer-latest.tar.gz
cd aws-efa-installer
sudo ./efa_installer.sh -y

# Verify EFA
fi_info -p efa
# Should show: provider: efa, fabric: EFA-xxx

# Verify RDMA
ibv_devinfo
# Should show: hca_id: rdmap0s6, transport: InfiniBand (EFA)
```

### 2.2 UCX Installation

```bash
# UCX with EFA/libfabric support
sudo dnf install -y ucx ucx-devel

# Verify UCX sees EFA
ucx_info -d | grep efa
# Should show: Transport: dc_mlx5 or ud_verbs (EFA mapped)

# Check available transports
ucx_info -t
```

### 2.3 Build ZeptoDB with UCX

```bash
git clone https://github.com/zeptodb/zeptodb.git
cd zeptodb

sudo dnf install -y clang19 clang19-devel llvm19-devel \
  highway-devel numactl-devel lz4-devel ninja-build ucx-devel

mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DCMAKE_CXX_FLAGS="-march=native -O3" \
  -DZEPTO_USE_UCX=ON

ninja -j$(nproc)

# Verify UCX is linked
ldd zepto_server | grep ucx
# Should show: libucp.so, libucs.so, libuct.so
```

### 2.4 System Tuning

```bash
# On data nodes
sudo ../scripts/tune_bare_metal.sh

# Additional EFA tuning
# Increase locked memory limit (required for RDMA memory registration)
echo "* soft memlock unlimited" | sudo tee -a /etc/security/limits.conf
echo "* hard memlock unlimited" | sudo tee -a /etc/security/limits.conf

# Hugepages (for arena allocator)
echo 8192 | sudo tee /proc/sys/vm/nr_hugepages

# Disable irqbalance (pin NIC interrupts)
sudo systemctl stop irqbalance

# Set EFA interrupt affinity to non-data cores
# (data cores 0-3 for ZeptoDB, NIC interrupts on cores 4+)
```

---

## 3. Cluster Startup

### 3.1 UCX Transport Mode

```bash
# Data Node 0 (seed) — UCX transport
./zepto_server --port 8123 --node-id 0 \
  --transport ucx \
  --rpc-port 8223 \
  --cluster-seeds ""

# Data Node 1
./zepto_server --port 8123 --node-id 1 \
  --transport ucx \
  --rpc-port 8223 \
  --cluster-seeds "10.0.1.10:8223"

# Data Node 2
./zepto_server --port 8123 --node-id 2 \
  --transport ucx \
  --rpc-port 8223 \
  --cluster-seeds "10.0.1.10:8223,10.0.1.11:8223"
```

### 3.2 UCX Environment Variables

```bash
# Force EFA transport (skip TCP fallback)
export UCX_TLS=dc,ud,self
export UCX_NET_DEVICES=efa0

# Tune for low latency
export UCX_RC_TX_QUEUE_LEN=4096
export UCX_RC_RX_QUEUE_LEN=4096
export UCX_MM_SEG_SIZE=8388608    # 8MB segments

# Debug: show which transport UCX selected
export UCX_LOG_LEVEL=info
```

### 3.3 Verify RDMA Path

```bash
# On any node, check UCX is using EFA
UCX_LOG_LEVEL=info ./zepto_server --port 8123 --transport ucx 2>&1 | grep -i "transport\|efa\|rdma"
# Should show: selected transport: dc_mlx5 (or similar EFA transport)

# Quick latency test between nodes
ucx_perftest -t tag_bw -s 64 -n 100000 10.0.1.11
ucx_perftest -t tag_lat -s 64 -n 100000 10.0.1.11
```

---

## 4. Benchmark Scenarios

### R1: RDMA Write Latency (Micro-benchmark)

UCX one-sided write latency — ZeptoDB transport layer 직접 측정.

```bash
# On load gen: run bench_cluster with UCX backend
./bench_cluster --transport ucx --peer 10.0.1.10

# Measures:
#   - 64B write + fence latency (target: <5μs)
#   - 4KB bulk write throughput (target: >20 GB/s)
#   - Memory registration overhead
```

**Expected results:**

| Operation | SharedMem | TCP | EFA RDMA |
|-----------|-----------|-----|----------|
| 64B write + fence | ~100ns | ~60μs | **~2-5μs** |
| 64B read | ~50ns | ~60μs | **~2-5μs** |
| 4KB bulk write | ~30 GB/s | ~3 GB/s | **~20 GB/s** |

### R2: Tick Ingestion — TCP vs RDMA

Same workload, different transport. Measures real ingestion throughput difference.

```bash
# TCP baseline (on load gen)
./bench_distributed_ingest \
  --host 10.0.1.10:8123 \
  --transport tcp \
  --symbols 100 --ticks-per-symbol 1000000 --batch 512

# RDMA (on load gen)
./bench_distributed_ingest \
  --host 10.0.1.10:8123 \
  --transport ucx \
  --symbols 100 --ticks-per-symbol 1000000 --batch 512
```

**Expected:**

| Transport | Ticks/sec (single node) | Ticks/sec (3 nodes) |
|-----------|------------------------|---------------------|
| TCP RPC | ~5M | ~12M |
| UCX/EFA | **~8-10M** | **~20-25M** |

### R3: Remote Memory Read — Query Data Fetch

RDMA GET으로 원격 노드의 컬럼 데이터를 직접 읽는 경로 측정.

```bash
# Scatter-gather query: coordinator reads from 3 data nodes
# TCP: serialize → send → deserialize
# RDMA: one-sided read directly from remote column memory

# TCP
for i in $(seq 1 500); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST http://10.0.1.20:8123/ \
    -d 'SELECT symbol, vwap(price, volume) FROM trades GROUP BY symbol'
done > /tmp/tcp_latency.txt

# RDMA (coordinator uses UCX for data fetch)
# Same query, coordinator configured with --transport ucx
for i in $(seq 1 500); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST http://10.0.1.20:8123/ \
    -d 'SELECT symbol, vwap(price, volume) FROM trades GROUP BY symbol'
done > /tmp/rdma_latency.txt

# Compare
paste /tmp/tcp_latency.txt /tmp/rdma_latency.txt | \
  awk '{tcp+=$1; rdma+=$2; n++} END {
    printf "TCP avg=%.3fms  RDMA avg=%.3fms  speedup=%.1fx\n",
      tcp/n*1000, rdma/n*1000, (tcp/n)/(rdma/n)
  }'
```

### R4: Cross-Node ASOF JOIN — RDMA vs TCP

ASOF JOIN은 양쪽 테이블 데이터를 모두 읽어야 해서 네트워크 의존도가 높다.

```bash
# Pre-load: trades on node 0, quotes on node 1

# TCP
time curl -X POST http://10.0.1.20:8123/ \
  -d 'SELECT t.price, q.bid FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp WHERE t.symbol = 1'

# RDMA
time curl -X POST http://10.0.1.20:8123/ \
  -d 'SELECT t.price, q.bid FROM trades t ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp WHERE t.symbol = 1'
```

**Expected:** RDMA 2-5x faster for cross-node joins (data transfer dominates).

### R5: Sustained Throughput Under Load

30초간 최대 부하로 인제스트하면서 동시에 쿼리 실행. RDMA의 CPU 절감 효과 측정.

```bash
# Terminal 1: sustained ingestion
./bench_distributed_ingest \
  --host 10.0.1.20:8123 --transport ucx \
  --symbols 100 --ticks-per-symbol 10000000 --batch 512

# Terminal 2: concurrent queries (while ingesting)
for i in $(seq 1 100); do
  curl -s -o /dev/null -w "%{time_total}\n" \
    -X POST http://10.0.1.20:8123/ \
    -d 'SELECT count(*), vwap(price, volume) FROM trades WHERE symbol = 42'
  sleep 0.1
done > /tmp/query_under_load.txt

# Terminal 3: monitor CPU on data nodes
ssh 10.0.1.10 'sar -u 1 30'
```

**Key metric:** RDMA should show lower CPU utilization on data nodes (kernel bypass = no syscall overhead).

---

## 5. Result Template

```markdown
# RDMA / EFA Benchmark Results
# Date: YYYY-MM-DD
# Cluster: 4x m7a.4xlarge Spot (EFA, RDMA R/W, Nitro v4), placement group cluster
# UCX version: X.Y.Z, libfabric: X.Y.Z
# ZeptoDB: vX.Y.Z, Clang 19, -O3 -march=native, ZEPTO_USE_UCX=ON
# Cost: ~$2.25 (4 Spot instances × 2 hours)

## R1: Transport Micro-benchmark
| Operation | SharedMem | TCP | EFA RDMA |
|-----------|-----------|-----|----------|
| 64B write+fence | | | |
| 64B read | | | |
| 4KB bulk write | | | |

## R2: Ingestion Throughput
| Transport | 1 node | 3 nodes | Scale |
|-----------|--------|---------|-------|
| TCP | | | |
| UCX/EFA | | | |

## R3: Scatter-Gather Query
| Transport | Tier A (single) | Tier B (scatter) |
|-----------|----------------|-----------------|
| TCP | | |
| UCX/EFA | | |

## R4: Cross-Node ASOF JOIN
| Transport | Latency | Speedup |
|-----------|---------|---------|
| TCP | | baseline |
| UCX/EFA | | |

## R5: CPU Under Load
| Transport | Ingest ticks/sec | Query p99 | Data node CPU% |
|-----------|-----------------|-----------|----------------|
| TCP | | | |
| UCX/EFA | | | |
```

---

## 6. Troubleshooting

### EFA not detected

```bash
# Check EFA device exists
ls /sys/class/infiniband/
# Should show: rdmap0s6 (or similar)

# Check EFA driver loaded
lsmod | grep efa
# Should show: efa module

# Re-install EFA driver
sudo ./efa_installer.sh -y --reinstall
```

### UCX falls back to TCP

```bash
# Force EFA only (will fail if EFA not available)
export UCX_TLS=dc,ud
export UCX_NET_DEVICES=efa0

# Check what UCX actually selected
UCX_LOG_LEVEL=info ./zepto_server 2>&1 | grep "using transport"
```

### Memory registration fails

```bash
# Check memlock limit
ulimit -l
# Should show: unlimited

# If not, fix limits.conf and re-login
echo "* soft memlock unlimited" | sudo tee -a /etc/security/limits.conf
echo "* hard memlock unlimited" | sudo tee -a /etc/security/limits.conf
```

### Poor RDMA performance

```bash
# Check placement group — nodes MUST be in same cluster placement group
aws ec2 describe-instances --instance-ids i-xxx \
  --query 'Reservations[].Instances[].Placement.GroupName'

# Check MTU (EFA supports jumbo frames)
ip link show efa0 | grep mtu
# Should be 8900+ for EFA

# Check for packet drops
ethtool -S efa0 | grep -i drop
```

---

## 7. Cleanup

```bash
# Terminate all 4 instances
aws ec2 terminate-instances \
  --instance-ids $(aws ec2 describe-instances \
    --filters "Name=tag:Name,Values=zepto-efa-bench" "Name=instance-state-name,Values=running" \
    --query 'Reservations[].Instances[].InstanceId' --output text \
    --region us-east-1) \
  --region us-east-1

# Delete placement group (must be empty first)
aws ec2 delete-placement-group --group-name zepto-efa-bench --region us-east-1
```

> Spot instances also auto-terminate if interrupted, so no orphan cost risk.

---

## 8. Comparison: When to Use What

| Scenario | Transport | Instance | Cost (2h) |
|----------|-----------|----------|-----------|
| **RDMA R/W bench (추천)** | UCX/EFA | 4× `m7a.4xlarge` Spot | **~$2.25** |
| RDMA Read + 고대역폭 | UCX/EFA | 4× `hpc7g.4xlarge` OD | ~$13.44 |
| EFA SRD (RDMA 아님) | UCX/SRD | 4× `c5n.9xlarge` Spot | ~$4.66 |
| EKS TCP 벤치 | TCP RPC | EKS r7i.2xlarge | ~$12 |
| 개발/CI | SharedMem | 로컬 | $0 |

### Cost comparison vs previous guide
- Before: 5× c5n (9xl + 4xl + 2xl) = **~$30/2h**
- After: 4× m7a.4xlarge Spot = **~$2.25/2h** (13× cheaper)

### Related Guides
- **EKS benchmark (TCP):** `docs/bench/eks_multinode_benchmark.md`
- **Bare-metal benchmark (TCP):** `docs/bench/multinode_benchmark_guide.md`
- **UCX backend source:** `src/cluster/ucx_backend.h`
- **Transport abstraction:** `include/zeptodb/cluster/transport.h`
