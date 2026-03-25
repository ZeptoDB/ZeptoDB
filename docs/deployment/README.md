# ZeptoDB Deployment Guides

Guides for deploying ZeptoDB in production environments.

---

## Documentation

### 1. [PRODUCTION_DEPLOYMENT.md](PRODUCTION_DEPLOYMENT.md) — Required Reading
**Bare Metal vs Cloud Selection Guide**
- Deployment option selection by workload
- Bare metal deployment (HFT Edition)
- Cloud-native deployment (Analytics Edition)
- Comparison table and migration paths

### 2. [BARE_METAL_TUNING.md](BARE_METAL_TUNING.md)
**Bare Metal Optimization Detailed Guide** (TODO)
- CPU Pinning strategy
- NUMA optimization
- io_uring configuration
- Network tuning
- Benchmarking methods

### 3. [KUBERNETES_OPS.md](KUBERNETES_OPS.md)
**Kubernetes Operations Guide** (TODO)
- Helm Chart usage
- Rolling updates
- Monitoring configuration
- Troubleshooting

### 4. [Rolling Upgrade Guide](../ops/rolling_upgrade.md)
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

# 3. Verify
kubectl get pods -n zeptodb
curl -s http://<LB>:8123/health

# Upgrade
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb --set image.tag=1.1.0 --wait
```

---

## Deployment Option Selection

| Requirement | Deployment |
|-------------|-----------|
| Latency < 100us | **Bare metal** |
| Latency < 1ms | Bare metal recommended |
| Latency > 1ms | Cloud OK |
| Auto-scaling needed | Cloud |
| Fixed workload | Bare metal |
| Cost optimization priority | Cloud (spot) |

---

## Support

- **Enterprise support**: enterprise@zeptodb.io
- **Community**: https://discord.gg/zeptodb
- **Documentation**: https://docs.zeptodb.io
