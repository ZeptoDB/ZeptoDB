# 030 — RingConsensus: PartitionRouter 분산 동기화

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: PartitionRouter 분산 동기화

---

## 배경

각 ClusterNode, QueryCoordinator, ComputeNode가 독립적인 PartitionRouter 사본을 보유.
노드 추가/제거 시 ring 변경을 동기화하는 메커니즘이 없어 노드 간 라우팅 불일치 발생 가능.

## 설계 결정

**Epoch Broadcast** 방식 채택 (Raft 대신).

이유:
- Ring 변경은 드물게 발생 (노드 장애/추가 시에만)
- 상태가 작음 (노드 목록 수 KB)
- 이미 CoordinatorHA + FencingToken 인프라 존재
- ZeptoDB 타겟 도메인 (HFT, IoT, Observability)은 eventual consistency로 충분
- Raft 합의 지연이 틱 경로 성능을 저하시킬 수 있음

향후 은행/헬스케어 등 strong consistency 필요 시 RaftConsensus로 교체 가능하도록
인터페이스를 분리.

## 변경 파일

### 신규
- `include/zeptodb/cluster/ring_consensus.h`
  - `RingConsensus` — 추상 인터페이스 (propose_add, propose_remove, apply_update)
  - `RingSnapshot` — ring 상태 직렬화/역직렬화
  - `EpochBroadcastConsensus` — epoch broadcast 구현체

### 수정
- `include/zeptodb/cluster/rpc_protocol.h` — `RING_UPDATE(13)`, `RING_ACK(14)` 추가
- `include/zeptodb/cluster/tcp_rpc.h` — `RingUpdateCallback`, `set_ring_update_callback()`
- `src/cluster/tcp_rpc.cpp` — `RING_UPDATE` 핸들러 (apply → RING_ACK 응답)
- `include/zeptodb/cluster/cluster_node.h`
  - `ClusterConfig::is_coordinator` 플래그
  - `fencing_token_`, `consensus_` 멤버
  - `join_cluster()` — consensus 자동 초기화 + RPC 콜백 등록
  - `on_node_state_change()` — coordinator면 consensus 경유, follower면 로컬 직접 변경
  - `set_consensus()` — 외부 구현체 주입 (Raft-ready)

## 동작 흐름

```
Coordinator:
  on_node_state_change(ACTIVE) → consensus_->propose_add(id)
    → router_.add_node(id)
    → token_.advance() (epoch bump)
    → broadcast RING_UPDATE to all peers
    → peers apply_update() (epoch validate → router rebuild)

Follower:
  TcpRpcServer receives RING_UPDATE
    → ring_update_callback_ → consensus_->apply_update()
    → FencingToken::validate(epoch) — stale 거부
    → router 재구성
    → RING_ACK 응답
```

## 교체 경로

```cpp
// 기본 (EpochBroadcast)
node.join_cluster(seeds);

// Raft 도입 시
node.set_consensus(std::make_unique<RaftConsensus>(config));
node.join_cluster(seeds);
```

## 테스트

기존 796개 테스트 전체 통과 (regression 없음).
