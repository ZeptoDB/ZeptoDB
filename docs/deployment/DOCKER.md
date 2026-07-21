# ZeptoDB Docker Deployment Guide

> Single Docker image for all roles: master, data node, flight server, CLI.

**Image:** `zeptodb/zeptodb:0.1.8`
**Size:** ~300MB (distroless runtime)
**Base:** `gcr.io/distroless/cc-debian12:nonroot`
**Platforms:** `linux/amd64`, `linux/arm64`

---

## Quick Start

```bash
# Create one bootstrap admin key and its hash-only server key store.
mkdir -m 0700 -p auth
umask 077
export ZEPTO_ADMIN_KEY="zepto_$(openssl rand -hex 32)"
KEY_HASH="$(printf '%s' "$ZEPTO_ADMIN_KEY" | sha256sum | cut -d' ' -f1)"
printf '# zeptodb-keys-v1\nak_docker|docker-bootstrap-admin|%s|admin||1|0|||0\n' \
  "$KEY_HASH" > auth/api_keys.txt
# The distroless container runs as UID 65532. The mounted file contains only a
# one-way hash; make it readable while the parent directory remains private.
chmod 0444 auth/api_keys.txt

# Bind to loopback and mount the key store read-only.
docker run --rm -p 127.0.0.1:8123:8123 \
  -v "$PWD/auth/api_keys.txt:/run/secrets/zeptodb-auth/api_keys.txt:ro" \
  zeptodb/zeptodb:0.1.8

# Query via curl
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  -X POST http://localhost:8123/ -d "SELECT 1+1 AS result"
```

The production image does not create development credentials. Without a
mounted key store it remains fail-closed. `--no-auth` is available only as an
explicit isolated development/benchmark override. HTTP is plaintext unless
the server is configured with TLS or placed behind a TLS-terminating proxy;
do not publish the default port directly to the internet.

The image explicitly permits plaintext only inside the container boundary and
marks session cookies Secure. Put the Web UI behind HTTPS; Bearer-key curl
requests over a loopback-only Docker port remain suitable for initial checks.

The mounted file is intentionally read-only. Rotate keys by replacing the
file from a secrets manager and restarting the container. Deployments that
need runtime `/admin/keys` mutations must use a writable/Vault-backed store.

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
docker run --rm -p 127.0.0.1:8123:8123 \
  -v "$PWD/auth/api_keys.txt:/run/secrets/zeptodb-auth/api_keys.txt:ro" \
  zeptodb/zeptodb:0.1.8

# Data node (requires a cluster secret; see the Compose example below)
docker run --rm \
  --entrypoint ./zepto_data_node \
  -e ZEPTO_CLUSTER_SECRET_FILE=/run/secrets/cluster-secret \
  -v "$PWD/auth/cluster-secret:/run/secrets/cluster-secret:ro" \
  zeptodb/zeptodb:0.1.8 9000

# Arrow Flight server (production TLS + API-key authentication)
docker run --rm -p 8815:8815 \
  --entrypoint ./zepto_flight_server \
  -v "$PWD/tls:/run/zeptodb/tls:ro" \
  -v "$PWD/auth/api_keys.txt:/run/zeptodb/api_keys.txt:ro" \
  zeptodb/zeptodb:0.1.8 \
    --flight-host 0.0.0.0 \
    --flight-port 8815 \
    --tls-cert /run/zeptodb/tls/server.crt \
    --tls-key /run/zeptodb/tls/server.key \
    --api-keys-file /run/zeptodb/api_keys.txt

# CLI (interactive)
docker run -it --entrypoint ./zepto-cli zeptodb/zeptodb:0.1.8
```

---

## Enabled Features

| Feature | Status | Notes |
|---------|--------|-------|
| Highway SIMD | ✅ | Vectorized scan/aggregation |
| LLVM JIT | ✅ | Runtime query compilation |
| OpenSSL / TLS / JWT | ✅ | `--tls-cert`, `--jwt-issuer` to activate |
| AWS S3 | ✅ | `AWS_ACCESS_KEY_ID` env var to activate |
| Arrow Flight | ✅ | Loopback-safe default; TLS required for external bind |
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
| Auto-scaling | ❌ | ⚠️ K8s node capacity only; ZeptoDB HPA is currently blocked |
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

The shortened commands below focus on resource flags. Add the read-only API
key-store mount from Quick Start to every `zepto_http_server` container.

### CPU Pinning

```bash
# Pin to cores 0-3
docker run --cpuset-cpus="0-3" -p 8123:8123 zeptodb/zeptodb:0.1.8
```

### HugePages

```bash
# Host: allocate HugePages
echo 1024 > /proc/sys/vm/nr_hugepages

# Container: mount hugetlbfs
docker run --shm-size=2g \
  -v /dev/hugepages:/dev/hugepages \
  -p 8123:8123 zeptodb/zeptodb:0.1.8
```

### NUMA (multi-socket hosts)

```bash
docker run --cpuset-cpus="0-15" --cpuset-mems="0" \
  -p 8123:8123 zeptodb/zeptodb:0.1.8
```

### Persistent Storage

```bash
docker run -p 127.0.0.1:8123:8123 \
  -v "$PWD/auth/api_keys.txt:/run/secrets/zeptodb-auth/api_keys.txt:ro" \
  -v /data/zeptodb:/opt/zeptodb/data \
  zeptodb/zeptodb:0.1.8 \
    --bind 0.0.0.0 --allow-plaintext-http --secure-cookie \
    --port 8123 --ticks 0 \
    --storage-mode tiered --hdb-dir /opt/zeptodb/data \
    --api-keys-file /run/secrets/zeptodb-auth/api_keys.txt \
    --no-bootstrap-dev-keys --web-dir /opt/zeptodb/web
```

Mounting a directory alone does not enable persistence. The
`--storage-mode tiered --hdb-dir ...` arguments connect the process to that mount. Run the host
directory with ownership/permissions compatible with the distroless nonroot
UID (`65532`).

This is a tiered-storage evaluation path, not a production durability claim.
Hot-partition WAL/recovery and merging HDB rows into general SQL reads remain
release blockers; an end-to-end restart query must be proven before relying on
the directory as a recoverable database.

### S3 Upload

```bash
docker run -p 8123:8123 \
  -e AWS_ACCESS_KEY_ID=<key> \
  -e AWS_SECRET_ACCESS_KEY=<secret> \
  -e AWS_DEFAULT_REGION=us-east-1 \
  zeptodb/zeptodb:0.1.8
```

---

## Multi-Node Cluster (Docker Compose)

Create the API key store as in Quick Start, then create a separate peer secret:

```bash
openssl rand -base64 48 > auth/cluster-secret
# The direct-bind example runs as UID 65532; the private parent directory keeps
# this raw peer secret out of other users' path traversal.
chmod 0444 auth/cluster-secret
```

```yaml
services:
  master:
    image: zeptodb/zeptodb:0.1.8
    ports:
      - "127.0.0.1:8123:8123"
    environment:
      ZEPTO_CLUSTER_SECRET_FILE: /run/secrets/cluster_secret
    secrets:
      - api_keys
      - cluster_secret
    command: >
      --bind 0.0.0.0
      --allow-plaintext-http
      --secure-cookie
      --port 8123
      --ticks 0
      --api-keys-file /run/secrets/api_keys
      --no-bootstrap-dev-keys
      --web-dir /opt/zeptodb/web
      --add-node 1:data1:9000
      --add-node 2:data2:9001

  data1:
    image: zeptodb/zeptodb:0.1.8
    entrypoint: ["./zepto_data_node"]
    command: ["9000", "--node-id", "1"]
    environment:
      ZEPTO_CLUSTER_SECRET_FILE: /run/secrets/cluster_secret
    secrets:
      - cluster_secret

  data2:
    image: zeptodb/zeptodb:0.1.8
    entrypoint: ["./zepto_data_node"]
    command: ["9001", "--node-id", "2"]
    environment:
      ZEPTO_CLUSTER_SECRET_FILE: /run/secrets/cluster_secret
    secrets:
      - cluster_secret

secrets:
  api_keys:
    file: ./auth/api_keys.txt
  cluster_secret:
    file: ./auth/cluster-secret
```

```bash
docker compose up -d
curl -H "Authorization: Bearer $ZEPTO_ADMIN_KEY" \
  http://localhost:8123/admin/nodes
```

The shared secret provides mutual peer authentication and replay resistance;
it does not encrypt RPC payloads. Keep the Compose network private. Use an
encrypted overlay or a private/VPC network for traffic that crosses hosts.

---

## CLI Options Reference

### Arrow Flight server

```
zepto_flight_server [OPTIONS]

  --flight-host HOST       Flight bind host (default: 127.0.0.1)
  --flight-port PORT       Flight port (default: 8815)
  --port PORT              Alias for --flight-port
  --http-port PORT         Bundled HTTP/HTTPS port (default: 8123)
  --tls-cert PATH          PEM certificate for Flight and bundled HTTP
  --tls-key PATH           PEM private key for Flight and bundled HTTP
  --api-keys-file PATH     API key store (default: dev_keys.txt)
  --no-auth                Disable authentication (development only)
  --allow-insecure-flight  Allow plaintext non-loopback bind (development only)
  --ticks N                Seed demo rows
```

The certificate and key must be supplied together. Without TLS, Flight accepts
only a loopback bind unless `--allow-insecure-flight` is explicitly set. Do not
use `--no-auth` or the insecure override in production.

### HTTP server

```
zepto_http_server [OPTIONS]

Server:
  --bind HOST              HTTP bind host (default: 127.0.0.1)
  --allow-plaintext-http   Permit non-loopback HTTP behind a trusted TLS proxy
  --secure-cookie          Mark session cookies Secure for HTTPS clients
  --port PORT              HTTP API port (default: 8123)
  --ticks N                Seed synthetic demo rows (production: 0)
  --storage-mode MODE      pure (memory) or tiered (persistent HDB)
  --hdb-dir PATH           Persistent HDB directory (implies tiered mode)
  --web-dir PATH           Web UI static files directory
  --api-keys-file PATH     API key store
  --no-bootstrap-dev-keys  Compatibility no-op; production is already fail-closed
  --bootstrap-dev-keys     Create/print development credentials (development only)
  --no-auth                Disable authentication (development only)
  --log-level LEVEL        info|debug|warn|error

Cluster:
  --node-id ID             Node identifier
  --add-node ID:HOST:PORT  Add remote data node
  --rpc-port PORT          RPC port for HA communication
  --allow-insecure-cluster Allow unauthenticated RPC (development only)

Cluster RPC authentication is configured with exactly one of
`ZEPTO_CLUSTER_SECRET_FILE` (recommended) or `ZEPTO_CLUSTER_SECRET`. The secret
must contain at least 32 bytes.

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
| File logging bootstrap warning | Log directory is read-only or not writable | The server continues with stdout logging; mount a writable log volume only when local rotating files are required |
| `mmap failed, falling back` | HugePages not available on host | Normal — auto-fallback to regular pages |
| Port already in use | Another container on same port | Change `-p` mapping |
| Web UI blank page | Browser cache | Hard refresh (Ctrl+Shift+R) |
| `No Authorization header` | Auth enabled by default | Mount the API-key store and send `Authorization: Bearer ...`; use `--no-auth` only in isolated development |

---

See also:
- [Quick Start](../getting-started/QUICK_START.md)
- [Bare Metal Tuning](BARE_METAL_TUNING.md)
- [Production Deployment](PRODUCTION_DEPLOYMENT.md)
- [Multi-Node Cluster](MULTI_NODE_CLUSTER.md)
- [Kubernetes Operations](../operations/KUBERNETES_OPERATIONS.md)
