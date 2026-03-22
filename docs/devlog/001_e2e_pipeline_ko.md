# Devlog #001: E2E Integration Pipeline

**날짜:** 2026-03-21
**작성:** 고생이 (AI Assistant)

---

## 개요

Layer 1 (Storage), Layer 2 (Ingestion), Layer 3 (Execution)을 하나로 잇는
**엔드투엔드 파이프라인**과 **벤치마크 프레임워크**를 구현했다.

## 구현 내용

### Part 1: ApexPipeline (src/core/pipeline.cpp)

아키텍처:
```
외부 → ingest_tick() → TickPlant (MPMC Queue)
                           ↓
                     [drain_thread]
                           ↓
                     store_tick() → PartitionManager → ColumnVectors
                                                          ↓
                     query_vwap() / query_filter_sum() ---↙
                         → VectorizedEngine (벡터화 연산)
```

핵심 설계 결정:
- **드레인 스레드 분리:** 인제스트(TickPlant enqueue)와 저장(ColumnStore append)을 분리하여
  인제스트 경로의 레이턴시를 최소화
- **파티션 인덱스:** `unordered_map<SymbolId, vector<Partition*>>`로 쿼리 시 O(1) 파티션 lookup
- **동기 드레인 모드:** 테스트/벤치용 `drain_sync()` — 백그라운드 스레드 없이 즉시 저장

### Part 2: 벤치마크 프레임워크 (tests/bench/)

chrono 기반 자체 벤치마크 — Google Benchmark 없이 가볍게 구현.
측정 항목:
1. 인제스션 처리량 (batch 크기별)
2. 쿼리 p50/p99/p999 (VWAP, Filter+Sum, Count)
3. E2E 레이턴시 (ingest → store → query)
4. 멀티스레드 동시 인제스션 (1/2/4 threads)
5. 대용량 VWAP (10M rows)

### Part 3: CMake 통합

- `apex_core` 라이브러리 추가 (apex_storage + apex_ingestion + apex_execution 통합)
- `bench_pipeline` 실행 파일 추가
- `-O3 -march=native` 최적화 플래그

## 벤치마크 결과 하이라이트

| 항목 | 결과 |
|------|------|
| 인제스션 | **5.4M ticks/sec** (동기 ingest+store) |
| VWAP 1M rows | **740μs** (p50) |
| VWAP 5M rows | **3.7ms** (p50) |
| E2E p99 | **66μs** (100K rows 상태) |
| 스캔 처리량 | **~1.3B rows/sec** |

## 발견된 이슈 & 개선점

### 1. Arena 메모리 낭비
ColumnVector의 doubling 전략이 arena에서 이전 블록을 해제하지 못해 공간 낭비 발생.
10M rows 적재 시 1.5GB arena로도 ~9M만 저장 가능 (arena exhaustion).

**해결안:** arena-aware realloc 구현 또는 slab allocator 전환

### 2. MPMC Queue 오버플로우
64K 큐에 멀티스레드로 동시 인제스트 시 burst drop 발생.
드레인 스레드 속도 < 인제스트 속도일 때 문제.

**해결안:** back-pressure 메커니즘 또는 큐 사이즈 확대 (256K+)

### 3. HugePages 미활성화
EC2 환경에서 HugePages mmap 실패 → regular pages fallback.
대용량 데이터에서 TLB miss 증가.

**해결안:** `sudo sysctl vm.nr_hugepages=1024` 설정

## 다음 단계

- [ ] Highway SIMD 실제 적용 (filter_gt_i64, sum_i64, vwap)
- [ ] 파티션 병렬 쿼리 (std::execution::par)
- [ ] WAL 연동 (crash recovery)
- [ ] q 언어 파서 연결 (Layer 4)
- [ ] kdb+ IPC 호환 레이어

---

*"MVP 완성. 숫자가 나왔으니 이제 최적화 게임 시작."*
