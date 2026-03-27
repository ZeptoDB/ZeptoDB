# ZeptoDB 멀티노드 운영 안정성 설계

**Date**: 2026-03-27
**Status**: P8-Critical/High 완료, P8-Medium 진행 중

---

## 개요

ZeptoDB 분산 클러스터의 프로덕션 운영 안정성을 위해 해결한 문제들과 설계 결정을 정리한 문서.
Split-brain, 데이터 유실, 리소스 고갈, 동시성 버그, 장애 복구 등 멀티노드 환경에서 발생 가능한
모든 안정성 이슈를 다룬다.

### 아키텍처 컨텍스트

```
┌─────────────────────────────────────────────────────────┐
│                    Control Plane                         │
│  CoordinatorHA (Active/Standby) + K8sLease + Fencing    │
│  QueryCoordinator (scatter-gather routing)               │
│  RingConsensus (epoch broadcast → 전 노드 ring 동기화)   │
└────────────────────┬────────────────────────────────────┘
                     │ TcpRpc (HMAC auth)
     ┌───────────────┼───────────────┐
     ▼               ▼               ▼
┌─────────┐   ┌─────────┐   ┌─────────┐
│ DataNode │   │ DataNode │   │ DataNode │
│ Pipeline │   │ Pipeline │   │ Pipeline │
│ WAL+Rep  │   │ WAL+Rep  │   │ WAL+Rep  │
│ Health   │   │ Health   │   │ Health   │
└─────────┘   └─────────┘   └─────────┘
     ↕ UDP heartbeat + TCP probe ↕
```

---

## 1. Split-Brain / 데이터 유실 방지 (P8-Critical ✅)

### 1.1 PartitionRouter 분산 동기화

**문제**: 각 노드가 독립적인 PartitionRouter 사본을 보유. ring 변경(노드 추가/제거) 시
동기화 메커니즘이 없어 노드 간 라우팅 불일치 발생. 같은 symbol이 서로 다른 노드로 라우팅되어
데이터 분산 저장 → 쿼리 결과 불일치.

**해결**: `RingConsensus` 인터페이스 + `EpochBroadcastConsensus` 구현.

```
Coordinator: propose_add(node_id)
  → epoch++ → serialize(ring snapshot + epoch)
  → broadcast to all followers via TcpRpc RING_UPDATE
  → followers: apply_update() → epoch 검증 → ring 교체

Follower: RING_UPDATE 수신
  → epoch > local_epoch? → ring 교체 + cache 무효화
  → epoch <= local_epoch? → 무시 (stale)
```

- Coordinator만 ring 변경 권한 보유 (`is_coordinator=true`)
- Follower는 RPC 콜백으로 ring update 수신/적용
- Raft 구현체로 교체 가능한 플러그인 구조 (`RingConsensus` 인터페이스)

**관련 코드**: `include/zeptodb/cluster/ring_consensus.h`
**Devlog**: `docs/devlog/030_ring_consensus.md`

### 1.2 CoordinatorHA — K8sLease 기반 Split-Brain 방지

**문제**: Active coordinator 2개가 동시 존재 가능. 네트워크 파티션 시 Standby가
Active 장애로 오판하고 promote → 두 coordinator가 동시에 write 수행.

**해결**: K8sLease + FencingToken 이중 방어.

```
Standby: Active ping 실패 (2초)
  → try_promote()
  → require_lease=true? → K8sLease::try_acquire() 필수
  → 성공: FencingToken::advance() → epoch bump
  → peer_rpc_.set_epoch(new_epoch)
  → ACTIVE 전환

Stale Active (epoch=5) recovers:
  → write 시도 (TICK_INGEST, epoch=5)
  → DataNode: FencingToken::validate(5) → last_seen=6 → REJECTED
```

- `require_lease=false` (기본): ping 기반 failover (테스트/개발)
- `require_lease=true` (프로덕션): K8sLease 획득 필수
- Lease 상실 시 자동 demote (ACTIVE → STANDBY)
- 현재 Active 1 + Standby 1 구성

**관련 코드**: `include/zeptodb/cluster/coordinator_ha.h`, `src/cluster/coordinator_ha.cpp`
**Devlog**: `docs/devlog/031_coordinator_ha_lease.md`

### 1.3 WAL 복제 보장

**문제**: WalReplicator가 기본 async 모드에서 큐 초과 시 데이터 drop, 실패 시 재시도 없음.
노드 장애 시 미복제 데이터 유실.

**해결**: 3가지 복제 모드 + 재시도 + 백프레셔.

| 모드 | 동작 | 용도 |
|------|------|------|
| `ASYNC` | fire-and-forget, 큐 초과 시 drop | 개발/테스트 |
| `SYNC` | 모든 replica ACK 대기 | 최대 내구성 |
| `QUORUM` | W=N 중 과반 ACK 대기 | 프로덕션 권장 |

- 실패 재시도 큐: `max_retries=3`, 지수 백오프
- 백프레셔: 큐 포화 시 producer block (ingest 속도 자동 조절)
- 기존 async/sync 모드 하위 호환

**관련 코드**: `include/zeptodb/ingestion/wal_replicator.h`
**Devlog**: `docs/devlog/032_wal_replicator_reliability.md`

### 1.4 Failover 데이터 복구

**문제**: 노드 장애 시 router에서 즉시 제거하지만, 미복제 데이터 복구 절차 없음.
콜백 미등록 시 데이터가 조용히 유실.

**해결**: re-replication을 failover 필수 단계로 포함.

```
Node DEAD 감지
  → FailoverManager::handle_failure()
  → 1. router.remove_node(dead_id)
  → 2. auto_re_replicate=true?
       → PartitionMigrator: replica → new_replica 데이터 복제
       → 비동기/동기 선택 가능
  → 3. on_failover 콜백 호출
```

- `auto_re_replicate` 설정으로 자동 re-replication 활성화
- PartitionMigrator 내장, 비동기/동기 선택 가능
- 노드 미등록 시 graceful fallback (로그 경고 후 계속)

**관련 코드**: `include/zeptodb/cluster/failover_manager.h`
**Devlog**: `docs/devlog/033_failover_data_recovery.md`

### 1.5 내부 RPC 보안

**문제**: 클러스터 내부 TCP RPC가 평문. 인증/인가 없음. 네트워크 접근 가능한 누구나
SQL 실행, 데이터 주입 가능.

**해결**: Shared-secret HMAC 인증 프로토콜.

```
Client → Server: AUTH_HANDSHAKE (nonce + HMAC-SHA256(shared_secret, nonce))
Server: HMAC 검증 → AUTH_OK / AUTH_REJECT
  → 실패 시 연결 즉시 거부
  → 성공 후 일반 RPC 메시지 허용
```

- `RpcSecurityConfig::enabled=true` + `shared_secret` 설정
- 새 연결마다 자동 handshake (TcpRpcClient::acquire())
- mTLS 설정 구조 준비 (`cert_path`/`key_path`/`ca_cert_path`)

**관련 코드**: `include/zeptodb/cluster/rpc_security.h`
**Devlog**: `docs/devlog/034_rpc_security.md`

---

## 2. 장애 감지 및 복구 (P8-High ✅)

### 2.1 HealthMonitor — DEAD 노드 복구

**문제**: DEAD 상태 노드가 복구되어도 영원히 DEAD. 재합류 경로 없음.
운영자가 수동으로 클러스터 재구성 필요.

**해결**: `REJOINING` 상태 추가.

```
상태 전이:
  DEAD → REJOINING (heartbeat 재수신 시)
  REJOINING → ACTIVE (on_rejoin 콜백 true 반환 시)
  REJOINING → REJOINING (콜백 false → 다음 heartbeat에서 재시도)
```

- `on_rejoin()` 콜백으로 데이터 재동기화 제어
- ClusterNode에서 REJOINING→ACTIVE 전이 시 라우터 자동 재추가

### 2.2 HealthMonitor — UDP 내결함성

**문제**: UDP 패킷 1개 손실만으로 DEAD 판정 가능. 소켓 bind 실패 시 무시.

**해결**: 3중 방어.

| 방어 계층 | 메커니즘 |
|-----------|----------|
| 연속 miss 확인 | `consecutive_misses_for_suspect=3` — 3회 연속 miss 후에만 SUSPECT |
| TCP 이중 확인 | SUSPECT→DEAD 전이 전 TCP probe (port 9101)로 재확인 |
| Bind 실패 감지 | `fatal_on_bind_failure=true` — UDP bind 실패 시 예외 발생 |

**관련 코드**: `include/zeptodb/cluster/health_monitor.h`
**Devlog**: `docs/devlog/035_health_monitor_resilience.md`

---

## 3. RPC 서버 리소스 관리 (P8-High ✅)

TcpRpcServer의 4가지 리소스 관리 문제를 해결.

### 3.1 스레드 풀

**문제**: 연결마다 `std::thread(...).detach()` — 스레드 수 무제한, OOM 위험.

**해결**: 고정 크기 워커 스레드 풀 + 작업 큐.

```
accept_loop(): accept() → conn_queue_.push(fd) → notify_one()
worker_loop(): queue_cv_.wait() → pop(fd) → handle_connection(fd) → close(fd)
```

- `set_thread_pool_size(n)` — 기본 `hardware_concurrency`, 최소 4
- 풀 포화 시 큐에 대기 (자연스러운 backpressure)
- `stop()` 시 모든 워커 join 가능 (detach 제거)

### 3.2 Payload 크기 상한

**문제**: `payload_len` 검증 없이 `std::vector` 할당 — 4GB payload 헤더로 OOM 공격 가능.

**해결**: `max_payload_size_` 검증 (기본 64MB).

- `handle_connection()` 메인 루프 + auth handshake 양쪽에서 검증
- 초과 시 연결 즉시 종료 + `ZEPTO_WARN` 로그
- `set_max_payload_size(bytes)` API로 런타임 설정

### 3.3 Graceful Drain

**문제**: `stop()`이 1초 후 active 연결 포기. 진행 중 쿼리 결과 유실.

**해결**: 5단계 graceful shutdown.

```
stop():
  ① listen socket close (새 연결 거부)
  ② pending queue drain (대기 중 fd close)
  ③ drain_timeout_ms 동안 in-flight 요청 완료 대기 (기본 30초)
  ④ 타임아웃 시 shutdown(SHUT_RDWR) 강제 종료
  ⑤ 워커 join (정상) 또는 detach (강제)
```

- `set_drain_timeout_ms(ms)` API로 설정 가능
- 롤링 업그레이드 시 진행 중 쿼리 보호

### 3.4 동시 연결 수 제한

**문제**: `active_conns_` 카운트만 하고 상한 없음.

**해결**: `max_connections_` 설정 (기본 1024).

- `accept_loop()`에서 초과 시 즉시 `close(cfd)` + 경고 로그
- `set_max_connections(n)` API로 런타임 설정

**관련 코드**: `include/zeptodb/cluster/tcp_rpc.h`, `src/cluster/tcp_rpc.cpp`

---

## 4. 라우팅 및 연결 안정성 (P8-High ✅)

### 4.1 PartitionRouter 동시성

**문제**: `ring_`/`node_set_`에 자체 lock 없음. FailoverManager가 lock 없이
`remove_node()` 호출 → data race, TOCTOU 버그.

**해결**: `ring_mutex_` (`std::shared_mutex`) 내장.

| 연산 | Lock 타입 |
|------|-----------|
| `add_node`, `remove_node` | `unique_lock` (writer) |
| `route`, `route_replica`, `node_count`, `all_nodes`, `plan_*` | `shared_lock` (reader) |
| copy constructor, `operator=` | 양쪽 lock 보호 |

- 외부 `router_mutex_` 없이도 안전한 동시 접근
- 캐시(`cache_mutex_`)는 기존 별도 mutex 유지

### 4.2 TcpRpcClient::ping() 연결 누수

**문제**: `ping()`이 매번 `connect_to_server()` + `close()` — 500ms마다 새 TCP 연결
생성/파괴. 커널 TIME_WAIT 소켓 누적.

**해결**: `acquire()`/`release()` 사용으로 연결 풀 재활용.

```
// Before (매번 새 연결)
int fd = connect_to_server();
... ping ...
close(fd);

// After (풀 재활용)
int fd = acquire();      // 풀에서 꺼내거나 새로 생성
... ping ...
release(fd, ok);         // 성공 시 풀에 반환, 실패 시 close
```

**관련 코드**: `src/cluster/tcp_rpc.cpp`

---

## 5. 노드 레지스트리 안정성 (P8-Medium ✅ 일부)

### 5.1 GossipNodeRegistry data race

**문제**: `running_`이 `bool` — 멀티스레드에서 동시 읽기/쓰기 시 UB.

**해결**: `std::atomic<bool> running_{false}`.

### 5.2 K8sNodeRegistry 데드락

**문제**: `fire_event_unlocked()`가 `mu_` lock 보유 상태에서 콜백 호출.
콜백 내에서 `active_nodes()` 등 호출 시 데드락.

**해결**: lock 해제 후 콜백 호출.

```
// Before (데드락)
void register_node(addr) {
    lock_guard lock(mu_);
    nodes_[id] = ...;
    fire_event_unlocked(id, JOINED);  // mu_ 보유 상태에서 콜백!
}

// After (안전)
void register_node(addr) {
    bool is_new;
    { lock_guard lock(mu_); is_new = ...; nodes_[id] = ...; }
    if (is_new) fire_event(id, JOINED);  // lock 해제 후 콜백
}
```

### 5.3 ClusterNode 노드 재합류 검증

**문제**: seed 전체 연결 실패해도 "참가" 처리. peer 맵 동기화 없음.

**해결**: seed 연결 성공 카운트 + 전체 실패 시 예외.

- Transport connect + RPC client 생성 성공 여부로 `seed_connected` 카운트
- `seed_connected == 0` (seeds 비어있지 않은 경우): router/transport 정리 후 `std::runtime_error`
- Bootstrap (seeds 없음): 정상 허용 (첫 번째 노드)

**관련 코드**: `include/zeptodb/cluster/node_registry.h`, `include/zeptodb/cluster/cluster_node.h`

---

## 6. 미완료 항목 (P8-Medium 잔여)

| Task | 현재 문제 | 필요 조치 | Effort |
|------|----------|----------|--------|
| **SnapshotCoordinator 일관성** | 글로벌 배리어 없이 병렬 실행, 시점 불일치 | 2PC (prepare → commit), 인제스트 일시 중단 또는 LSN 컷오프 | M |
| **K8sNodeRegistry 실제 구현** | poll_loop()가 빈 루프 | K8s Endpoints API watch 구현 | M |
| **PartitionMigrator 원자성** | 중간 실패 시 부분 이동, 재개 불가 | 상태 머신 + 체크포인트 + 롤백 | L |

---

## 7. 설정 요약

프로덕션 배포 시 권장 설정:

```cpp
// Coordinator HA
CoordinatorHAConfig ha_cfg;
ha_cfg.require_lease = true;           // K8sLease 필수
ha_cfg.failover_after_ms = 2000;       // 2초 후 failover

// WAL 복제
WalReplicatorConfig wal_cfg;
wal_cfg.mode = ReplicationMode::QUORUM; // 과반 ACK
wal_cfg.max_retries = 3;

// RPC 서버
server.set_max_payload_size(64 * 1024 * 1024);  // 64MB
server.set_max_connections(1024);
server.set_thread_pool_size(0);                   // auto (hardware_concurrency)
server.set_drain_timeout_ms(30000);               // 30초

// RPC 보안
RpcSecurityConfig sec;
sec.enabled = true;
sec.shared_secret = "<cluster-secret>";

// Health Monitor
HealthConfig health;
health.consecutive_misses_for_suspect = 3;
health.fatal_on_bind_failure = true;
// TCP probe 자동 활성화 (port 9101)
```

---

## 8. 테스트 커버리지

| 영역 | 테스트 수 | 주요 시나리오 |
|------|-----------|--------------|
| RPC 서버 리소스 | 4 | payload 상한, 연결 제한, 스레드 풀 동시성, graceful drain |
| Split-brain 방지 | 2 | stale epoch tick/WAL 거부 (FencingRpc) |
| PartitionRouter 동시성 | 1 | 4 reader + 1 writer 동시 add/remove/route |
| ping() 풀 재활용 | 1 | 연속 ping 후 pool_idle_count 확인 |
| GossipNodeRegistry | 1 | atomic running_ 동시 읽기 |
| K8sNodeRegistry 데드락 | 1 | 콜백 내 registry 접근 시 데드락 없음 |
| ClusterNode seed 검증 | 2 | 전체 seed 실패 시 예외, bootstrap 성공 |
| CoordinatorHA | 7 | promote/demote, lease 통합, 이중 promote 방지 |
| WAL 복제 | 3 | quorum write, 재시도, 백프레셔 |
| Failover | 2 | auto re-replication, graceful fallback |

전체 테스트: 803+ passing
