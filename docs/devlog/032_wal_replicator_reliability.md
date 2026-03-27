# 032 — WalReplicator 복제 보장 (Quorum / Retry / Backpressure)

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: WalReplicator 복제 보장

---

## 배경

기존 WalReplicator는 async best-effort:
- 큐 초과 시 데이터 drop (silent)
- Send 실패 시 재시도 없음 (에러 카운트만)
- 복수 replica 중 일부 실패해도 구분 없음

## 변경 내용

### ReplicatorConfig 추가 필드

| 필드 | 기본값 | 설명 |
|------|--------|------|
| `quorum_w` | 0 | 최소 ACK 수 (0 = 전체 replica) |
| `retry_queue_capacity` | 64K | 재시도 큐 크기 |
| `retry_interval_ms` | 100 | 재시도 간격 |
| `max_retries` | 3 | 최대 재시도 횟수 |
| `backpressure` | false | true = 큐 가득 차면 producer block |
| `backpressure_timeout_ms` | 500 | block 최대 대기 시간 |

### ReplicatorStats 추가 필드

| 필드 | 설명 |
|------|------|
| `retried` | 재시도 성공 batch 수 |
| `retry_exhausted` | max_retries 초과로 drop된 batch 수 |
| `backpressured` | 백프레셔로 block된 enqueue 호출 수 |

### 구현

**Quorum write**: `flush_batch()`에서 각 replica에 전송 후 ACK 수 집계.
`ack_count >= quorum_w`이면 성공. quorum_w=0이면 전체 성공 필요.

**재시도 큐**: send 실패 batch를 `retry_queue_`에 보관.
`send_loop()` 매 iteration마다 `process_retry_queue()` 호출.
attempts < max_retries면 재시도, 초과 시 drop + 로그.

**백프레셔**: `backpressure=true`일 때 큐 가득 차면 `cv_.wait_for()`로
producer를 block. `send_loop()`에서 batch swap 후 `cv_.notify_all()`로 해제.
timeout 초과 시 drop.

### 하위 호환성

기본값이 모두 기존 동작과 동일:
- `quorum_w=0` → 전체 replica 시도 (기존과 동일)
- `max_retries=3` → 재시도 추가 (기존: 0)
- `backpressure=false` → 큐 초과 시 drop (기존과 동일)

## 테스트

기존 796개 테스트 전체 통과. ReplicaDown 테스트에서 retry 후 drop 로그 확인.
