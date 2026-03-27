# 031 — CoordinatorHA ↔ K8sLease/FencingToken 통합

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: CoordinatorHA ↔ K8sLease 통합

---

## 배경

CoordinatorHA가 K8sLease/FencingToken을 사용하지 않아 네트워크 파티션 시
Active coordinator가 2개 동시 존재 가능. Stale coordinator의 write가 거부되지 않음.

## 변경 내용

### coordinator_ha.h
- `CoordinatorHAConfig`에 `require_lease`, `LeaseConfig` 추가
- `FencingToken fencing_token_` 멤버 추가
- `std::unique_ptr<K8sLease> lease_` 멤버 추가
- `fencing_token()`, `epoch()`, `lease()` 접근자 추가
- `try_promote()` private 메서드 추출

### coordinator_ha.cpp

**init()**:
- `require_lease=true` 시 K8sLease 생성 + on_elected/on_lost 콜백 등록
- ACTIVE 초기화 시 즉시 `fencing_token_.advance()` + peer RPC에 epoch 설정

**start()**:
- K8sLease 시작 (lease 경쟁 참여)

**try_promote()** (신규):
- Lease 필요 시 `lease_->try_acquire()` 확인 — 실패 시 promote 거부
- `fencing_token_.advance()` — epoch bump
- `peer_rpc_->set_epoch(new_epoch)` — RPC client에 epoch 전파
- 기존 노드 re-registration + promotion callback

**monitor_loop()**:
- Promote 경로를 `try_promote()`로 위임
- Lease 획득 실패 시 재시도 (break 대신 last_pong 리셋)

**on_lost 콜백**:
- Lease 상실 시 자동 demote (ACTIVE → STANDBY)

## Split-brain 방지 흐름

```
Coordinator A (epoch=5) ──X── network partition
Coordinator B: lease acquired → try_promote()
  → fencing_token_.advance() → epoch=6
  → peer_rpc_.set_epoch(6)

Coordinator A recovers, tries write with epoch=5
  → Data node: FencingToken::validate(5) fails (last_seen=6)
  → REJECTED
```

## 하위 호환성

- `require_lease=false` (기본값) → 기존 동작 그대로 (ping 기반 failover)
- `require_lease=true` → 프로덕션 모드 (lease + fencing)
- 기존 테스트 796개 전체 통과 (regression 없음)
