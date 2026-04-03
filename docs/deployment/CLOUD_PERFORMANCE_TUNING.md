# Cloud Performance Tuning Guide

ZeptoDB actively leverages hardware optimizations such as SIMD, NUMA-aware allocation, lock-free ring buffer, and LLVM JIT. The following configuration is required to achieve near bare-metal performance even in Kubernetes + container environments.

---

## Why Default K8s Settings Are Not Enough

The overhead of containers themselves is nearly zero (Linux cgroup/namespace). Performance degradation comes from K8s default scheduling policies:

| Problem | Cause | Impact |
|---------|-------|--------|
| CPU throttling | cgroup CFS quota interrupts busy-spin | ring buffer ingestion throughput drops sharply |
| NUMA ignored | Pod spans multiple NUMA nodes | Memory latency increases 2~3× |
| Disk I/O | EBS gp3 = 125 MB/s | HDB flush 40× slower (vs NVMe) |
| THP jitter | Transparent Huge Pages compaction | Unpredictable latency spikes |

---

## 1. Guaranteed QoS (CPU Pinning)

Setting `requests == limits` causes kubelet to assign a dedicated cpuset. CFS throttling is eliminated and the lock-free ring buffer operates reliably.

```yaml
# values.yaml
resources:
  requests:
    cpu: "8"
    memory: "32Gi"
    hugepages-2Mi: "4Gi"
  limits:
    cpu: "8"              # requests == limits → Guaranteed QoS
    memory: "32Gi"
    hugepages-2Mi: "4Gi"
```

## 2. Kubelet Configuration (Node Level)

Configure kubelet flags in the launch template of EKS managed node groups or in Karpenter userData:

```bash
# Additional kubelet flags
--cpu-manager-policy=static              # Dedicated CPU core allocation
--topology-manager-policy=single-numa-node  # Enforce single NUMA node
--memory-manager-policy=Static           # NUMA-local memory allocation as well
--reserved-memory='[{"numaNode":0,"limits":{"memory":"1Gi"}}]'
```

How to apply on EKS:

```bash
# EKS managed node group — launch template userdata
#!/bin/bash
/etc/eks/bootstrap.sh my-cluster \
  --kubelet-extra-args '--cpu-manager-policy=static --topology-manager-policy=single-numa-node'
```

## 3. Kernel Tuning at Node Boot

Automatically applied via Karpenter `userData` (`values.yaml`'s `karpenter.realtime.userData`):

| Tuning Item | Setting | Purpose |
|-------------|---------|---------|
| Hugepages | `echo 8192 > /proc/sys/vm/nr_hugepages` | Arena allocator performance |
| CPU governor | `echo performance > scaling_governor` | Consistent clock speed |
| NUMA balancing | `echo 0 > numa_balancing` | ZeptoDB manages NUMA directly |
| Swappiness | `sysctl vm.swappiness=0` | Swap unnecessary for in-memory DB |
| THP | `echo never > transparent_hugepage/enabled` | Prevent latency spikes |
| Network | `busy_poll=50, tcp_low_latency=1` | Reduce network latency |

## 4. Storage: Instance Store vs EBS

Use instances with instance store when HDB flush performance matters:

| Storage | Throughput | Latency | Use Case |
|---------|-----------|---------|----------|
| Instance store (i4i/i4g) | ~4 GB/s | ~100μs | HDB flush, WAL |
| EBS io2 Block Express | 4 GB/s | ~200μs | When persistent storage is needed |
| EBS gp3 (default) | 125 MB/s | ~1ms | Cost priority |

```yaml
# values.yaml — Karpenter realtime pool
karpenter:
  realtime:
    instanceFamilies: ["i4g", "c7g"]
    instanceStorePolicy: "RAID0"       # Automatic RAID0 for NVMe instance store
```

> ⚠️ Instance store data is lost when the node terminates. WAL replication must be enabled.

## 5. Network: hostNetwork

When there is heavy RPC communication between data nodes in cluster mode, you can bypass CNI overhead:

```yaml
# values.yaml
performanceTuning:
  hostNetwork: false    # Change to true to bypass CNI
```

Considerations when using hostNetwork:
- Pods share the host network namespace, so watch out for port conflicts
- `dnsPolicy: ClusterFirstWithHostNet` is automatically configured
- RPC port (8223) must be allowed in security groups

## 6. RDMA / UCX

Standard RDMA is not supported in cloud VPCs. You must use AWS EFA (Elastic Fabric Adapter):

- EFA-supported instances: `hpc7g`, `p5.48xlarge`, `trn1.32xlarge`, etc.
- EFA device plugin DaemonSet installation required
- For most workloads, TCP + busy_poll tuning is sufficient

---

## How to Apply

### Helm Deployment

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb \
  --set karpenter.enabled=true \
  --set performanceTuning.hostNetwork=false \
  --set performanceTuning.hugepages.enabled=true
```

### Performance Verification

After deployment, verify that the settings are correctly applied with the following commands:

```bash
# Check Pod QoS class (should be Guaranteed)
kubectl get pod -n zeptodb -o jsonpath='{.items[0].status.qosClass}'

# Check cpuset (dedicated cores should be assigned)
kubectl exec -n zeptodb <pod> -- cat /sys/fs/cgroup/cpuset.cpus

# Check hugepages
kubectl exec -n zeptodb <pod> -- cat /proc/meminfo | grep -i huge

# Check NUMA
kubectl exec -n zeptodb <pod> -- numactl --show
```

---

## Performance Comparison Summary

Properly tuned K8s environment vs bare-metal:

| Metric | Bare-metal | K8s (after tuning) | Difference |
|--------|-----------|-------------------|------------|
| Ingestion (ring buffer) | 5.52M evt/s | ~5.4M evt/s | ~2% |
| Filter 1M rows (SIMD) | 272μs | ~275μs | ~1% |
| VWAP 1M rows (JIT) | 532μs | ~540μs | ~1.5% |
| HDB flush (instance store) | 4.8 GB/s | ~4.5 GB/s | ~6% |
| HDB flush (EBS gp3) | 4.8 GB/s | 125 MB/s | 38× slower |

SIMD, JIT, and lock-free structures are userspace operations, so they are barely affected by containers. The differences come from I/O and scheduling, and the tuning above resolves most of them.
