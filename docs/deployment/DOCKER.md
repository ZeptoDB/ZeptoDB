# ZeptoDB Docker Deployment Guide

> Single Docker image for all roles: master, data node, flight server, CLI.

**Image:** `zeptodb/zeptodb:0.0.1`
**Size:** ~300MB (distroless runtime)
**Base:** `gcr.io/distroless/cc-debian12:nonroot`

---

## Quick Start

```bash
# Start master node with Web UI
docker run -p 8123:8123 zeptodb/zeptodb:0.0.1

# Open Web UI
open http://localhost:8123/ui/

# Query via curl
curl -X POST http://localhost:8123/ -d "SELECT 1+1 AS result"
```

---

## Included Binaries

| Binary | Role | Default Port |
|--------|------|-------------|
| `zepto_http_server` | Master node — HTTP API, SQL engine, Web UI | 8123 |
| `zepto_data_node` | Data node — partition storage, RPC server | 9000+ |
| `zepto_flight_server` | Arrow Flight — gRPC streaming | 8815 |
| `zepto-cli` | Interactive SQL REPL | — |

## Running Different Roles

```bash
# Master node (default)
docker run -p 8123:8123 zeptodb/zeptodb:0.0.1

# Data node
docker run zeptodb/zeptodb:0.0.1 ./zepto_data_node 9000

# Arrow Flight server
docker run -p 8815:8815 zeptodb/zeptodb:0.0.1 ./zepto_flight_server --port 8815

# CLI (interactive)
docker run -it --entrypoint ./zepto-cli zeptodb/zeptodb:0.0.1
```

---

## Enabled Features

| Feature | Status | Notes |
|---------|--------|-------|
| Highway SIMD | ✅ | Vectorized scan/aggregation |
| LLVM JIT | ✅ | Runtime query compilation |
| OpenSSL / TLS / JWT | ✅ | `--tls-cert`, `--jwt-issuer` to activate |
| AWS S3 | ✅ | `AWS_ACCESS_KEY_ID` env var to activate |
| Arrow Flight | ✅ | `zepto_flight_server` binary |
| Parquet | ✅ | HDB flush to Parquet files |
| LZ4 compression | ✅ | WAL and HDB compression |
| io_uring | ✅ | Async I/O for HDB reads |
| HugePages | ✅ | Auto-detect: uses if available, falls back to regular pages |
| Web UI | ✅ | Served at `/ui/` on master node |

### Disabled (Docker-inappropriate)

| Feature | Reason | Alternative |
|---------|--------|-------------|
| UCX / RDMA | Requires InfiniBand hardware + kernel modules | Use bare-metal deployment |
| Python binding | Runtime dependency on Python + pybind11 | `pip install zeptodb` separately |
| tcmalloc | Marginal gain in containers | Default allocator is fine |

---

## Bare Metal vs Docker Comparison

### Performance

| Metric | Bare Metal (tuned) | Docker | Gap | Notes |
|--------|-------------------|--------|-----|-------|
| Tick-to-trade latency | < 1μs | 3–8μs | 3–8x | Container syscall overhead + no CPU isolation |
| Ingest throughput | 50M+ msg/s | 20–35M msg/s | ~2x | No NUMA pinning, shared scheduler |
| Query (1M rows scan) | ~200μs | ~250μs | ~25% | Minimal overhead for compute-bound |
| Query (aggregation) | ~50μs | ~60μs | ~20% | SIMD/JIT identical |
| Tail latency (p99) | < 5μs | 20–100μs | 10–20x | Kernel scheduler jitter |
| Network (RDMA) | < 2μs | N/A | — | RDMA not available in containers |

### Feature Availability

| Feature | Bare Metal | Docker |
|---------|-----------|--------|
| CPU pinning (`isolcpus`, `taskset`) | ✅ Full control | ⚠️ `--cpuset-cpus` only |
| NUMA binding | ✅ `numactl --membind` | ⚠️ `--cpuset-mems` only |
| HugePages (2MB/1GB) | ✅ Kernel-level config | ⚠️ Host must pre-allocate |
| RDMA / InfiniBand | ✅ Native | ❌ Not supported |
| io_uring | ✅ Native | ✅ Works (kernel 5.10+) |
| Kernel bypass (DPDK) | ✅ | ❌ |
| `nohz_full` (tickless) | ✅ | ❌ Host-level only |
| TLS / JWT auth | ✅ | ✅ |
| Arrow Flight | ✅ | ✅ |
| S3 upload | ✅ | ✅ |
| Web UI | ✅ (separate process) | ✅ (embedded, `/ui/`) |
| Rolling upgrade | Manual | ✅ K8s native |
| Auto-scaling | ❌ | ✅ K8s HPA |
| Deployment speed | Hours | Seconds |

### When to Use What

| Use Case | Recommendation | Reason |
|----------|---------------|--------|
| HFT / market making | **Bare metal** | Every microsecond matters; need RDMA, CPU isolation |
| Market data feed handler | **Bare metal** | Consistent sub-millisecond latency required |
| Real-time risk / surveillance | **Bare metal** or **Docker** | Depends on latency SLA |
| Quant research / backtesting | **Docker** | Cost-effective, easy to spin up/down |
| Analytics dashboard | **Docker** | Query latency tolerance > 10ms |
| Development / CI | **Docker** | Fast iteration, reproducible |
| Multi-tenant SaaS | **Docker + K8s** | Isolation, resource quotas, auto-scaling |

---

## Docker Performance Tuning

### CPU Pinning

```bash
# Pin to cores 0-3
docker run --cpuset-cpus="0-3" -p 8123:8123 zeptodb/zeptodb:0.0.1
```

### HugePages

```bash
# Host: allocate HugePages
echo 1024 > /proc/sys/vm/nr_hugepages

# Container: mount hugetlbfs
docker run --shm-size=2g \
  -v /dev/hugepages:/dev/hugepages \
  -p 8123:8123 zeptodb/zeptodb:0.0.1
```

### NUMA (multi-socket hosts)

```bash
docker run --cpuset-cpus="0-15" --cpuset-mems="0" \
  -p 8123:8123 zeptodb/zeptodb:0.0.1
```

### Persistent Storage

```bash
docker run -p 8123:8123 \
  -v /data/zeptodb:/opt/zeptodb/data \
  zeptodb/zeptodb:0.0.1
```

### S3 Upload

```bash
docker run -p 8123:8123 \
  -e AWS_ACCESS_KEY_ID=<key> \
  -e AWS_SECRET_ACCESS_KEY=<secret> \
  -e AWS_DEFAULT_REGION=us-east-1 \
  zeptodb/zeptodb:0.0.1
```

---

## Multi-Node Cluster (Docker Compose)

```yaml
version: "3.8"
services:
  master:
    image: zeptodb/zeptodb:0.0.1
    ports:
      - "8123:8123"
    command: >
      --port 8123 --no-auth
      --web-dir /opt/zeptodb/web
      --add-node 1:data1:9000
      --add-node 2:data2:9001

  data1:
    image: zeptodb/zeptodb:0.0.1
    entrypoint: ["./zepto_data_node"]
    command: ["9000", "--node-id", "1"]

  data2:
    image: zeptodb/zeptodb:0.0.1
    entrypoint: ["./zepto_data_node"]
    command: ["9001", "--node-id", "2"]
```

```bash
docker compose up -d
curl http://localhost:8123/admin/nodes  # verify cluster
```

---

## CLI Options Reference

```
zepto_http_server [OPTIONS]

Server:
  --port PORT              HTTP API port (default: 8123)
  --web-dir PATH           Web UI static files directory
  --no-auth                Disable authentication
  --log-level LEVEL        info|debug|warn|error

Cluster:
  --node-id ID             Node identifier
  --add-node ID:HOST:PORT  Add remote data node
  --rpc-port PORT          RPC port for HA communication

HA:
  --ha active|standby      Enable HA mode
  --peer HOST:PORT         HA peer address

TLS:
  --tls-cert PATH          TLS certificate file
  --tls-key PATH           TLS private key file

JWT / SSO:
  --jwt-issuer URL         Expected JWT issuer
  --jwt-audience AUD       Expected JWT audience
  --jwt-secret SECRET      HS256 shared secret
  --jwt-public-key PATH    RS256 PEM public key
  --jwks-url URL           JWKS endpoint (auto-fetch)
```

---

## Troubleshooting

| Issue | Cause | Fix |
|-------|-------|-----|
| `Permission denied [/var/log/zeptodb]` | Log directory not writable | Already handled in image; report if seen |
| `mmap failed, falling back` | HugePages not available on host | Normal — auto-fallback to regular pages |
| Port already in use | Another container on same port | Change `-p` mapping |
| Web UI blank page | Browser cache | Hard refresh (Ctrl+Shift+R) |
| `No Authorization header` | Auth enabled by default | Add `--no-auth` or provide API key |

---

See also:
- [Quick Start](../getting-started/QUICK_START.md)
- [Bare Metal Tuning](BARE_METAL_TUNING.md)
- [Production Deployment](PRODUCTION_DEPLOYMENT.md)
- [Multi-Node Cluster](MULTI_NODE_CLUSTER.md)
- [Kubernetes Operations](../operations/KUBERNETES_OPERATIONS.md)
