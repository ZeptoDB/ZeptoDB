# ZeptoDB Production Deployment Guide

> Choose **bare metal** or **cloud-native** deployment based on your workload.

**Last Updated:** 2026-03-22

---

## Deployment Option Selection Guide

| Workload | Latency Requirement | Recommended | Reason |
|---------|---------------------|-------------|--------|
| **HFT tick processing** | < 100us | Bare metal | Container overhead is fatal |
| **Market data feed** | < 500us | Bare metal | Network latency critical |
| **Real-time risk** | < 1ms | Bare metal recommended | Stability critical |
| **Quant backtesting** | > 10ms | Cloud OK | Cost-effective |
| **Batch analytics** | > 100ms | Cloud OK | Elasticity important |
| **Development/testing** | Any | Cloud recommended | Fast provisioning |

---

## Option 1: Bare Metal Deployment (HFT Edition)

### Target Customers
- HFT trading firms
- Market data providers
- Real-time risk systems

### System Requirements

**Hardware:**
- **CPU**: Intel Xeon (Skylake+) or AMD EPYC (Zen 3+)
  - Minimum 16 cores (realtime 4 + analytics 4 + system 8)
  - Recommended: 32 cores+
- **RAM**: Minimum 64GB, recommended 256GB+
  - Hugepages support
  - ECC memory recommended
- **Network**: 10GbE or higher
  - RDMA support (Mellanox ConnectX-5+) recommended
  - Kernel bypass (DPDK) support
- **Storage**: NVMe SSD
  - Minimum 1TB for HDB
  - io_uring support (Kernel 5.10+)

**Operating System:**
- RHEL 8.6+ / Rocky Linux 8+
- Ubuntu 22.04 LTS+
- Kernel 5.10+ (io_uring, nohz_full)

### Installation Steps

#### 1. System Tuning

```bash
# Edit /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3 \
    transparent_hugepage=never intel_pstate=disable \
    default_hugepagesz=2M hugepagesz=2M hugepages=16384 \
    processor.max_cstate=1 intel_idle.max_cstate=0"

sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

#### 2. Runtime Tuning Script

`/opt/zeptodb/tune_bare_metal.sh`:

```bash
#!/bin/bash
# ZeptoDB Bare Metal Optimization Script

set -e

echo "=== ZeptoDB Bare Metal Tuning ==="

# 1. CPU Governor: performance mode
echo "Setting CPU governor to performance..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] && echo performance > $cpu
done

# 2. Disable Turbo Boost (consistent latency)
echo "Disabling Turbo Boost..."
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true

# 3. Hugepages: 32GB (16,384 x 2MB pages)
echo "Configuring Hugepages (32GB)..."
echo 16384 > /proc/sys/vm/nr_hugepages

# 4. IRQ Affinity: move interrupts to system cores (8-15)
echo "Setting IRQ affinity to cores 8-15..."
for irq in $(grep -E 'eth|mlx' /proc/interrupts | cut -d: -f1 | tr -d ' '); do
    echo ff00 > /proc/irq/$irq/smp_affinity 2>/dev/null || true
done

# 5. Network stack tuning (low latency)
echo "Tuning network stack..."
sysctl -w net.core.busy_poll=50
sysctl -w net.core.busy_read=50
sysctl -w net.ipv4.tcp_low_latency=1
sysctl -w net.core.netdev_max_backlog=10000
sysctl -w net.core.rmem_max=134217728
sysctl -w net.core.wmem_max=134217728

# 6. Disable NUMA balancing (explicit control)
echo "Disabling NUMA balancing..."
echo 0 > /proc/sys/kernel/numa_balancing

# 7. Minimize swappiness
echo "Setting swappiness to 0..."
sysctl -w vm.swappiness=0

# 8. Limit C-states (consistent latency)
echo "Limiting C-states..."
for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do
    [ -f "$cpu" ] && echo 1 > $cpu 2>/dev/null || true
done

echo "=== Tuning complete ==="
echo ""
echo "Verify with:"
echo "  - cat /proc/cmdline"
echo "  - numactl --hardware"
echo "  - cat /proc/meminfo | grep Huge"
echo "  - cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
```

#### 3. ZeptoDB Build (Bare Metal Optimized)

```bash
# Install dependencies
sudo dnf install -y clang19 clang19-devel llvm19-devel \
    highway-devel numactl-devel ucx-devel lz4-devel \
    liburing-devel ninja-build cmake

# Download source
git clone https://github.com/your-org/zeptodb.git
cd zeptodb

# Bare metal optimized build
mkdir -p build && cd build
cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-19 \
    -DCMAKE_CXX_COMPILER=clang++-19 \
    -DCMAKE_CXX_FLAGS="-march=native -mtune=native -O3" \
    -DAPEX_BARE_METAL=ON \
    -DAPEX_USE_HUGEPAGES=ON \
    -DAPEX_USE_IO_URING=ON \
    -DAPEX_USE_RDMA=ON

ninja -j$(nproc)

# Run tuning script
sudo ../deploy/scripts/tune_bare_metal.sh
```

#### 4. Run (NUMA-Aware)

```bash
# Single NUMA node
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./zepto_server \
        --port 8123 \
        --realtime-cores 0-3 \
        --analytics-cores 4-7 \
        --hugepages

# Multi-NUMA node (one instance per node)
# Node 0 (realtime)
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./zepto_server --port 8123 --node-id 0 --role realtime &

# Node 1 (analytics)
sudo numactl --cpunodebind=1 --membind=1 \
    taskset -c 16-19 \
    ./zepto_server --port 8124 --node-id 1 --role analytics &
```

#### 5. Verification & Monitoring

```bash
# Check CPU affinity
taskset -p $(pidof zepto_server)

# Check NUMA memory usage
numastat -p $(pidof zepto_server)

# Check Hugepages usage
grep Huge /proc/meminfo

# Latency profiling
sudo perf record -F 999 -a -g -- sleep 30
sudo perf script | flamegraph.pl > zepto_latency.svg

# Continuous monitoring
watch -n 1 'cat /proc/$(pidof zepto_server)/status | grep VmHWM'
```

### Bare Metal Benchmarks (Expected)

| Metric | Bare Metal (Optimized) | Target |
|--------|----------------------|--------|
| filter 1M | **250us** | < 300us |
| VWAP 1M | **500us** | < 600us |
| Ingestion | **6.0M ticks/sec** | > 5.5M |
| p99 latency | **550us** | < 600us |
| Jitter (p99.9 - p50) | **+/-5us** | < 10us |

---

## Option 2: Cloud-Native Deployment (Analytics Edition)

### Target Customers
- Quant research teams
- Risk analytics
- Batch backtesting
- Development/test environments

### Architecture

```
+─────────────────────────────────────+
│  Load Balancer (ALB)                │
+────────────+────────────────────────+
             |
    +────────+─────────+
    |                  |
+───v────+      +─────v───+
│ Pod 1  │      │ Pod 2   │    ... (Auto-scaling)
│ APEX   │      │ APEX    │
+───+────+      +────+────+
    |                |
    +────────+───────+
             |
    +────────v─────────+
    │ EBS (HDB Storage)│
    +──────────────────+
```

### System Requirements

**Cloud infrastructure:**
- **AWS**: c6i.4xlarge+ (16 vCPU, 32GB RAM)
- **GCP**: c2-standard-16+ (16 vCPU, 64GB RAM)
- **Azure**: F16s v2+ (16 vCPU, 32GB RAM)

**Kubernetes:**
- K8s 1.26+
- CNI: Calico or Cilium
- Storage: EBS gp3, Persistent Disk SSD

### Deployment Steps

#### 1. Docker Image Build

`deploy/docker/Dockerfile`:

```dockerfile
# Multi-stage build
FROM clang:19 AS builder

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake ninja-build \
    libhighway-dev libnuma-dev liblz4-dev \
    liburing-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# Cloud-optimized build
RUN mkdir -p build && cd build && \
    cmake .. \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-march=x86-64-v3 -O3" \
        -DAPEX_CLOUD_NATIVE=ON \
        -DAPEX_USE_HUGEPAGES=OFF \
        -DAPEX_USE_RDMA=OFF && \
    ninja -j$(nproc)

# Runtime image
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libhighway1 libnuma1 liblz4-1 liburing2 \
    curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/zeptodb
COPY --from=builder /build/build/zepto_server .
COPY --from=builder /build/build/*.so .

# Non-root user
RUN useradd -r -u 1000 zeptodb && \
    chown -R zeptodb:zeptodb /opt/zeptodb

USER zeptodb
EXPOSE 8123

# Health check
HEALTHCHECK --interval=10s --timeout=3s --start-period=30s \
    CMD curl -f http://localhost:8123/health || exit 1

ENTRYPOINT ["./zepto_server"]
CMD ["--port", "8123"]
```

Build:

```bash
docker build -t zeptodb:latest .
docker tag zeptodb:latest your-registry/zeptodb:v1.0.0
docker push your-registry/zeptodb:v1.0.0
```

#### 2. Kubernetes Deployment

`deploy/k8s/deployment.yaml`:

```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: zeptodb
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: zepto-config
  namespace: zeptodb
data:
  zeptodb.conf: |
    port: 8123
    worker_threads: 8
    analytics_mode: true
    cloud_native: true
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: zeptodb
  namespace: zeptodb
spec:
  replicas: 3
  selector:
    matchLabels:
      app: zeptodb
  template:
    metadata:
      labels:
        app: zeptodb
    spec:
      affinity:
        podAntiAffinity:
          preferredDuringSchedulingIgnoredDuringExecution:
          - weight: 100
            podAffinityTerm:
              labelSelector:
                matchExpressions:
                - key: app
                  operator: In
                  values:
                  - zeptodb
              topologyKey: kubernetes.io/hostname
      containers:
      - name: zeptodb
        image: your-registry/zeptodb:v1.0.0
        ports:
        - containerPort: 8123
          name: http
          protocol: TCP
        resources:
          requests:
            cpu: "4"
            memory: "16Gi"
          limits:
            cpu: "8"
            memory: "32Gi"
        env:
        - name: APEX_WORKER_THREADS
          value: "8"
        - name: APEX_ANALYTICS_MODE
          value: "true"
        volumeMounts:
        - name: config
          mountPath: /opt/zeptodb/config
        - name: data
          mountPath: /opt/zeptodb/data
        livenessProbe:
          httpGet:
            path: /health
            port: 8123
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /ready
            port: 8123
          initialDelaySeconds: 10
          periodSeconds: 5
      volumes:
      - name: config
        configMap:
          name: zepto-config
      - name: data
        persistentVolumeClaim:
          claimName: zeptodb-pvc
---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: zeptodb-pvc
  namespace: zeptodb
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: gp3  # AWS EBS gp3
  resources:
    requests:
      storage: 500Gi
---
apiVersion: v1
kind: Service
metadata:
  name: zeptodb-service
  namespace: zeptodb
spec:
  type: LoadBalancer
  selector:
    app: zeptodb
  ports:
  - port: 8123
    targetPort: 8123
    protocol: TCP
    name: http
---
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: zeptodb-hpa
  namespace: zeptodb
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: zeptodb
  minReplicas: 3
  maxReplicas: 10
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
  - type: Resource
    resource:
      name: memory
      target:
        type: Utilization
        averageUtilization: 80
```

Deploy:

```bash
kubectl apply -f deploy/k8s/deployment.yaml

# Verify
kubectl get pods -n zeptodb
kubectl get svc -n zeptodb

# Check logs
kubectl logs -f deployment/zeptodb -n zeptodb
```

#### 3. Helm Chart (Recommended)

`helm/zeptodb/values.yaml`:

```yaml
replicaCount: 3

image:
  repository: your-registry/zeptodb
  tag: v1.0.0
  pullPolicy: IfNotPresent

resources:
  requests:
    cpu: 4
    memory: 16Gi
  limits:
    cpu: 8
    memory: 32Gi

autoscaling:
  enabled: true
  minReplicas: 3
  maxReplicas: 10
  targetCPUUtilizationPercentage: 70
  targetMemoryUtilizationPercentage: 80

persistence:
  enabled: true
  storageClass: gp3
  size: 500Gi

config:
  workerThreads: 8
  analyticsMode: true
  cloudNative: true

monitoring:
  prometheus:
    enabled: true
  grafana:
    enabled: true
```

Install:

```bash
helm install zeptodb ./deploy/helm/zeptodb \
    --namespace zeptodb \
    --create-namespace \
    --values values-prod.yaml
```

#### 4. Monitoring (Prometheus + Grafana)

`deploy/k8s/monitoring.yaml`:

```yaml
apiVersion: v1
kind: ServiceMonitor
metadata:
  name: zeptodb-metrics
  namespace: zeptodb
spec:
  selector:
    matchLabels:
      app: zeptodb
  endpoints:
  - port: http
    path: /metrics
    interval: 15s
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: grafana-dashboard
  namespace: zeptodb
data:
  zeptodb.json: |
    {
      "dashboard": {
        "title": "ZeptoDB Analytics",
        "panels": [
          {
            "title": "Query Latency (p95)",
            "targets": [{"expr": "histogram_quantile(0.95, zepto_query_duration_seconds)"}]
          },
          {
            "title": "Throughput (queries/sec)",
            "targets": [{"expr": "rate(zepto_queries_total[1m])"}]
          },
          {
            "title": "Memory Usage",
            "targets": [{"expr": "zepto_memory_bytes / 1024 / 1024 / 1024"}]
          }
        ]
      }
    }
```

### Cloud Benchmarks (Expected)

| Metric | Cloud (c6i.4xlarge) | Target |
|--------|---------------------|--------|
| filter 1M | **350us** | < 500us |
| VWAP 1M | **650us** | < 800us |
| Ingestion | **4.5M ticks/sec** | > 4M |
| p99 latency | **800us** | < 1ms |
| Throughput | **5K queries/sec** | > 3K |

---

## Comparison: Bare Metal vs Cloud

| Item | Bare Metal | Cloud | Difference |
|------|-----------|-------|------------|
| **Initial cost** | High (server purchase) | Low (per-hour billing) | 10x |
| **Operational complexity** | High (manual tuning) | Low (automated) | 3x |
| **Latency p50** | **250us** | 350us | +40% |
| **Latency p99** | **550us** | 800us | +45% |
| **Jitter** | **+/-5us** | +/-30us | 6x worse |
| **Throughput** | **6M/sec** | 4.5M/sec | -25% |
| **Scalability** | Manual (add servers) | Auto (HPA) | Auto |
| **Cost (annual)** | $100K (fixed) | $50K-$200K (variable) | Usage-dependent |
| **Suitable workloads** | HFT, real-time | Analytics, backtesting | — |

---

## Migration Path

### Phase 1: Development/Testing (Cloud)
```
Development -> Docker local test -> K8s staging -> Validation
```

### Phase 2: Production (Choose One)

**Option A: Hybrid**
- HFT: Bare metal (on-premises)
- Backtesting: Cloud (AWS)

**Option B: Full Cloud**
- All workloads: K8s
- Latency critical: Dedicated nodes (c6i.metal)

**Option C: Full Bare Metal**
- All workloads: On-premises
- High operational complexity

---

## Checklist

### Before Bare Metal Deployment
- [ ] Hardware specs verified (CPU, RAM, Network)
- [ ] Kernel parameters set (`isolcpus`, `hugepages`)
- [ ] Tuning script run (`tune_bare_metal.sh`)
- [ ] NUMA topology verified
- [ ] Benchmarks run (target latency achieved)

### Before Cloud Deployment
- [ ] Docker image built and pushed
- [ ] K8s cluster ready
- [ ] Persistent Volume configured
- [ ] Monitoring set up (Prometheus, Grafana)
- [ ] Auto-scaling policy configured

---

## Support

- **Bare metal deployment support**: enterprise@zeptodb.com
- **Cloud deployment support**: cloud@zeptodb.com
- **Community**: https://discord.gg/zeptodb

---

**Next documents:**
- [Bare Metal Tuning Detailed Guide](BARE_METAL_TUNING.md)
- [Kubernetes Operations Guide](KUBERNETES_OPS.md)
- [Monitoring & Alerting](MONITORING.md)
