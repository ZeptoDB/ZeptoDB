# kdb+ / KDB-X & ClickHouse 벤치마크 레퍼런스

> APEX-DB와 비교하기 위한 kdb+ 및 경쟁 DB 벤치마크 데이터 모음
> 마지막 업데이트: 2026-03-22

---

## 1. kdb+ / KDB-X 공식 벤치마크

### 1-A. KDB-X vs TSBS 벤치마크 (2025년 KX 공식 블로그)

**출처:** [KX Blog — KDB-X vs QuestDB, ClickHouse, TimescaleDB, InfluxDB](https://kx.com/blog/benchmarking-kdb-x-vs-questdb-clickhouse-timescaledb-and-influxdb-with-tsbs/)

**환경:** 256 코어, 2.2TB RAM (KDB-X는 4 스레드, 16GB 메모리로 제한)

**결과 (1년치 저빈도 데이터 기준, KDB-X 대비 느린 배수):**

| 쿼리 | QuestDB | InfluxDB | TimescaleDB | ClickHouse |
|---|---|---|---|---|
| single-groupby-1-1-1 | 16.2x | 48.1x | 119.9x | **9,791.9x** |
| single-groupby-1-1-12 | 25.9x | 39.7x | 528.2x | 5,741.8x |
| cpu-max-all-1 | 14.1x | 23.3x | 127.2x | 425.9x |
| high-cpu-1 | 8.3x | 2.4x | 519.3x | 443.7x |
| double-groupby-1 | 0.7x | 11.9x | 4.9x | 21.0x |
| lastpoint | 0.8x | 7,069.8x | 17.4x | 112.3x |
| **기하평균** | **4.2x** | **53.1x** | **25.5x** | **161.3x** |

**핵심 발견:**
- KDB-X는 64개 시나리오 중 **58개에서 1위** (4 스레드만 사용하고도)
- ClickHouse는 일부 쿼리에서 **10,000배 느림**
- QuestDB가 가장 경쟁력 있지만 평균 4.2배 느림
- KDB-X의 q 언어 벡터 연산은 여전히 업계 최고 수준

### 1-B. kdb+ Tick Plant 프로파일링 (KX 공식 화이트페이퍼)

**출처:** [kdb+tick profiling](https://code.kx.com/q/wp/tick-profiling/)

**환경:** 64비트 Linux, 8 CPU, kdb+ 3.1

**Tickerplant 처리 성능 (마이크로초):**

| rows/update | rows/sec | TP 로그 쓰기 | TP 발행 | RDB 수신 | RDB 삽입 | TP CPU |
|---|---|---|---|---|---|---|
| 1 | 10,000 | 14μs | 3μs | 71μs | 4μs | 31% |
| 10 | 100,000 | 15μs | 4μs | 82μs | 7μs | 32% |
| 100 | 100,000 | 32μs | 6μs | 103μs | 46μs | 6% |
| 100 | 500,000 | 28μs | 6μs | 105μs | 42μs | 32% |

**핵심 발견:**
- 단일 행 전송: ~30K rows/sec에서 CPU 100%
- 10행 배치: 100K rows/sec 가능 (벡터 연산 효율)
- 100행 배치: 500K rows/sec 가능 (CPU 32%)
- **kdb+ tickerplant 이론 최대치: ~2-5M rows/sec** (배치 크기, 하드웨어 따라)

### 1-C. kdb+ 쿼리 최적화 성능 (KX 공식 화이트페이퍼)

**출처:** [kdb+ query scaling](https://code.kx.com/q/wp/query-scaling/)

**환경:** kdb+ 3.1, 인메모리 2M 행 / 파티션 10M 행

| 연산 | 인메모리 (2M rows) | 파티션 (10M rows) |
|---|---|---|
| select by sym | **20ms** | **78ms** |
| select last per sym | 51ms | 345ms |
| select first per sym | 12ms | - |
| max aggregation per sym | 28ms | - |
| filter (3 syms, lambda each) | - | **15ms** |
| filter (3 syms, in operator) | - | 25ms |

**참고:** 이 수치는 "쿼리 전체"이며, 단순 벡터 연산(sum/filter) 단독 레이턴시가 아님.

---

## 2. kdb+ vs APEX-DB 직접 비교

### 인제스션 비교

| 메트릭 | kdb+ tickerplant | APEX-DB | 비고 |
|---|---|---|---|
| 단일행 처리 | ~30K/sec (CPU 100%) | 4.97M/sec | **165x 우위** (kdb+는 q 인터프리터 오버헤드) |
| 배치 100행 | ~500K/sec (CPU 32%) | 5.52M/sec | **11x 우위** |
| 이론 최대 | ~2-5M/sec | 5.52M/sec | **동등~우위** |

> **주의:** kdb+ tickerplant 수치는 2014년 하드웨어 기준. 최신 하드웨어에서는 더 높을 수 있음.
> KDB-X(2025)는 멀티스레드 지원으로 크게 개선됐을 가능성 있으나, 공개 인제스션 벤치마크 미확인.

### 쿼리 비교 (인메모리 OLAP)

| 연산 | kdb+ (추정) | APEX-DB Scalar | APEX-DB SIMD | 갭 |
|---|---|---|---|---|
| VWAP 1M rows | ~200-500μs | 649μs | 532μs | ⚠️ 1.1~2.5x 열세 |
| sum 1M rows | ~50-150μs | 267μs | 264μs | ⚠️ ~2x 열세 |
| filter 1M rows | ~100-300μs | 3,550μs | 1,358μs | ❌ 4~13x 열세 |

**열세 원인 분석:**
1. **kdb+ q 언어는 컬럼 벡터 연산 네이티브** — 30년 이상 최적화된 인터프리터
2. **APEX-DB filter는 SelectionVector 기록 오버헤드** — bitmask 방식으로 개선 예정
3. **APEX-DB sum은 이미 auto-vectorize** — 추가 개선 여지 적음 (메모리 대역폭 bound)
4. **APEX-DB 파티셔닝 오버헤드** — 단일 파티션 내 직접 스캔이 아닌 인덱스 탐색 비용

---

## 3. ClickHouse 벤치마크 레퍼런스

### ClickBench (공식 벤치마크 프레임워크)

**출처:** [benchmark.clickhouse.com](https://benchmark.clickhouse.com/)

ClickHouse는 **디스크 기반 OLAP**에 특화. 인메모리 실시간 HFT와는 직접 비교 부적절하지만 참고:

**ClickHouse 특성:**
- 벡터화 실행 엔진 (APEX-DB Layer 3과 유사한 아키텍처)
- MergeTree 스토리지 (APEX-DB DMMT에 영감)
- 디스크 기반 → 인메모리 대비 쿼리 레이턴시 10~1000x 느림
- TSBS 벤치마크에서 kdb+ 대비 **기하평균 161x 느림**

**ClickHouse가 잘하는 것:**
- 대규모 배치 인제스션 (수억 행/초)
- 디스크 기반 압축 + 스캔 (비용 효율)
- SQL 호환성, 에코시스템

**ClickHouse가 못하는 것:**
- 실시간 μs 레이턴시 쿼리 (우리 목표)
- 틱 단위 스트리밍 인제스션
- 인메모리 성능 (kdb+, APEX-DB 도메인)

### APEX-DB vs ClickHouse 포지셔닝

| 차원 | ClickHouse | APEX-DB |
|---|---|---|
| 타겟 | 범용 OLAP | HFT 실시간 |
| 스토리지 | 디스크 기반 | 인메모리 (CXL) |
| 레이턴시 | ms~sec | **μs** |
| 인제스션 | 배치 최적화 | 스트리밍 최적화 |
| 쿼리 언어 | SQL | C++ DAG / Python DSL |
| 경쟁 상대 | Snowflake, BigQuery | kdb+, custom HFT systems |

---

## 4. 결론 및 APEX-DB 목표 수치

### 최종 목표 (Phase B 최적화 후)

| 메트릭 | kdb+ (최신 추정) | APEX-DB 목표 | 전략 |
|---|---|---|---|
| 인제스션 | ~5M/sec | **10M+/sec** | RDMA 직접 쓰기, sharded drain |
| VWAP 1M | ~200-500μs | **<200μs** | SIMD fused pipeline |
| sum 1M | ~50-150μs | **<100μs** | 멀티컬럼 fusion, prefetch |
| filter 1M | ~100-300μs | **<200μs** | bitmask SIMD, branch-free |
| 동적 쿼리 | q 인터프리터 | **JIT SIMD** | LLVM AVX2/512 emit |
| Python 연동 | PyKX (IPC 기반) | **Zero-copy** | nanobind + Arrow |

### TSBS 대비 목표
- 단일 쿼리: kdb+ 대비 **1:1 동등 이상**
- 멀티스레드 쿼리: kdb+ 대비 **2~4x 우위** (kdb+의 단일 스레드 한계 돌파)
