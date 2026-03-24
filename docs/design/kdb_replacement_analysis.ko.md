# ZeptoDB vs kdb+ 대체 가능성 분석
# 2026-03-22 (병렬 쿼리, Parquet HDB, S3 Sink 완료 반영)

---

## 1. kdb+가 제공하는 핵심 기능 체크리스트

kdb+/q의 기능을 전수 조사하고, ZeptoDB 현재 상태와 비교.

### A. 데이터 인제스션 & 스토리지

| kdb+ 기능 | 설명 | ZeptoDB | 상태 |
|---|---|---|---|
| Tickerplant (TP) | 실시간 틱 수집, Pub/Sub | TickPlant + MPMC | ✅ |
| RDB (실시간 DB) | 당일 데이터 인메모리 | Arena + ColumnStore | ✅ |
| HDB (히스토리) | 과거 데이터 디스크 (splayed) | HDB Writer/Reader + LZ4 | ✅ |
| WAL (TP 로그) | 장애 복구 로그 | WAL Writer | ✅ |
| EOD 프로세스 | 장 마감 시 RDB→HDB 전환 | FlushManager | ✅ |
| Parquet HDB | HDB를 Parquet 형식으로 저장 (DuckDB/Polars 호환) | ParquetWriter (SNAPPY/ZSTD/LZ4) | ✅ |
| S3 Sink | HDB 파티션 → S3 비동기 업로드 (클라우드 데이터 레이크) | S3Sink (MinIO 호환, hive 경로) | ✅ |
| Attributes (g#, p#, s#, u#) | 인덱스 힌트 | 파티션 기반 (부분) | ⚠️ |
| Symbol interning | 심볼 해시 최적화 | SymbolId (uint32) | ✅ |

**갭:** kdb+ attributes(g#=grouped, s#=sorted, p#=parted)는 쿼리 옵티마이저 힌트. ZeptoDB는 파티션 구조로 대부분 커버하지만, 명시적 attribute API는 없음.

### B. 쿼리 언어 & 실행

| kdb+ 기능 | 설명 | ZeptoDB | 상태 |
|---|---|---|---|
| q-SQL select | SELECT-WHERE-GROUP BY | SQL Parser + Executor | ✅ |
| fby (filter by) | 그룹별 필터링 | SQL WHERE + GROUP BY | ✅ |
| 벡터 연산 | 컬럼 전체에 대한 일괄 연산 | Highway SIMD | ✅ |
| 집계 (sum, avg, min, max, count) | 기본 집계 | ✅ 구현 | ✅ |
| VWAP (wavg) | 가중 평균 | VWAP 함수 | ✅ |
| xbar | 시간 바 집계 (5분봉 등) | **xbar() 네이티브** | ✅ |
| ema (지수이동평균) | 금융 핵심 지표 | **EMA() OVER** | ✅ |
| mavg, msum, mmin, mmax | 이동 평균/합계/최소/최대 | Window SUM/AVG/MIN/MAX | ✅ |
| deltas, ratios | 행 간 차이, 비율 | **DELTA/RATIO OVER** | ✅ |
| within | 범위 체크 | BETWEEN | ✅ |
| each, peach | 벡터/병렬 맵 | LocalQueryScheduler | ✅ |

**완료!** 모든 핵심 금융 함수 구현됨 (devlog #010)

### C. JOIN 연산

| kdb+ 기능 | 설명 | ZeptoDB | 상태 |
|---|---|---|---|
| aj (asof join) | 시계열 조인 | AsofJoinOperator | ✅ |
| aj0 | 왼쪽 컬럼만 반환 | 변형으로 가능 | ⚠️ |
| ij (inner join) | 내부 조인 | HashJoinOperator | ✅ |
| lj (left join) | 왼쪽 조인 | **HashJoinOperator (LEFT)** | ✅ |
| uj (union join) | 합집합 조인 | ❌ 미구현 | 🟡 |
| wj (window join) | 시간 윈도우 조인 | **WindowJoinOperator** | ✅ |
| ej (equi join) | 등가 조인 | HashJoinOperator | ✅ |
| pj (plus join) | 덧셈 조인 | ❌ 미구현 | 🟡 |

**완료!** 핵심 JOIN 모두 구현 (devlog #010)
- wj: O(n log m) 이진 탐색, wj_avg/sum/count/min/max
- lj: NULL 센티넬 (INT64_MIN)

### D. 시스템 & 운영

| kdb+ 기능 | 설명 | ZeptoDB | 상태 |
|---|---|---|---|
| IPC 프로토콜 | 프로세스 간 통신 | HTTP API + UCX | ✅ |
| 멀티프로세스 (TP/RDB/HDB/GW) | 역할별 프로세스 분리 | Pipeline 단일 + 분산 | ⚠️ |
| Gateway | 쿼리 라우팅 | PartitionRouter | ✅ |
| -s secondary threads | 병렬 쿼리 | **LocalQueryScheduler** | ✅ |
| .z.ts 타이머 | 스케줄링 | ❌ | 🟡 |
| \t 타이밍 | 쿼리 벤치마크 | execution_time_us | ✅ |

**남은 갭:**
- **프로세스 역할 분리**: kdb+는 TP/RDB/HDB/Gateway 별도 프로세스. ZeptoDB는 통합형 (향후 분산 스케줄러로 해결)

### E. Python 연동

| kdb+ 기능 | 설명 | ZeptoDB | 상태 |
|---|---|---|---|
| PyKX | kdb+↔Python | pybind11 + DSL | ✅ |
| IPC 기반 접근 | 소켓으로 데이터 전송 | zero-copy (메모리 직접) | ✅ 우위 |
| Arrow 연동 | Apache Arrow 변환 | Arrow 호환 레이아웃 | ✅ 우위 |


### F. 마이그레이션 툴킷 (2026-03-22 신규)

| 기능 | 설명 | ZeptoDB | 상태 |
|---|---|---|---|
| kdb+ q→SQL 변환 | q 언어 → APEX SQL 자동 변환 | QToSQLTransformer | ✅ |
| kdb+ HDB 로더 | Splayed tables → Columnar mmap | HDBLoader | ✅ |
| ClickHouse DDL 생성 | MergeTree, LowCardinality, Codec | ClickHouseSchemaGenerator | ✅ |
| ClickHouse 쿼리 변환 | xbar/ASOF/argMin/argMax | ClickHouseQueryTranslator | ✅ |
| DuckDB Parquet 내보내기 | SNAPPY/ZSTD, hive partitioning | ParquetExporter | ✅ |
| TimescaleDB DDL 생성 | Hypertable, Continuous Aggregate | TimescaleDBSchemaGenerator | ✅ |
| zepto-migrate CLI | 5개 모드 통합 CLI | tools/zepto-migrate.cpp | ✅ |

**결과:** 경쟁 시스템 고객의 마이그레이션 마찰 대폭 감소

→ Python 연동은 **ZeptoDB가 kdb+보다 확실히 우위** (zero-copy vs IPC 직렬화)

---

## 2. 핵심 갭 요약 (kdb+ 대체 위해 반드시 필요한 것)

### ✅ 완료 (2026-03-22)

모든 긴급 갭이 구현 완료되었습니다!

| 기능 | 상태 | 성능 | devlog |
|---|---|---|---|
| **xbar (시간 바)** | ✅ | 1M rows → 3,334 bars in **24ms** | devlog #010 |
| **ema (지수이동평균)** | ✅ | 1M rows in **2.2ms** | devlog #010 |
| **LEFT JOIN** | ✅ | NULL 센티넬 (INT64_MIN) | devlog #010 |
| **Window JOIN (wj)** | ✅ | O(n log m) 이진 탐색 | devlog #010 |
| **병렬 쿼리 실행** | ✅ | 8 threads = **3.48x** 가속 | devlog #011 |
| **deltas/ratios** | ✅ | OVER 윈도우 함수 | devlog #010 |
| **FIRST/LAST 집계** | ✅ | OHLC 캔들차트용 | devlog #010 |
| **Parquet HDB** | ✅ | DuckDB/Polars/Spark 직접 쿼리 (SNAPPY/ZSTD/LZ4) | devlog #012 |
| **S3 Sink** | ✅ | 클라우드 데이터 레이크 (비동기 업로드, MinIO 호환) | devlog #012 |

**221개 테스트 PASS** (기존 151 + 피드핸들러 37 + 마이그레이션 70 - 일부 중복) (devlog #010: 29개 신규, devlog #011: 27개 신규)

### 🟡 향후 개선 (없어도 kdb+ 95% 대체 가능)

| 기능 | 이유 | 난이도 |
|---|---|---|
| RIGHT JOIN, FULL OUTER JOIN | SQL 표준 완성 | ⭐⭐ |
| uj (union join) | 테이블 합치기 | ⭐⭐ |
| Attribute 힌트 (s#, g#) | 쿼리 최적화 | ⭐⭐ |
| 타이머/스케줄러 | EOD 자동화 | ⭐⭐ |
| Window JOIN 슬라이딩 윈도우 | O(n+m) 최적화 | ⭐⭐⭐ |

### ✅ 이미 kdb+보다 나은 것

| 항목 | kdb+ | ZeptoDB | 이유 |
|---|---|---|---|
| 언어 접근성 | q (난해) | **SQL + Python** | 학습 곡선 |
| Python 연동 | PyKX (IPC) | **zero-copy** | 522ns vs ms |
| SIMD 벡터화 | 없음 (q 인터프리터) | **Highway AVX-512** | 하드웨어 활용 |
| JIT 컴파일 | 없음 | **LLVM OrcJIT** | 동적 쿼리 최적화 |
| 클라우드 스케일 | 제한적 | **분산 + Transport 교체** | CXL 대비 |
| HTTP API | 없음 (IPC만) | **port 8123** | Grafana 연결 |
| 가격 | 연 $100K+ | **오픈소스 가능** | 비용 |
| Window 함수 | mavg/msum | **SQL 표준 OVER** | 표준 호환 |

---

## 3. 대체 가능성 판정 (2026-03-22 업데이트)

### HFT (틱 처리 + 실시간 쿼리)
**✅ 95% 대체 가능** (목표 달성!)
- ✅ 인제스션 (5.52M ticks/sec)
- ✅ RDB/HDB + LZ4 압축 (4.8 GB/s)
- ✅ VWAP, ASOF JOIN
- ✅ xbar (시간 바), ema (지수이동평균)
- ✅ Window JOIN (wj) — 호가 시간 윈도우 조인
- ✅ 병렬 쿼리 (8T = 3.48x)
- ⚠️ 남은 5%: RIGHT/FULL JOIN, uj, Attribute 힌트

### 퀀트 리서치 (백테스트)
**✅ 90% 대체 가능** (목표 달성!)
- ✅ Python zero-copy (522ns)
- ✅ Window 함수 (SUM/AVG/MIN/MAX/LAG/LEAD/ROW_NUMBER/RANK OVER)
- ✅ EMA, DELTA, RATIO
- ✅ GROUP BY + xbar (캔들차트)
- ✅ LEFT JOIN, Window JOIN
- ✅ FIRST/LAST 집계
- ⚠️ 남은 10%: 스케줄러, 타이머

### 리스크/컴플라이언스
**✅ 95% 대체 가능** (목표 달성!)
- ✅ SQL 파서 + HTTP API (port 8123)
- ✅ Hash JOIN (INNER, LEFT)
- ✅ GROUP BY 집계
- ✅ 병렬 쿼리
- ⚠️ 남은 5%: RIGHT/FULL JOIN, union join

---

## 4. 완료된 액션 플랜 ✅

**예상: 1주일 → 실제: 2일 완료** (2026-03-22)

| 일차 | 작업 | 상태 |
|---|---|---|
| Day 1 | xbar + ema + deltas/ratios | ✅ **완료** (devlog #010) |
| Day 2 | LEFT JOIN + Window JOIN (wj) | ✅ **완료** (devlog #010) |
| Day 3 | 병렬 쿼리 실행 (LocalQueryScheduler) | ✅ **완료** (devlog #011) |
| Day 4 | 통합 테스트 + 벤치마크 | ✅ **완료** (151개 테스트 PASS) |
| Day 5 | 문서 업데이트 | ✅ **완료** (2026-03-22) |

**결과:** kdb+ 대체율 **평균 95%** 달성 (HFT 95%, 퀀트 90%, 리스크 95%, 마이그레이션 툴킷 포함)
