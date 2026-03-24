# Phase C: 분산 메모리 & 클러스터 아키텍처 설계서

> Cloud-Native 수평 확장, Transport 추상화(RDMA→CXL 교체 가능), K8s 없는 경량 Control Plane

---

## 1. 아키텍처 개요

```
┌─────────────────────────────────────────────────┐
│            APEX Control Plane (단일 바이너리)      │
│                                                   │
│  ┌──────────┐ ┌───────────┐ ┌────────────────┐  │
│  │ Fleet    │ │ Metadata  │ │ Health         │  │
│  │ Manager  │ │ Store     │ │ Monitor        │  │
│  │(EC2 Fleet│ │(DynamoDB) │ │(Heartbeat +    │  │
│  │ API)     │ │           │ │ Failover)      │  │
│  └──────────┘ └───────────┘ └────────────────┘  │
│  ┌──────────┐ ┌───────────┐                      │
│  │ Partition│ │ Metrics   │                      │
│  │ Router   │ │ Exporter  │                      │
│  │(Consist. │ │(Prometheus│                      │
│  │ Hashing) │ │ format)   │                      │
│  └──────────┘ └───────────┘                      │
└───────────────────┬─────────────────────────────┘
                    │ 관리 (gRPC / REST)
                    │
   ┌────────────────┼────────────────────┐
   │                │                    │
┌──┴───┐  ┌────────┴─────┐  ┌──────────┴──┐
│ Node1│←→│    Node2     │←→│    Node3    │  Data Plane
│ APEX │  │    APEX      │  │    APEX     │  (EFA/RDMA 직접)
│ DB   │  │    DB        │  │    DB       │
└──────┘  └──────────────┘  └─────────────┘
   ↑              ↑                ↑
   └──── Placement Group (CLUSTER) ────┘
         같은 AZ, 같은 랙 → 최저 레이턴시
```

---

## 2. Transport 추상화 레이어

### 2-A. 인터페이스 설계 (모듈 교체 가능)

```cpp
// 컴파일 타임 디스패치 — virtual call 오버헤드 제로
template <typename Impl>
class TransportBackend {
public:
    // 원격 노드에 메모리 영역 등록
    RemoteRegion register_memory(void* addr, size_t size);

    // One-sided RDMA write (원격 노드 CPU 개입 없음)
    void remote_write(const void* local, RemoteRegion remote, size_t offset, size_t size);

    // One-sided RDMA read
    void remote_read(RemoteRegion remote, size_t offset, void* local, size_t size);

    // 메모리 펜스 (순서 보장)
    void fence();

    // 노드 연결/해제
    ConnectionId connect(const NodeAddress& addr);
    void disconnect(ConnectionId conn);
};
```

### 2-B. 백엔드 구현체

| 백엔드 | 용도 | 레이턴시 |
|---|---|---|
| `UCXBackend` | 프로덕션 — RDMA/AWS EFA/InfiniBand | ~1-15μs |
| `CXLBackend` | 차세대 — CXL 3.0 메모리 시맨틱 | ~150-300ns |
| `SharedMemBackend` | 개발/테스트 — 단일 머신 POSIX shm | ~100ns |
| `TCPBackend` | 폴백 — RDMA 미지원 환경 | ~50-100μs |

### 2-C. CXL 전환 시 변경 범위

```cpp
// 현재 (RDMA)
using ProductionTransport = TransportBackend<UCXBackend>;

// 미래 (CXL 3.0) — 이 한 줄만 바꾸면 됨
using ProductionTransport = TransportBackend<CXLBackend>;
```

CXL에서는 `remote_write/read`가 내부적으로 단순 `memcpy`가 됨.
하드웨어가 캐시 코히런시를 보장하므로 `fence()`도 `std::atomic_thread_fence`로 충분.

---

## 3. Control Plane 설계

### 3-A. Fleet Manager (EC2 Fleet API 기반)

```cpp
struct FleetConfig {
    // EFA 지원 인스턴스만
    std::vector<std::string> instance_types = {"r7i.8xlarge", "r8g.8xlarge"};

    // Placement Group — 같은 랙 배치
    std::string placement_group = "zepto-cluster";
    PlacementStrategy strategy = PlacementStrategy::CLUSTER;

    // Warm Pool — 미리 대기 인스턴스
    size_t warm_pool_size = 2;

    // Capacity Reservation
    CapacityMode capacity_mode = CapacityMode::ON_DEMAND_RESERVED;
};

class FleetManager {
    // 즉시 노드 추가 (warm pool에서 → 수 초)
    NodeId launch_node();

    // Graceful 종료 (파티션 이전 → 종료)
    void drain_and_terminate(NodeId id);

    // Warm pool 유지 (부팅 완료, ZeptoDB 대기 상태)
    void maintain_warm_pool();

    // 현재 클러스터 상태
    ClusterTopology topology() const;
};
```

### 3-B. Metadata Store (DynamoDB)

```
테이블: zepto-cluster-metadata

PK: "partition#{symbol_id}#{hour_epoch}"
SK: "assignment"
Attributes:
  - node_id: "node-abc123"
  - state: ACTIVE | MIGRATING | SEALED
  - arena_usage_pct: 45.2
  - created_at: 1711065600

테이블: zepto-cluster-nodes

PK: "node#{node_id}"
Attributes:
  - address: "10.0.1.5:9000"
  - state: JOINING | ACTIVE | SUSPECT | DEAD | LEAVING
  - last_heartbeat: 1711065612
  - instance_type: "r7i.8xlarge"
  - partitions_count: 42
```

왜 DynamoDB?
- 서버리스 → 운영 부담 제로
- 단일 자릿수 ms 레이턴시 (메타데이터 접근은 콜드 패스)
- 자동 복제 + 고가용성

### 3-C. Health Monitor (Heartbeat + Failover)

```cpp
struct HealthConfig {
    uint32_t heartbeat_interval_ms = 1000;   // 1초마다
    uint32_t suspect_timeout_ms = 3000;      // 3초 무응답 → SUSPECT
    uint32_t dead_timeout_ms = 10000;        // 10초 → DEAD
    uint32_t failover_grace_ms = 5000;       // 파티션 이전 유예
};

// 상태 전이
// ACTIVE → (3s 무응답) → SUSPECT → (7s 추가) → DEAD → failover 트리거
// SUSPECT 상태에서 heartbeat 재개 → ACTIVE 복귀
```

Failover 절차:
1. Node DEAD 판정
2. 해당 노드의 파티션 목록 조회 (DynamoDB)
3. Consistent Hash Ring의 다음 노드에 파티션 재할당
4. Warm Pool 노드 활성화하여 데이터 복구 (HDB에서 로드)

### 3-D. Partition Router (Consistent Hashing)

```cpp
class PartitionRouter {
    // Symbol → Node 라우팅 (O(1) 로컬 해시 테이블)
    NodeId route(SymbolId symbol) const;

    // 노드 추가 — 최소 파티션만 이동
    MigrationPlan add_node(NodeId new_node);

    // 노드 제거 — 파티션 시계방향 다음으로
    MigrationPlan remove_node(NodeId failed_node);

    // Virtual nodes로 균등 분배
    // 물리 노드 1개 = 가상 노드 128개 → 데이터 균등 분배
    static constexpr size_t VIRTUAL_NODES_PER_PHYSICAL = 128;
};
```

---

## 4. Data Plane 설계

### 4-A. Distributed Arena (글로벌 메모리 풀)

```cpp
template <typename Transport>
class DistributedArena {
    Transport transport_;
    LocalArena local_arena_;           // 로컬 메모리 (기존 ArenaAllocator)
    RemoteRegion registered_region_;    // Transport에 등록된 영역

    // 로컬 할당 (핫 패스 — 기존과 동일)
    void* allocate_local(size_t size);

    // 원격 노드가 이 아레나에 직접 쓰기 허용 (RDMA one-sided)
    RemoteRegion expose();
};
```

### 4-B. 분산 인제스션 흐름

```
Client Tick → PartitionRouter.route(symbol)
                    │
            ┌───────┴───────┐
            │ 로컬 노드?     │
            ├── YES ────────→ 로컬 Ring Buffer → 로컬 RDB
            │
            └── NO ─────────→ Transport.remote_write()
                              → 원격 노드 Ring Buffer (zero-copy)
```

### 4-C. 분산 쿼리 흐름

```
Client Query(VWAP, symbol=AAPL)
    → PartitionRouter: "AAPL은 Node2에"
    → Node2에 쿼리 요청 (gRPC)
    → Node2 로컬 실행 (SIMD 벡터화)
    → 결과 반환

// 시간 범위가 여러 파티션에 걸칠 경우:
Client Query(VWAP, symbol=AAPL, range=24h)
    → 각 노드에 파티션별 부분 쿼리 병렬 전송
    → 부분 결과 수집 (partial VWAP: Σpv, Σv)
    → 클라이언트에서 최종 합산
```

---

## 5. 스케일링 시나리오

### 스케일 아웃 (노드 추가)
```
1. FleetManager: warm pool에서 노드 활성화 (수 초)
2. 새 노드 → Control Plane에 JOINING 등록
3. PartitionRouter: consistent hash에 추가
4. MigrationPlan 생성 → 이관 대상 파티션 목록
5. 원본 노드 → 새 노드로 파티션 데이터 RDMA 전송
6. DynamoDB 메타데이터 업데이트
7. 새 노드 ACTIVE → 트래픽 수신 시작
```

### 스케일 인 (노드 제거)
```
1. 대상 노드 LEAVING 마킹
2. 파티션 → consistent hash 다음 노드로 이전
3. 이전 완료 확인 후 terminate
4. FleetManager: warm pool 보충
```

### 장애 복구
```
1. Heartbeat 실패 → SUSPECT (3s) → DEAD (10s)
2. 해당 노드 파티션 → 다음 노드에 재할당
3. RDB 데이터 손실분 → HDB(S3/NVMe)에서 복구
4. WAL 리플레이로 최신 데이터 복원
5. Warm pool 노드 활성화
```

---

## 6. 기술 스택

| 컴포넌트 | 기술 |
|---|---|
| Transport (현재) | UCX → RDMA/AWS EFA |
| Transport (미래) | CXL 3.0 (모듈 교체) |
| Metadata | DynamoDB (서버리스) |
| 노드 관리 | EC2 Fleet API + Warm Pool |
| 네트워크 배치 | Placement Group (CLUSTER) |
| 노드 간 RPC | gRPC (관리용) / RDMA (데이터용) |
| HDB Cold Storage | S3 |
| 모니터링 | Prometheus exporter → CloudWatch/Grafana |
| 설정 | S3 JSON 또는 DynamoDB |

---

## 7. 구현 순서

### Phase C-1: Transport 추상화
- `TransportBackend` 인터페이스
- `SharedMemBackend` (테스트용)
- `UCXBackend` (프로덕션)
- 기존 ArenaAllocator → DistributedArena 확장

### Phase C-2: 클러스터 코어
- `PartitionRouter` (consistent hashing)
- `HealthMonitor` (heartbeat)
- `ClusterNode` (노드 프로세스)
- 로컬 2-노드 테스트 (SharedMem)

### Phase C-3: AWS 통합
- `FleetManager` (EC2 Fleet API)
- DynamoDB 메타데이터
- Placement Group 설정
- EFA 실제 테스트

### Phase C-4: 분산 쿼리
- scatter-gather 쿼리 실행
- 부분 결과 합산
- 멀티노드 벤치마크

---

## 8. 핵심 설계 원칙

1. **핫 패스에 간접 호출 없음** — 템플릿 디스패치, 인라인
2. **Control Plane ≠ Data Plane** — 관리는 느려도 됨, 데이터는 μs
3. **Transport 교체 = 1줄 변경** — RDMA → CXL 마이그레이션 무고통
4. **K8s 없음** — Fleet API + DynamoDB로 충분
5. **Warm Pool** — 노드 추가 수 초, 부팅 대기 없음
6. **Consistent Hashing** — 노드 변경 시 최소 파티션 이동
