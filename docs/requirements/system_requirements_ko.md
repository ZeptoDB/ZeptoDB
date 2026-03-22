# APEX-DB 시스템 요구사항

*버전 2.2 — 최종 업데이트: 2026-03-22 (FR-4 Python 에코시스템 — apex_py 완료)*

---

## 기능 요구사항

### FR-1: 시계열 인제스트
- FR-1.1: 지속적으로 1M ticks/초 이상 인제스트 (**달성: 5.52M/초**)
- FR-1.2: 락프리 MPMC 링 버퍼 (핫 패스에서 블로킹 없음)
- FR-1.3: 재시작 후 내구성을 위한 WAL
- FR-1.4: 피드 핸들러 지원: FIX, NASDAQ ITCH, UDP 멀티캐스트, WebSocket

### FR-2: 쿼리 엔진
- FR-2.1: 표준 SQL (SELECT / WHERE / GROUP BY / ORDER BY / LIMIT)
- FR-2.2: ASOF JOIN (시계열 조인, O(n) 투포인터)
- FR-2.3: Hash JOIN (동등 조인)
- FR-2.4: NULL 센티넬을 사용한 LEFT JOIN
- FR-2.5: Window JOIN (wj) — 시간 윈도우 범위 조인
- FR-2.6: 윈도우 함수: SUM/AVG/MIN/MAX/ROW_NUMBER/RANK/DENSE_RANK/LAG/LEAD
- FR-2.7: 금융 함수: xbar, EMA, VWAP, DELTA, RATIO, FIRST, LAST
- FR-2.8: 병렬 쿼리 실행 (scatter/gather, N 스레드)

### FR-3: 스토리지
- FR-3.1: 열 지향 인메모리 스토어 (타입화된 배열)
- FR-3.2: LZ4 압축을 사용한 HDB 디스크 영구 저장 (4.8 GB/s 플러시)
- FR-3.3: 심볼별 파티션 라우팅 (2ns 오버헤드)
- FR-3.4: Arena 할당자 — 핫 패스에서 malloc 없음
- FR-3.5: Parquet 출력 (SNAPPY/ZSTD/LZ4_RAW) — Arrow C++ API, DuckDB/Polars/Spark에서 직접 쿼리 가능
- FR-3.6: S3 싱크 — Parquet/Binary → S3 비동기 업로드, MinIO 호환, 하이브 파티셔닝 (`{symbol}/{hour}.parquet`)

### FR-4: API
- FR-4.1: 포트 8123의 HTTP API (ClickHouse 와이어 프로토콜 호환)
  - `POST /` — SQL 쿼리 실행 (JSON 응답)
  - `GET /ping` — ClickHouse 호환 헬스 체크
  - `GET /health` — Kubernetes liveness probe
  - `GET /ready` — Kubernetes readiness probe
  - `GET /stats` — 파이프라인 통계
  - `GET /metrics` — Prometheus OpenMetrics 형식
- FR-4.2: Python 바인딩 (pybind11, 제로카피 numpy/Arrow, 1μs 미만)
- FR-4.3: C++ API (직접 struct 접근, 최저 레이턴시)
- FR-4.4: Python 에코시스템 — `apex_py` 패키지 ✅
  - `from_pandas(df, pipeline)` — 벡터화 numpy 배치 인제스트
  - `from_polars(df, pipeline)` — 제로카피 Arrow 버퍼 → ingest_batch
  - `from_polars_arrow(df, pipeline)` — polars.to_arrow() → ingest_batch
  - `from_arrow(table, pipeline)` — Arrow Table → ingest_batch
  - `ArrowSession` — 제로카피 인제스트/익스포트, DuckDB 등록, RecordBatchReader
  - `StreamingSession` — 진행 콜백, 에러 모드, 제너레이터 지원이 있는 배치 인제스트
  - `ApexConnection` — pandas/polars/numpy DataFrame을 반환하는 HTTP 클라이언트
  - `query_to_pandas()` / `query_to_polars()` — SQL JSON 결과 → DataFrame
  - 지원: numpy, pandas, polars, pyarrow, duckdb 상호운용성
  - `_arrow_col_to_numpy()`를 통한 Null 안전 Arrow 추출 (null → 0으로 채움)

### FR-5: 분산
- FR-5.1: UCX 전송 (RDMA/InfiniBand/TCP)
- FR-5.2: 공유 메모리 전송 (같은 호스트, 제로카피 IPC)
- FR-5.3: 일관된 해시 파티션 라우터
- FR-5.4: 헬스 모니터링 + 클러스터 관리

### FR-6: 마이그레이션 툴킷
- FR-6.1: kdb+ q 언어 → APEX SQL 트랜스파일러 (렉서/파서/변환기)
- FR-6.2: kdb+ HDB 스플레이드 테이블 로더 (mmap, 제로카피)
- FR-6.3: ClickHouse 스키마 생성기 (MergeTree, 코덱, LowCardinality)
- FR-6.4: ClickHouse 쿼리 번역기 (xbar, ASOF JOIN, argMin/argMax)
- FR-6.5: DuckDB/Parquet 익스포터 (SNAPPY/ZSTD, 하이브 파티셔닝)
- FR-6.6: TimescaleDB 하이퍼테이블 DDL + 연속 집계 생성기
- FR-6.7: `apex-migrate` CLI (5가지 모드: query/hdb/clickhouse/duckdb/timescaledb)

### FR-7: 프로덕션 운영
- FR-7.1: Prometheus /metrics 엔드포인트 (OpenMetrics 형식)
- FR-7.2: /health 및 /ready liveness/readiness 엔드포인트
- FR-7.3: Grafana 대시보드 + 9개 알림 규칙
- FR-7.4: 자동화된 백업 (HDB/WAL/Config → S3)
- FR-7.5: 자동 재시작이 있는 systemd 서비스
- FR-7.6: Kubernetes 배포 (HPA, PVC, LoadBalancer)

---

## 비기능 요구사항

### NFR-1: 레이턴시
| 연산 | 요구사항 | 달성 |
|---|---|---|
| 틱 인제스트 (p99) | 1μs 미만 | 200ns 미만 |
| 1M 행 필터 | 500μs 미만 | 272μs |
| 1M 행 VWAP | 1ms 미만 | 532μs |
| SQL 파싱 | 10μs 미만 | 1.5–4.5μs |
| Python 제로카피 | 1μs 미만 | 522ns |
| 파티션 라우팅 | 10ns 미만 | 2ns |

### NFR-2: 처리량
- 인제스트: 지속적으로 1M ticks/초 이상 (**5.52M/초**)
- HDB 플러시: 1 GB/s 이상 (**4.8 GB/s**)
- 쿼리 병렬성: 8스레드에서 3x 이상 속도향상 (**3.48x**)

### NFR-3: 신뢰성
- 정상 종료 시 데이터 손실 없음 (WAL)
- 로드 밸런서 통합을 위한 헬스 엔드포인트
- S3 업로드가 포함된 자동화된 일별 백업

### NFR-4: 호환성
- SQL: ANSI SQL 서브셋 (SELECT/WHERE/GROUP BY/JOIN/OVER)
- HTTP: ClickHouse 와이어 프로토콜 (포트 8123)
- Python: numpy/Arrow 제로카피; apex_py를 통한 pandas/polars/duckdb 상호운용
- 모니터링: Prometheus + Grafana

### NFR-5: 빌드 & 플랫폼
- C++20, clang-19, LLVM 19
- Linux (Amazon Linux 2023 / Ubuntu 22.04+)
- x86_64 (AVX2/AVX-512) + ARM64 (SVE, 로드맵)
- CMake + Ninja 빌드 시스템

---

## 테스트 요구사항

| 스위트 | 수량 | 커버리지 |
|---|---|---|
| 단위 테스트 (코어) | 151+ | 스토리지, 실행, SQL, JOIN, Window, Parquet, S3 |
| 피드 핸들러 테스트 | 37 | FIX 파서, ITCH 파서, 벤치마크 |
| 마이그레이션 테스트 | 70 | q→SQL (20), HDB (15), ClickHouse (18), DuckDB (17), TimescaleDB (18) |
| Python 에코시스템 테스트 | 208 | from_pandas/polars/arrow (47), ArrowSession (46), pandas (20), polars (16), StreamingSession (41), ingest_batch (47) |
| **합계** | **429+** | — |

> Python 에코시스템 스위트에는 ArrowSession 경로에 대해 test_arrow_integration.py와 겹치는 ingest_batch 테스트 (test_ingest_batch.py)가 포함됩니다.
