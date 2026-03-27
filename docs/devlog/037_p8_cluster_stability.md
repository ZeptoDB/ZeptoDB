# 037 — P8 클러스터 운영 안정성 일괄 구현

**Date**: 2026-03-27
**Status**: Completed (P8-Critical/High 전체, P8-Medium 6/6)

---

## 배경

BACKLOG P8 섹션의 멀티노드 운영 안정성 항목들을 일괄 구현.
프로덕션 배포 전 필수 해결 사항으로, split-brain, 리소스 고갈, 동시성 버그,
장애 복구, 스냅샷 일관성 등을 다룸.

## 변경 내용

### P8-High: TcpRpcServer 리소스 관리 (4개)

**1. 스레드 풀 전환**
- `accept_loop()`의 `std::thread(...).detach()` → 고정 크기 워커 풀 + `std::queue<int>` 작업 큐
- `set_thread_pool_size()` API (기본 `hardware_concurrency`)
- 파일: `tcp_rpc.h`, `tcp_rpc.cpp`

**2. Payload 크기 상한**
- `max_payload_size_` (기본 64MB), `handle_connection()` + auth handshake에서 검증
- 초과 시 연결 종료 + `ZEPTO_WARN`
- 파일: `tcp_rpc.h`, `tcp_rpc.cpp`

**3. Graceful drain 모드**
- `stop()` 5단계: listen close → 큐 drain → drain 대기 (기본 30초) → 강제 shutdown → 워커 join/detach
- `set_drain_timeout_ms()` API
- 파일: `tcp_rpc.cpp`

**4. 동시 연결 수 제한**
- `max_connections_` (기본 1024), 초과 시 즉시 close
- 파일: `tcp_rpc.h`, `tcp_rpc.cpp`

### P8-High: PartitionRouter 동시성

- `ring_mutex_` (`std::shared_mutex`) 내장
- writer: `add_node`/`remove_node` → `unique_lock`
- reader: `route`/`route_replica`/`node_count`/`all_nodes`/`plan_*` → `shared_lock`
- copy constructor/assignment도 lock 보호
- 파일: `partition_router.h`

### P8-High: TcpRpcClient::ping() 연결 누수

- `connect_to_server()` + `close()` → `acquire()` + `release()` 풀 재활용
- 파일: `tcp_rpc.cpp`

### P8-Medium: GossipNodeRegistry data race

- `bool running_` → `std::atomic<bool> running_{false}`
- 파일: `node_registry.h`

### P8-Medium: K8sNodeRegistry 데드락

- `fire_event_unlocked()` 삭제
- `register_node()`/`deregister_node()`/`start()`에서 lock 해제 후 `fire_event()` 호출
- 파일: `node_registry.h`

### P8-Medium: ClusterNode 노드 재합류

- seed 연결 시 RPC client 생성 성공 여부로 카운트
- `seed_connected == 0`이면 router/transport 정리 후 `std::runtime_error`
- bootstrap (seeds 없음)은 정상 허용
- 파일: `cluster_node.h`

### P8-Medium: SnapshotCoordinator 일관성

- 단일 phase → 2PC (PREPARE → COMMIT/ABORT)
- Phase 1: 전 노드 `SNAPSHOT PREPARE <id>` (ingest 일시 중단)
- Phase 2: 전원 성공 시 `SNAPSHOT COMMIT <id>`, 실패 시 `SNAPSHOT ABORT <id>`
- `take_snapshot_legacy()` 하위 호환
- 파일: `snapshot_coordinator.h`, `snapshot_coordinator.cpp`

### P8-Medium: K8sNodeRegistry 실제 구현

- `poll_loop()`에서 K8s Endpoints API HTTP GET
- `KUBERNETES_SERVICE_HOST/PORT` 환경변수 자동 감지
- Service account token 인증
- `parse_endpoints_json()`: IP/port 추출, IP 해시로 stable NodeId
- `reconcile()`: 현재 노드 맵과 diff → JOINED/LEFT 이벤트
- K8s 외 환경: 수동 `register_node()` 폴백 유지
- 파일: `node_registry.h`

### P8-Medium: PartitionMigrator 원자성 (Phase A)

- `MoveState` 상태 머신: PENDING → DUAL_WRITE → COPYING → COMMITTED/FAILED
- `MigrationCheckpoint`: 각 move 상태 추적
- `resume_plan()`: FAILED move만 재시도 (max_retries=3)
- `execute_plan()` → `MigrationCheckpoint` 반환
- Phase B(디스크 체크포인트), Phase C(롤백)는 미구현
- 파일: `partition_migrator.h`, `partition_migrator.cpp`

## 문서

- `docs/design/multinode_stability.md` 신규 작성 (전체 안정성 설계 문서)
- `docs/BACKLOG.md` 완료 항목 업데이트
- `.kiro/KIRO.md` 문서 맵에 `multinode_stability.md` 추가

## 테스트

| 테스트 | 검증 내용 |
|--------|----------|
| `TcpRpcServerPayloadLimit.RejectsOversizedPayload` | 128B 제한, 256B 거부 |
| `TcpRpcServerMaxConnections.RejectsWhenFull` | 2개 제한, 3번째 거부 |
| `TcpRpcServerThreadPool.ConcurrentRequestsWithSmallPool` | 워커 2개, max concurrent ≤ 2 |
| `TcpRpcServerGracefulDrain.InFlightRequestCompletesBeforeStop` | 진행 중 쿼리 완료 보장 |
| `TcpRpcServerGracefulDrain.ForceCloseAfterTimeout` | 100ms 타임아웃 후 강제 종료 |
| `PartitionRouterConcurrency.ConcurrentAddRemoveRoute` | 4 reader + 1 writer 동시 |
| `TcpRpcClientPing.UsesConnectionPool` | ping 후 pool_idle_count=1 |
| `GossipNodeRegistryAtomic.RunningFlagIsAtomic` | atomic 동시 읽기 |
| `K8sNodeRegistryDeadlock.CallbackDuringRegisterDoesNotDeadlock` | 콜백 내 registry 접근 |
| `ClusterNodeSeedFailure.BootstrapWithNoSeedsSucceeds` | bootstrap 정상 |
| `ClusterNodeSeedFailure.PartialSeedConnectionSucceeds` | 부분 seed 성공 |
| `Snapshot.TwoPC_AbortOnPrepareFailure` | PREPARE 실패 → ABORT, COMMIT 0회 |
| `K8sNodeRegistryEndpoints.ParseEndpointsJson` | JSON 파싱 3개 노드 |
| `K8sNodeRegistryEndpoints.ReconcileDetectsJoinAndLeave` | JOINED/LEFT 이벤트 |
| `PartitionMigratorStateMachine.ResumeRetiesFailedMoves` | 1차 실패 → 리트라이 성공 |

전체 테스트: 803+ passing, regression 없음
