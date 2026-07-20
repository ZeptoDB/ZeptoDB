# ZeptoDB Deployment Guides

Guides for deploying ZeptoDB in production environments.

---

## Documentation

### 1. [DOCKER.md](DOCKER.md) — Getting Started
**Docker Deployment Guide**
- Quick start, all binaries, running different roles
- Enabled/disabled features comparison
- Bare metal vs Docker performance comparison
- Docker performance tuning (CPU pinning, HugePages, NUMA)
- Multi-node cluster with Docker Compose

### 2. [PRODUCTION_DEPLOYMENT.md](PRODUCTION_DEPLOYMENT.md) — Required Reading
**Bare Metal vs Cloud Selection Guide**
- Deployment option selection by workload
- Bare metal deployment (HFT Edition)
- Cloud-native deployment (Analytics Edition)
- Comparison table and migration paths

### 3. [BARE_METAL_TUNING.md](BARE_METAL_TUNING.md)
**Bare Metal Optimization Detailed Guide**
- CPU Pinning & isolation (`isolcpus`, `nohz_full`, IRQ affinity)
- NUMA optimization (per-node binding, `numastat` verification)
- Hugepages (2MB/1GB sizing, allocation, verification)
- C-state & CPU frequency control
- Kernel sysctl parameters
- Build optimization (tcmalloc, LTO, PGO)
- Network tuning (busy_poll, buffer sizes)
- Benchmarking & profiling methods

### 4. [Kubernetes Operations](../operations/KUBERNETES_OPERATIONS.md)
**Kubernetes Operations Guide**
- Helm Chart usage
- Rolling updates
- Monitoring configuration
- Troubleshooting

### 5. [Rolling Upgrade Guide](../ops/rolling_upgrade.md)
**Zero-Downtime Upgrade Procedures**
- Standard / Config-only / Canary / Cluster-mode upgrades
- Rollback procedures
- Pre-upgrade checklist

---

## Quick Start

### Bare Metal (HFT/Real-time)

```bash
# 1. Run tuning script
cd zeptodb
sudo ./deploy/scripts/tune_bare_metal.sh

# 2. Build
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPEX_BARE_METAL=ON
ninja -j$(nproc)

# 3. Run (NUMA-aware)
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./zepto_server --port 8123 --hugepages
```

### Cloud (Quant/Analytics)

```bash
# 1. Build Docker image
docker build -t zeptodb:latest .

# 2. Deploy via Helm (recommended)
helm install zeptodb ./deploy/helm/zeptodb -n zeptodb --create-namespace

# 3. Verify through the private ClusterIP Service
kubectl get pods -n zeptodb
kubectl port-forward svc/zeptodb 8123:8123 -n zeptodb
curl -s http://127.0.0.1:8123/health

# Upgrade
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb --set image.tag=1.1.0 --wait
```

The Helm default is one authenticated standalone replica behind a private
`ClusterIP`. It generates a bootstrap administrator key and a separate cluster
peer secret, mounts only the hash-only API key store into the pod, and preserves
generated credentials across upgrades. A default ingress NetworkPolicy limits
HTTP to the release namespace; add explicit peers for the approved TLS ingress
or external Prometheus namespace. Run `helm get notes zeptodb -n zeptodb`
for the explicit administrator-key retrieval command. Configure a
TLS-terminating ingress/load balancer before external exposure.

The default is deliberately in-memory. Enabling a PVC requires an explicit
incomplete-durability acknowledgement and is for evaluation only: hot-partition
WAL/recovery and SQL-visible HDB merge remain production release blockers.

Do not increase `replicaCount` in standalone mode. HPA rendering is currently
rejected in both standalone and cluster modes because StatefulSet startup peer
lists are static. For horizontal scale, explicitly set `cluster.enabled=true`,
apply a reviewed static `replicaCount`, review placement and persistence, and
keep peer RPC on a private network; shared-secret RPC authentication does not
encrypt payloads.

---

## Deployment Option Selection

| Requirement | Deployment |
|-------------|-----------|
| Latency < 100us | **Bare metal** |
| Latency < 1ms | Bare metal recommended |
| Latency > 1ms | Cloud OK |
| Elastic node capacity needed | Cloud (database replica changes remain reviewed/static) |
| Fixed workload | Bare metal |
| Cost optimization priority | Cloud (spot) |

---

## Support

- **Enterprise support**: skswlsaks@gmail.com
- **Community**: https://discord.gg/zeptodb
- **Documentation**: https://docs.zeptodb.com
