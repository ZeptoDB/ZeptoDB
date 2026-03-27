# 035 — HealthMonitor DEAD 복구 + UDP 내결함성

**Date**: 2026-03-27
**Status**: Completed
**P8-High**: HealthMonitor DEAD 복구, HealthMonitor UDP 내결함성

---

## 배경

두 가지 운영 안정성 문제:

1. **DEAD 복구 불가**: DEAD 상태 노드가 복구되어도 영원히 DEAD. 재합류 경로 없음.
2. **UDP 내결함성 부족**: UDP 패킷 손실만으로 DEAD 판정 가능. 소켓 bind 실패 시 무시.

## 변경 내용

### 1. DEAD 복구 — REJOINING 상태 + Rejoin Protocol

`health_monitor.h`:
- `NodeState::REJOINING` (= 6) 추가
- DEAD 노드가 heartbeat 재수신 시: DEAD → REJOINING → (재동기화) → ACTIVE
- `on_rejoin(RejoinCallback)` — 재동기화 콜백 등록. true 반환 시 ACTIVE 복귀, false 시 REJOINING 유지 후 다음 heartbeat에서 재시도
- `node_state_str()` 에 REJOINING 추가

`cluster_node.h`:
- `on_node_state_change()`: REJOINING → ACTIVE 전이 시에도 라우터에 노드 재추가 (기존 JOINING → ACTIVE와 동일 경로)

### 2. UDP 내결함성 — 3연속 miss + bind fatal + TCP heartbeat

`HealthConfig` 신규 필드:
- `tcp_heartbeat_port` (기본 9101) — 보조 TCP heartbeat 포트
- `consecutive_misses_for_suspect` (기본 3) — SUSPECT 전이 전 연속 miss 횟수
- `enable_dead_rejoin` (기본 true) — DEAD 복구 활성화
- `fatal_on_bind_failure` (기본 true) — UDP bind 실패 시 예외 발생

`HealthMonitor` 변경:
- `consecutive_misses_` 맵 추가 — 노드별 연속 heartbeat miss 카운트
- `check_timeouts()`: miss 카운트를 경과 시간 비례로 계산 후 SUSPECT 판정
- `setup_udp_socket()`: bind 실패 시 `std::runtime_error` 발생 (fatal_on_bind_failure=true)
- `tcp_recv_loop()`: TCP heartbeat 수신 스레드 — UDP 손실 보완
- `tcp_probe()`: SUSPECT → DEAD 전이 전 TCP connect probe — 실패 시에만 DEAD 확정
- `inject_heartbeat()`: miss 카운트 리셋, DEAD 상태에서 rejoin 처리

### 소켓 변경

- `sock_` → `udp_sock_` + `tcp_sock_` (UDP/TCP 분리)
- `send_thread_`, `recv_thread_`, `check_thread_` + `tcp_recv_thread_` (4 스레드)
- `stop()`: joinable 체크 후 join (안전한 종료)

## 테스트

- 기존 `HealthMonitor.StateTransitions` — 통과 (하위 호환)
- 기존 `HealthMonitor.GetActiveNodes` — 통과
- 기존 `Failover.HealthMonitorIntegration_DeadTriggersFailover` — 통과
- 전체 796 테스트 통과

## 파일 변경

| 파일 | 변경 |
|------|------|
| `include/zeptodb/cluster/health_monitor.h` | REJOINING 상태, consecutive miss, TCP heartbeat, bind fatal, rejoin protocol |
| `include/zeptodb/cluster/cluster_node.h` | REJOINING→ACTIVE 전이 시 라우터 재추가 |
| `docs/BACKLOG.md` | P8-High 2개 항목 완료 표시 |
