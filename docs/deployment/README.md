# APEX-DB Deployment Guides

Guides for deploying APEX-DB in production environments.

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

---

## Quick Start

### Bare Metal (HFT/Real-time)

```bash
# 1. Run tuning script
cd apex-db
sudo ./scripts/tune_bare_metal.sh

# 2. Build
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPEX_BARE_METAL=ON
ninja -j$(nproc)

# 3. Run (NUMA-aware)
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./apex_server --port 8123 --hugepages
```

### Cloud (Quant/Analytics)

```bash
# 1. Build Docker image
docker build -t apex-db:latest .

# 2. Deploy to Kubernetes
kubectl apply -f k8s/deployment.yaml

# 3. Verify
kubectl get pods -n apex-db
kubectl get svc -n apex-db
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

- **Enterprise support**: enterprise@apex-db.io
- **Community**: https://discord.gg/apex-db
- **Documentation**: https://docs.apex-db.io
