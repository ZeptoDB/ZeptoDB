# APEX-DB 프로덕션 배포 가이드

> 워크로드에 따라 **베어메탈** 또는 **클라우드 네이티브** 배포를 선택하세요.

**Last Updated:** 2026-03-22

---

## 📊 배포 옵션 선택 가이드

| 워크로드 | 레이턴시 요구 | 추천 배포 | 이유 |
|---------|-------------|---------|------|
| **HFT 틱 처리** | < 100μs | 🔴 베어메탈 | 컨테이너 오버헤드 치명적 |
| **마켓 데이터 피드** | < 500μs | 🔴 베어메탈 | 네트워크 레이턴시 중요 |
| **실시간 리스크** | < 1ms | 🟡 베어메탈 권장 | 안정성 중요 |
| **퀀트 백테스트** | > 10ms | 🟢 클라우드 OK | 비용 효율적 |
| **배치 분석** | > 100ms | 🟢 클라우드 OK | 탄력성 중요 |
| **개발/테스트** | 무관 | 🟢 클라우드 권장 | 빠른 프로비저닝 |

---

## 🏗️ Option 1: 베어메탈 배포 (HFT Edition)

### 타겟 고객
- HFT 트레이딩 펌
- 마켓 데이터 제공사
- 실시간 리스크 시스템

### 시스템 요구사항

**하드웨어:**
- **CPU**: Intel Xeon (Skylake+) 또는 AMD EPYC (Zen 3+)
  - 최소 16코어 (realtime 4 + analytics 4 + system 8)
  - 권장: 32코어+
- **RAM**: 최소 64GB, 권장 256GB+
  - Hugepages 지원
  - ECC 메모리 권장
- **Network**: 10GbE 이상
  - RDMA 지원 (Mellanox ConnectX-5+) 권장
  - Kernel bypass (DPDK) 지원
- **Storage**: NVMe SSD
  - HDB용 최소 1TB
  - io_uring 지원 (Kernel 5.10+)

**운영체제:**
- RHEL 8.6+ / Rocky Linux 8+
- Ubuntu 22.04 LTS+
- Kernel 5.10+ (io_uring, nohz_full)

### 설치 단계

#### 1. 시스템 튜닝

```bash
# /etc/default/grub 수정
GRUB_CMDLINE_LINUX="isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3 \
    transparent_hugepage=never intel_pstate=disable \
    default_hugepagesz=2M hugepagesz=2M hugepages=16384 \
    processor.max_cstate=1 intel_idle.max_cstate=0"

sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

#### 2. 런타임 튜닝 스크립트

`/opt/apex-db/tune_bare_metal.sh`:

```bash
#!/bin/bash
# APEX-DB 베어메탈 최적화 스크립트

set -e

echo "=== APEX-DB Bare Metal Tuning ==="

# 1. CPU Governor: performance 모드
echo "Setting CPU governor to performance..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] && echo performance > $cpu
done

# 2. Turbo Boost 비활성화 (일관된 레이턴시)
echo "Disabling Turbo Boost..."
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true

# 3. Hugepages: 32GB (16,384 x 2MB pages)
echo "Configuring Hugepages (32GB)..."
echo 16384 > /proc/sys/vm/nr_hugepages

# 4. IRQ Affinity: 인터럽트를 시스템 코어(8-15)로
echo "Setting IRQ affinity to cores 8-15..."
for irq in $(grep -E 'eth|mlx' /proc/interrupts | cut -d: -f1 | tr -d ' '); do
    echo ff00 > /proc/irq/$irq/smp_affinity 2>/dev/null || true
done

# 5. Network stack tuning (저지연)
echo "Tuning network stack..."
sysctl -w net.core.busy_poll=50
sysctl -w net.core.busy_read=50
sysctl -w net.ipv4.tcp_low_latency=1
sysctl -w net.core.netdev_max_backlog=10000
sysctl -w net.core.rmem_max=134217728
sysctl -w net.core.wmem_max=134217728

# 6. Disable NUMA balancing (명시적 제어)
echo "Disabling NUMA balancing..."
echo 0 > /proc/sys/kernel/numa_balancing

# 7. Swappiness 최소화
echo "Setting swappiness to 0..."
sysctl -w vm.swappiness=0

# 8. C-state 제한 (일관된 레이턴시)
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

#### 3. APEX-DB 빌드 (베어메탈 최적화)

```bash
# 의존성 설치
sudo dnf install -y clang19 clang19-devel llvm19-devel \
    highway-devel numactl-devel ucx-devel lz4-devel \
    liburing-devel ninja-build cmake

# 소스 다운로드
git clone https://github.com/your-org/apex-db.git
cd apex-db

# 베어메탈 최적화 빌드
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

# 튜닝 스크립트 실행
sudo ../scripts/tune_bare_metal.sh
```

#### 4. 실행 (NUMA 인식)

```bash
# 단일 NUMA 노드 실행
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./apex_server \
        --port 8123 \
        --realtime-cores 0-3 \
        --analytics-cores 4-7 \
        --hugepages

# 멀티 NUMA 노드 (각 노드당 인스턴스)
# Node 0 (realtime)
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 \
    ./apex_server --port 8123 --node-id 0 --role realtime &

# Node 1 (analytics)
sudo numactl --cpunodebind=1 --membind=1 \
    taskset -c 16-19 \
    ./apex_server --port 8124 --node-id 1 --role analytics &
```

#### 5. 검증 & 모니터링

```bash
# CPU affinity 확인
taskset -p $(pidof apex_server)

# NUMA 메모리 사용 확인
numastat -p $(pidof apex_server)

# Hugepages 사용 확인
grep Huge /proc/meminfo

# 레이턴시 프로파일링
sudo perf record -F 999 -a -g -- sleep 30
sudo perf script | flamegraph.pl > apex_latency.svg

# 지속적 모니터링
watch -n 1 'cat /proc/$(pidof apex_server)/status | grep VmHWM'
```

### 베어메탈 벤치마크 (예상)

| 메트릭 | 베어메탈 (최적화) | 목표 |
|--------|------------------|------|
| filter 1M | **250μs** | < 300μs |
| VWAP 1M | **500μs** | < 600μs |
| 인제스션 | **6.0M ticks/sec** | > 5.5M |
| p99 레이턴시 | **550μs** | < 600μs |
| Jitter (p99.9 - p50) | **±5μs** | < 10μs |

---

## ☁️ Option 2: 클라우드 네이티브 배포 (Analytics Edition)

### 타겟 고객
- 퀀트 리서치 팀
- 리스크 분석
- 배치 백테스트
- 개발/테스트 환경

### 아키텍처

```
┌─────────────────────────────────────┐
│  Load Balancer (ALB)                │
└────────────┬────────────────────────┘
             │
    ┌────────┴─────────┐
    │                  │
┌───▼────┐      ┌─────▼───┐
│ Pod 1  │      │ Pod 2   │    ... (Auto-scaling)
│ APEX   │      │ APEX    │
└───┬────┘      └────┬────┘
    │                │
    └────────┬───────┘
             │
    ┌────────▼─────────┐
    │ EBS (HDB Storage)│
    └──────────────────┘
```

### 시스템 요구사항

**클라우드 인프라:**
- **AWS**: c6i.4xlarge+ (16 vCPU, 32GB RAM)
- **GCP**: c2-standard-16+ (16 vCPU, 64GB RAM)
- **Azure**: F16s v2+ (16 vCPU, 32GB RAM)

**Kubernetes:**
- K8s 1.26+
- CNI: Calico 또는 Cilium
- Storage: EBS gp3, Persistent Disk SSD

### 배포 단계

#### 1. Docker 이미지 빌드

`Dockerfile`:

```dockerfile
# Multi-stage build
FROM clang:19 AS builder

# 의존성 설치
RUN apt-get update && apt-get install -y \
    cmake ninja-build \
    libhighway-dev libnuma-dev liblz4-dev \
    liburing-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# 클라우드 최적화 빌드
RUN mkdir -p build && cd build && \
    cmake .. \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-march=x86-64-v3 -O3" \
        -DAPEX_CLOUD_NATIVE=ON \
        -DAPEX_USE_HUGEPAGES=OFF \
        -DAPEX_USE_RDMA=OFF && \
    ninja -j$(nproc)

# Runtime 이미지
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libhighway1 libnuma1 liblz4-1 liburing2 \
    curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/apex-db
COPY --from=builder /build/build/apex_server .
COPY --from=builder /build/build/*.so .

# Non-root 사용자
RUN useradd -r -u 1000 apex && \
    chown -R apex:apex /opt/apex-db

USER apex
EXPOSE 8123

# Health check
HEALTHCHECK --interval=10s --timeout=3s --start-period=30s \
    CMD curl -f http://localhost:8123/health || exit 1

ENTRYPOINT ["./apex_server"]
CMD ["--port", "8123"]
```

빌드:

```bash
docker build -t apex-db:latest .
docker tag apex-db:latest your-registry/apex-db:v1.0.0
docker push your-registry/apex-db:v1.0.0
```

#### 2. Kubernetes 배포

`k8s/deployment.yaml`:

```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: apex-db
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: apex-config
  namespace: apex-db
data:
  apex.conf: |
    port: 8123
    worker_threads: 8
    analytics_mode: true
    cloud_native: true
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: apex-db
  namespace: apex-db
spec:
  replicas: 3
  selector:
    matchLabels:
      app: apex-db
  template:
    metadata:
      labels:
        app: apex-db
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
                  - apex-db
              topologyKey: kubernetes.io/hostname
      containers:
      - name: apex-db
        image: your-registry/apex-db:v1.0.0
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
          mountPath: /opt/apex-db/config
        - name: data
          mountPath: /opt/apex-db/data
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
          name: apex-config
      - name: data
        persistentVolumeClaim:
          claimName: apex-db-pvc
---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: apex-db-pvc
  namespace: apex-db
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
  name: apex-db-service
  namespace: apex-db
spec:
  type: LoadBalancer
  selector:
    app: apex-db
  ports:
  - port: 8123
    targetPort: 8123
    protocol: TCP
    name: http
---
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: apex-db-hpa
  namespace: apex-db
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: apex-db
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

배포:

```bash
kubectl apply -f k8s/deployment.yaml

# 확인
kubectl get pods -n apex-db
kubectl get svc -n apex-db

# 로그 확인
kubectl logs -f deployment/apex-db -n apex-db
```

#### 3. Helm Chart (권장)

`helm/apex-db/values.yaml`:

```yaml
replicaCount: 3

image:
  repository: your-registry/apex-db
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

설치:

```bash
helm install apex-db ./helm/apex-db \
    --namespace apex-db \
    --create-namespace \
    --values values-prod.yaml
```

#### 4. 모니터링 (Prometheus + Grafana)

`k8s/monitoring.yaml`:

```yaml
apiVersion: v1
kind: ServiceMonitor
metadata:
  name: apex-db-metrics
  namespace: apex-db
spec:
  selector:
    matchLabels:
      app: apex-db
  endpoints:
  - port: http
    path: /metrics
    interval: 15s
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: grafana-dashboard
  namespace: apex-db
data:
  apex-db.json: |
    {
      "dashboard": {
        "title": "APEX-DB Analytics",
        "panels": [
          {
            "title": "Query Latency (p95)",
            "targets": [{"expr": "histogram_quantile(0.95, apex_query_duration_seconds)"}]
          },
          {
            "title": "Throughput (queries/sec)",
            "targets": [{"expr": "rate(apex_queries_total[1m])"}]
          },
          {
            "title": "Memory Usage",
            "targets": [{"expr": "apex_memory_bytes / 1024 / 1024 / 1024"}]
          }
        ]
      }
    }
```

### 클라우드 벤치마크 (예상)

| 메트릭 | 클라우드 (c6i.4xlarge) | 목표 |
|--------|----------------------|------|
| filter 1M | **350μs** | < 500μs |
| VWAP 1M | **650μs** | < 800μs |
| 인제스션 | **4.5M ticks/sec** | > 4M |
| p99 레이턴시 | **800μs** | < 1ms |
| 처리량 | **5K queries/sec** | > 3K |

---

## 📊 비교표: 베어메탈 vs 클라우드

| 항목 | 베어메탈 | 클라우드 | 차이 |
|------|----------|---------|------|
| **초기 비용** | 높음 (서버 구매) | 낮음 (시간당 과금) | 10x |
| **운영 복잡도** | 높음 (수동 튜닝) | 낮음 (자동화) | 3x |
| **레이턴시 p50** | **250μs** | 350μs | +40% |
| **레이턴시 p99** | **550μs** | 800μs | +45% |
| **Jitter** | **±5μs** | ±30μs | 6x worse |
| **처리량** | **6M/sec** | 4.5M/sec | -25% |
| **확장성** | 수동 (서버 추가) | 자동 (HPA) | Auto |
| **비용 (연간)** | $100K (고정) | $50K~$200K (가변) | 사용량 의존 |
| **적합 워크로드** | HFT, 실시간 | 분석, 백테스트 | - |

---

## 🚀 마이그레이션 경로

### Phase 1: 개발/테스트 (클라우드)
```
개발 → Docker 로컬 테스트 → K8s 스테이징 → 검증
```

### Phase 2: 프로덕션 (선택)

**Option A: 하이브리드**
- HFT: 베어메탈 (온프레미스)
- 백테스트: 클라우드 (AWS)

**Option B: 풀 클라우드**
- 모든 워크로드: K8s
- 레이턴시 critical: 전용 노드 (c6i.metal)

**Option C: 풀 베어메탈**
- 모든 워크로드: 온프레미스
- 관리 복잡도 높음

---

## 📋 체크리스트

### 베어메탈 배포 전
- [ ] 하드웨어 스펙 확인 (CPU, RAM, Network)
- [ ] 커널 파라미터 설정 (`isolcpus`, `hugepages`)
- [ ] 튜닝 스크립트 실행 (`tune_bare_metal.sh`)
- [ ] NUMA 토폴로지 확인
- [ ] 벤치마크 실행 (목표 레이턴시 달성 확인)

### 클라우드 배포 전
- [ ] Docker 이미지 빌드 및 푸시
- [ ] K8s 클러스터 준비
- [ ] Persistent Volume 설정
- [ ] Monitoring 설정 (Prometheus, Grafana)
- [ ] Auto-scaling 정책 설정

---

## 📞 지원

- **베어메탈 배포 지원**: enterprise@apex-db.io
- **클라우드 배포 지원**: cloud@apex-db.io
- **커뮤니티**: https://discord.gg/apex-db

---

**다음 문서:**
- [베어메탈 튜닝 상세 가이드](BARE_METAL_TUNING.md)
- [Kubernetes 운영 가이드](KUBERNETES_OPS.md)
- [모니터링 & Alerting](MONITORING.md)
