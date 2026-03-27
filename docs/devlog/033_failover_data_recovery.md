# 033 — Failover 데이터 복구 (Auto Re-Replication)

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: Failover 데이터 복구

---

## 배경

기존 FailoverManager는 장애 노드를 router에서 즉시 제거하지만:
- 미복제 데이터 복구 절차 없음
- re-replication은 콜백에서 수동으로 해야 함
- 콜백 미등록 시 데이터 조용히 유실

## 변경 내용

### failover_manager.h

- `FailoverConfig` 추가: `auto_re_replicate` (기본 true), `async_re_replicate` (기본 true)
- `FailoverEvent`에 `re_replication_attempted`, `re_replication_succeeded` 필드 추가
- `PartitionMigrator migrator_` 내장 — 외부 의존 없이 자체 re-replication
- `register_node()` — migrator에 노드 RPC 주소 등록
- `re_replication_count()` — 성공한 re-replication 수 조회
- 비동기 re-replication 스레드 관리 (`async_threads_`)

### failover_manager.cpp

**trigger_failover()**:
1. Router/coordinator에서 장애 노드 제거 (기존)
2. Re-replication 대상 계산 (기존)
3. **신규**: migrator에 노드가 등록되어 있으면 자동 re-replication 실행
4. Re-replication 완료 후 콜백 실행 (비동기 시 스레드에서)
5. 노드 미등록 시 re-replication 건너뛰고 콜백 즉시 실행 (graceful fallback)

**run_re_replication()**:
- 각 ReReplicationTarget에 대해 `migrator_.migrate_symbol()` 호출
- 성공/실패 로그 + 통계 업데이트

### partition_migrator.h
- `has_node()` 추가 — 노드 등록 여부 확인

## Failover 흐름

```
Node 2 DEAD detected (HealthMonitor)
  → FailoverManager::trigger_failover(2)
    → router_.remove_node(2), coordinator_.remove_node(2)
    → re-replication targets: [{new_primary=1, new_replica=3}]
    → migrator has nodes? YES
      → async thread: migrate_symbol(1 → 3)
        → SELECT * FROM trades on node 1
        → replicate_wal() to node 3
        → callback fired with results
    → migrator has nodes? NO
      → callback fired immediately (graceful fallback)
```

## 하위 호환성

- 기존 테스트: `FailoverManager(router, coordinator)` 생성자 호환 (FailoverConfig 기본값)
- `register_node()` 미호출 시 기존 동작과 동일 (콜백만 실행)
- 796개 테스트 전체 통과
