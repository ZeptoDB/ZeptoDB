# devlog #011 — 병렬 쿼리 엔진: QueryScheduler 추상화 + 멀티노드 확장 경로

**날짜**: 2026-03-22
**작업**: 병렬 쿼리 실행 엔진 + QueryScheduler DI 패턴

---

## 배경 및 목표

> "지금 당장 멀티노드를 구현하는 게 아니라,
>  나중에 QueryExecutor 코드 수정 없이 노드만 추가해도 되는 구조를 잡는다."

기존 `QueryExecutor`에는 직렬 경로만 있었다. 멀티코어 집계는 분명히 필요하지만,
나중에 분산 실행으로 확장할 때 executor 내부를 전면 재작성하지 않으려면
**실행 위치(어디서 실행하는지)** 를 추상화해야 한다.

---

## 설계: scatter/gather + DI 패턴

### QueryScheduler 인터페이스

```
QueryExecutor
    │
    └─► QueryScheduler (abstract)
            ├── LocalQueryScheduler    ← 오늘 구현
            └── DistributedQueryScheduler ← stub (UCX 기반, 향후)
```

```cpp
class QueryScheduler {
    virtual vector<PartialAggResult> scatter(const vector<QueryFragment>&) = 0;
    virtual PartialAggResult         gather(vector<PartialAggResult>&&)    = 0;
    virtual size_t      worker_count()   const = 0;
    virtual string      scheduler_type() const = 0;
};
```

- **scatter**: 작업 단위(Fragment)를 워커에게 분배
- **gather**: 부분 결과를 병합
- QueryExecutor는 인터페이스만 보고, 실행 위치를 모른다

### Dependency Injection

```cpp
// 기본: LocalQueryScheduler 자동 생성
QueryExecutor ex(pipeline);

// 테스트/분산용: 외부 스케줄러 주입
QueryExecutor ex(pipeline, std::make_unique<MockScheduler>());
```

`pool_raw_` raw pointer 패턴: 내부 병렬 경로(`exec_agg_parallel` 등)는
`QueryExecutor` private 메서드(`eval_where`, `get_col_data`)를 호출해야 하므로
완전한 scatter/gather 분리보다 `WorkerPool*` non-owning pointer를 사용.
비-로컬 스케줄러 주입 시 `nullptr` → 직렬 폴백.

---

## 구현 파일

| 파일 | 역할 |
|------|------|
| `include/apex/execution/query_scheduler.h` | QueryFragment + PartialAggResult + 인터페이스 |
| `include/apex/execution/local_scheduler.h` | LocalQueryScheduler 선언 |
| `src/execution/local_scheduler.cpp` | scatter/gather/execute_fragment 구현 |
| `include/apex/execution/distributed_scheduler.h` | 분산 스케줄러 stub |
| `src/execution/distributed_scheduler.cpp` | stub |
| `include/apex/execution/parallel_scan.h` | ParallelScanExecutor (partition chunk 분배) |
| `src/execution/parallel_scan.cpp` | make_partition_chunks / select_mode |
| `src/sql/executor.cpp` | parallel 경로 + DI 생성자 |

### PartialAggResult: 재귀 타입 문제 해결

GROUP BY 집계를 분산하려면 부분 결과가 `group_key → 부분집계` 맵을 가져야 한다.
이 맵의 값이 `PartialAggResult` 자신이므로 incomplete type 컴파일 오류 발생.

```cpp
// 오류: std::unordered_map<int64_t, PartialAggResult> (incomplete type)
// 해결:
std::unordered_map<int64_t, std::shared_ptr<PartialAggResult>> group_partials;
```

---

## 병렬화 전략 (ParallelScanExecutor::select_mode)

```
total_rows < threshold(100K)      → SERIAL   (스레드 오버헤드 > 이득)
num_partitions >= num_threads     → PARTITION (파티션 단위 분배)
otherwise                         → CHUNKED   (단일 파티션 내 행 분할)
```

단, `num_threads <= 1`이면 항상 SERIAL. 이 조건을 `exec_agg`/`exec_group_agg` 진입 시
먼저 검사하지 않으면 `exec_group_agg` → `exec_group_agg_parallel` → SERIAL → `exec_group_agg` 무한재귀 발생.

**버그 수정**: 병렬 진입 조건에 `pool_raw_->num_threads() > 1` 추가.

---

## 벤치마크 결과 (1M rows, 2 symbols)

| 쿼리 | 1T | 2T | 4T | 8T |
|------|-----|-----|-----|-----|
| GROUP BY symbol sum(volume) | 0.862ms | 0.460ms (1.87x) | 0.398ms (2.16x) | 0.248ms (3.48x) |
| sum(volume) WHERE symbol=1 | 0.006ms | — | — | — |
| count(*) | 0.004ms | — | — | — |

- 단일 파티션 쿼리(WHERE symbol=X)는 `partitions.size() < 2` → 자동 직렬 경로
- GROUP BY (전체 파티션)에서 의미있는 멀티코어 가속 확인
- scatter/gather API 직접 호출: 평균 0.033ms/round (10회 평균)

---

## 테스트 (27개 추가)

- Part 1-4: WorkerPool 기본, ParallelScan 유틸, 병렬 집계 정확성, 다중 스레드 수 정확성
- Part 5: 직렬 폴백 (소량 데이터)
- Part 6: QueryScheduler DI 테스트 (MockScheduler 주입, 타입 검사, scatter/gather 직접 호출)

---

## 단일노드 → 멀티노드 확장 경로

```
현재 (단일 노드)
  QueryExecutor
    └─► LocalQueryScheduler
           └─► WorkerPool (jthread, N cores)

향후 (멀티 노드, UCX)
  QueryExecutor           ← 코드 변경 없음
    └─► DistributedQueryScheduler
           ├─► UCX transport (node A)
           ├─► UCX transport (node B)
           └─► ...
           scatter: PartialAggResult를 각 노드에 Fragment로 직렬화 전송
           gather:  각 노드 결과를 PartialAggResult::merge()로 병합
```

`PartialAggResult::serialize()` / `deserialize()` stub은 분산 스케줄러 구현 시 채운다.
UCX backend (`src/cluster/ucx_backend.cpp`)는 이미 Phase C에서 구현됨.

---

## 다음 단계

- [ ] UCX transport 위에 DistributedQueryScheduler 실제 구현
- [ ] PartialAggResult FlatBuffers 직렬화
- [ ] CHUNKED 모드 (단일 대형 파티션 행 분할) 활성화
- [ ] exec_simple_select 병렬화 (현재는 집계만 병렬)
