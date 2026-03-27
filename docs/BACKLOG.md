# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 726 tests passing
>
> Recent: ✅ String column (dictionary-encoded) | ✅ Repo restructure (`deploy/`, `.ai/`, CMakePresets)

---

## P1 — "보여줄 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Web UI / Admin Console** | curl 데모는 임팩트 제로. 투자자/고객 미팅에서 화면 필수 | M |
| ↳ Query editor + result table | SQL 입력 → 테이블/차트 출력 | |
| ↳ Cluster status dashboard | 노드 수, 파티션, ingestion rate | |
| ↳ Table schema browser | CREATE TABLE 결과 확인 | |

Web UI 없으면 데모 불가. `web/` 폴더, React+Vite, `localhost:8123`에서 static serve.

---

## P2 — "찾을 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Website (zeptodb.io)** | GitHub README만으로는 신뢰도 부족 | S |
| ↳ Landing page | 벤치마크 숫자가 핵심 셀링 포인트 | |
| ↳ Docs site (docs.zeptodb.io) | mkdocs 이미 있음, 배포만 하면 됨 | |

---

## P3 — "Python 퀀트가 쓸 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Arrow Flight server** | Pandas/Polars에서 `flight://` 직접 연결, 원격 zero-copy급 | M |

현재 zero-copy는 C++ 임베딩에서만 동작. Arrow Flight이면 원격 Python 클라이언트도 가능.

---

## P4 — 기존 도구 연결

| Task | Why | Effort |
|------|-----|--------|
| **ClickHouse wire protocol** | DBeaver, DataGrip, Grafana 네이티브 연결 | L |
| **JDBC/ODBC drivers** | Tableau, Excel, Power BI | L |

---

## P5 — 데이터 파이프라인

| Task | Why | Effort |
|------|-----|--------|
| **Kafka Connect Sink** | 엔터프라이즈 데이터 파이프라인 표준 | M |
| **CDC connector** | PostgreSQL/MySQL → 실시간 싱크 | M |
| **AWS Kinesis consumer** | AWS 네이티브 스트리밍 | S |
| **Apache Pulsar consumer** | Kafka 대안 | S |

---

## P6 — 엔터프라이즈 / 클라우드

| Task | Why | Effort |
|------|-----|--------|
| **Cloud Marketplace** | AWS/GCP 원클릭 배포 | M |
| **Vault-backed API Key Store** | 프로덕션 시크릿 관리 | S |
| **Geo-replication** | 멀티 리전, 글로벌 트레이딩 데스크 | L |

---

## P7 — 성능 / 엔진

| Task | Why | Effort |
|------|-----|--------|
| **JIT SIMD emit** | AVX2/512 벡터 IR from LLVM JIT | L |
| **DuckDB embedding** | 복잡한 JOIN을 Arrow zero-copy로 위임 | M |
| **Bare-metal tuning guide** | CPU pinning, NUMA, io_uring 상세 가이드 | S |
| **Limited DSL AOT compilation** | Nuitka/Cython → 단일 바이너리 | M |

---

## P8 — 클러스터 안정성 (엔터프라이즈 필수)

> ⚠️ 프로덕션 배포 전 반드시 해결. 현재 상태로는 데이터 유실/서비스 중단 위험.

### P8-Critical — Split-Brain / 데이터 유실 방지

| Task | 현재 문제 | 필요 조치 | Effort |
|------|----------|----------|--------|
| **PartitionRouter 분산 동기화** | 각 ClusterNode, QueryCoordinator, ComputeNode가 독립 router 사본 보유. ring 변경 시 동기화 메커니즘 없어 노드 간 라우팅 불일치 발생 | 중앙 집중형(coordinator 단일 권한) 또는 분산 합의형(Raft 기반 ring 동기화) 중 택 1 | L |
| **CoordinatorHA ↔ K8sLease 통합** | CoordinatorHA가 K8sLease/FencingToken을 사용하지 않음. 네트워크 파티션 시 Active 2개 동시 존재 가능 | promote 경로에서 K8sLease 획득 필수화, FencingToken::advance() 호출 + 전체 RPC client에 epoch 전파 | M |
| **WalReplicator 복제 보장** | 기본 async, 큐 초과 시 데이터 drop, 실패 시 재시도 없음 | Quorum write (W=2) 옵션, 실패 재시도 큐, 백프레셔 메커니즘 | M |
| **Failover 데이터 복구** | router에서 즉시 제거하지만 미복제 데이터 복구 절차 없음. 콜백 미등록 시 데이터 조용히 유실 | WAL 기반 데이터 복구를 failover 필수 단계로 포함, re-replication 자동 트리거 | M |
| **내부 RPC 보안** | 클러스터 내부 TCP RPC가 평문. 인증/인가 없음 | mTLS + 내부 인증 토큰 적용 | M |

### P8-High — 운영 안정성

| Task | 현재 문제 | 필요 조치 | Effort |
|------|----------|----------|--------|
| **HealthMonitor DEAD 복구** | DEAD 상태 노드가 복구되어도 영원히 DEAD. 재합류 경로 없음 | Rejoin protocol + 데이터 재동기화 메커니즘 | M |
| **HealthMonitor UDP 내결함성** | UDP 패킷 손실만으로 DEAD 판정 가능. 소켓 bind 실패 시 무시 | 다중 실패 확인 (3연속), bind 실패 시 fatal error, 보조 TCP heartbeat | S |
| **TcpRpcServer 리소스 관리** | thread-per-connection + detach, payload 크기 무제한, shutdown 시 1초 후 포기 | 스레드 풀, payload 상한 (예: 64MB), graceful drain 모드 | M |
| **PartitionRouter 동시성** | ring/node_set에 자체 lock 없음. FailoverManager가 lock 없이 remove_node() 호출 | Reader-writer lock 내장, TOCTOU 제거 | S |
| **TcpRpcClient::ping() 연결 누수** | 풀 미사용, 500ms마다 새 TCP 연결 생성/파괴 | acquire()/release() 사용으로 변경 | S |

### P8-Medium — 일관성 / 마이그레이션

| Task | 현재 문제 | 필요 조치 | Effort |
|------|----------|----------|--------|
| **PartitionMigrator 원자성** | 중간 실패 시 부분 이동 상태, 재개 불가 | 상태 머신 + 체크포인트 + 롤백 메커니즘 | L |
| **SnapshotCoordinator 일관성** | 글로벌 배리어 없이 병렬 실행, 시점 불일치 | 2PC 기반 (prepare → commit), 인제스트 일시 중단 또는 LSN 컷오프 | M |
| **K8sNodeRegistry 실제 구현** | poll_loop()가 빈 루프, 수동 register에만 의존 | K8s Endpoints API watch 구현 | M |
| **ClusterNode 노드 재합류** | seed 전체 실패해도 "참가" 처리, peer 맵 동기화 없음 | seed 연결 최소 1개 필수, peer_rpc_clients_ 동기화 | S |
| **GossipNodeRegistry data race** | `running_`이 std::atomic이 아닌 bool | std::atomic<bool>로 변경 | S |
| **K8sNodeRegistry 데드락** | fire_event_unlocked()가 lock 보유 상태에서 콜백 호출 | 콜백을 lock 해제 후 호출하도록 수정 | S |

### P8-Feature — 분산 기능 확장

| Task | Why | Effort |
|------|-----|--------|
| **Live rebalancing** | 무중단 파티션 이동 | L |
| **Tier C cold query offload** | 최근 → in-memory, 과거 → DuckDB on S3 | M |
| **PTP clock sync detection** | ASOF JOIN strict mode | S |
| **Global symbol registry** | 분산 환경에서 string symbol Tier A 직접 라우팅 | M |
| **Multi-node benchmark execution** | EKS 가이드 준비됨, ~$12/run | S |

---

## P9 — Physical AI / Industry

| Task | Why | Effort |
|------|-----|--------|
| **MQTT ingestion** | IoT 디바이스 | S |
| **OPC-UA connector** | Siemens S7, 산업 PLC | M |
| **ROS2 plugin** | ROS2 topics → ZeptoDB | M |

---

## P10 — 확장 / 장기

| Task | Why | Effort |
|------|-----|--------|
| **User-Defined Functions** | Python/WASM UDF, 커스텀 알파 시그널 | L |
| **Pluggable partition strategy** | symbol_affinity / hash_mod / site_id | M |
| **Edge mode** (`--mode edge`) | 단일 노드 + 비동기 클라우드 싱크 | M |
| **HyperLogLog** | 분산 approximate COUNT DISTINCT | S |
| **Variable-length strings** | 로그, 코멘트 등 free-text | M |
| HDB Compaction | Parquet merge (Glue/Spark 외부) | S |
| Snowflake/Delta Lake hybrid | | M |
| Graph index (CSR) | 자금 흐름 추적 | L |
| InfluxDB migration | InfluxQL → SQL | S |

---

**핵심 경로: P1(Web UI) → P2(Website) → P3(Arrow Flight)**

이 3개가 "발견 → 시도 → 사용"의 최소 경로.
