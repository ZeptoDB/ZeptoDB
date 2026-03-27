# Cloud Performance Tuning Guide

ZeptoDB는 SIMD, NUMA-aware allocation, lock-free ring buffer, LLVM JIT 등 하드웨어 최적화를 적극 활용합니다. Kubernetes + 컨테이너 환경에서도 bare-metal에 근접한 성능을 달성하려면 아래 설정이 필요합니다.

---

## 왜 기본 K8s 설정으로는 부족한가

컨테이너 자체의 오버헤드는 거의 0입니다 (Linux cgroup/namespace). 성능 저하는 K8s 기본 스케줄링 정책에서 발생합니다:

| 문제 | 원인 | 영향 |
|------|------|------|
| CPU throttling | cgroup CFS quota가 busy-spin 중단 | ring buffer ingestion throughput 급락 |
| NUMA 무시 | Pod이 여러 NUMA 노드에 걸침 | 메모리 latency 2~3× 증가 |
| 디스크 I/O | EBS gp3 = 125 MB/s | HDB flush 40× 느림 (vs NVMe) |
| THP jitter | Transparent Huge Pages compaction | 예측 불가능한 latency spike |

---

## 1. Guaranteed QoS (CPU Pinning)

`requests == limits`로 설정하면 kubelet이 전용 cpuset을 할당합니다. CFS throttling이 사라지고 lock-free ring buffer가 안정적으로 동작합니다.

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

## 2. Kubelet 설정 (노드 레벨)

EKS managed node group의 launch template 또는 Karpenter userData에서 kubelet 플래그를 설정합니다:

```bash
# kubelet 추가 플래그
--cpu-manager-policy=static              # 전용 CPU 코어 할당
--topology-manager-policy=single-numa-node  # 단일 NUMA 노드 강제
--memory-manager-policy=Static           # 메모리도 NUMA-local 할당
--reserved-memory='[{"numaNode":0,"limits":{"memory":"1Gi"}}]'
```

EKS에서 적용하는 방법:

```bash
# EKS managed node group — launch template userdata
#!/bin/bash
/etc/eks/bootstrap.sh my-cluster \
  --kubelet-extra-args '--cpu-manager-policy=static --topology-manager-policy=single-numa-node'
```

## 3. 노드 부팅 시 커널 튜닝

Karpenter `userData`로 자동 적용됩니다 (`values.yaml`의 `karpenter.realtime.userData`):

| 튜닝 항목 | 설정 | 목적 |
|-----------|------|------|
| Hugepages | `echo 8192 > /proc/sys/vm/nr_hugepages` | arena allocator 성능 |
| CPU governor | `echo performance > scaling_governor` | 일관된 clock speed |
| NUMA balancing | `echo 0 > numa_balancing` | ZeptoDB가 직접 NUMA 관리 |
| Swappiness | `sysctl vm.swappiness=0` | in-memory DB에 swap 불필요 |
| THP | `echo never > transparent_hugepage/enabled` | latency spike 방지 |
| Network | `busy_poll=50, tcp_low_latency=1` | 네트워크 latency 감소 |

## 4. 스토리지: Instance Store vs EBS

HDB flush 성능이 중요하면 instance store가 있는 인스턴스를 사용합니다:

| 스토리지 | 처리량 | 지연 | 용도 |
|----------|--------|------|------|
| Instance store (i4i/i4g) | ~4 GB/s | ~100μs | HDB flush, WAL |
| EBS io2 Block Express | 4 GB/s | ~200μs | 영구 저장 필요 시 |
| EBS gp3 (기본) | 125 MB/s | ~1ms | 비용 우선 |

```yaml
# values.yaml — Karpenter realtime pool
karpenter:
  realtime:
    instanceFamilies: ["i4g", "c7g"]
    instanceStorePolicy: "RAID0"       # NVMe instance store 자동 RAID0
```

> ⚠️ Instance store는 노드 종료 시 데이터가 사라집니다. WAL replication이 활성화되어 있어야 합니다.

## 5. 네트워크: hostNetwork

클러스터 모드에서 data node 간 RPC 통신이 많으면 CNI 오버헤드를 우회할 수 있습니다:

```yaml
# values.yaml
performanceTuning:
  hostNetwork: false    # true로 변경 시 CNI bypass
```

hostNetwork 사용 시 주의사항:
- Pod이 호스트 네트워크 네임스페이스를 공유하므로 포트 충돌 주의
- `dnsPolicy: ClusterFirstWithHostNet`이 자동 설정됨
- 보안 그룹에서 RPC 포트 (8223) 허용 필요

## 6. RDMA / UCX

클라우드 VPC에서는 일반 RDMA가 지원되지 않습니다. AWS EFA(Elastic Fabric Adapter)를 사용해야 합니다:

- EFA 지원 인스턴스: `hpc7g`, `p5.48xlarge`, `trn1.32xlarge` 등
- EFA device plugin DaemonSet 설치 필요
- 대부분의 워크로드에서는 TCP + busy_poll 튜닝으로 충분

---

## 적용 방법

### Helm 배포

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb \
  --set karpenter.enabled=true \
  --set performanceTuning.hostNetwork=false \
  --set performanceTuning.hugepages.enabled=true
```

### 성능 검증

배포 후 아래 명령으로 설정이 올바르게 적용되었는지 확인합니다:

```bash
# Pod의 QoS 클래스 확인 (Guaranteed여야 함)
kubectl get pod -n zeptodb -o jsonpath='{.items[0].status.qosClass}'

# cpuset 확인 (전용 코어가 할당되어야 함)
kubectl exec -n zeptodb <pod> -- cat /sys/fs/cgroup/cpuset.cpus

# hugepages 확인
kubectl exec -n zeptodb <pod> -- cat /proc/meminfo | grep -i huge

# NUMA 확인
kubectl exec -n zeptodb <pod> -- numactl --show
```

---

## 성능 비교 요약

올바르게 튜닝된 K8s 환경 vs bare-metal:

| 항목 | Bare-metal | K8s (튜닝 후) | 차이 |
|------|-----------|--------------|------|
| Ingestion (ring buffer) | 5.52M evt/s | ~5.4M evt/s | ~2% |
| Filter 1M rows (SIMD) | 272μs | ~275μs | ~1% |
| VWAP 1M rows (JIT) | 532μs | ~540μs | ~1.5% |
| HDB flush (instance store) | 4.8 GB/s | ~4.5 GB/s | ~6% |
| HDB flush (EBS gp3) | 4.8 GB/s | 125 MB/s | 38× 느림 |

SIMD, JIT, lock-free 구조는 유저스페이스 연산이므로 컨테이너 영향이 거의 없습니다. 차이가 나는 부분은 I/O와 스케줄링이며, 위 튜닝으로 대부분 해소됩니다.
