# APEX-DB 백로그

## ✅ kdb+ 격차 해소 완료! (대체율 93% 달성)

**목표:** 없으면 kdb+ 대체 불가능한 핵심 기능
**참고:** `docs/design/kdb_replacement_analysis.md`, `docs/devlog/010_financial_functions.md`

| 작업 | 상태 | 성능 |
|------|------|------|
| **xbar (시간 바 집계)** | ✅ | 1M → 3,334 bars in 24ms |
| **ema (지수이동평균)** | ✅ | 1M rows in 2.2ms |
| **LEFT JOIN** | ✅ | NULL 센티넬 (INT64_MIN) |
| **Window JOIN (wj)** | ✅ | O(n log m) 이진 탐색 |
| **deltas/ratios 네이티브** | ✅ | OVER 윈도우 함수 |
| **FIRST/LAST 집계** | ✅ | OHLC 캔들차트용 |

**결과:** kdb+ 대체율 **HFT 95%, 퀀트 90%, 리스크 95%** 🎉

---

## 즉시 (다음 커밋)
- [x] **비즈니스 전략 문서화** ✅ 완료 (2026-03-22)
  - `docs/business/BUSINESS_STRATEGY.md` - 완전한 비즈니스 전략 (13개 섹션)
    - 시장 분석, 경쟁 전략, 제품 현황, GTM 전략
    - 마이그레이션 툴킷 로드맵, 재무 전망, 팀 빌드업
    - 12개월 목표: $3.6M ARR, 43 고객
  - `docs/business/EXECUTIVE_SUMMARY.md` - 1페이지 요약 (투자자/경영진용)
- [ ] **설계 문서 전체 업데이트** — 현재 구현 상태에 맞게 동기화
  - high_level_architecture.md: SQL/HTTP/Cluster/병렬쿼리 레이어 추가
  - initial_doc.md: 범용 OLAP/TSDb 타겟 확장
  - system_requirements.md: SQL/HTTP/JOIN/Window/병렬쿼리 요구사항
  - layer4: nanobind → pybind11, DSL 실제 구현
  - README.md: 전체 기능 + 최신 벤치마크 + kdb+ 대체율
  - kdb_replacement_analysis.md: 병렬 쿼리 완료 반영

## 높은 우선순위 (기술)
- [ ] **SQL 파서 완성** — ClickHouse 사용자 유입 핵심
- [ ] **시간 범위 인덱스** — 거의 공짜, 이미 정렬된 데이터
- [ ] **Graviton (ARM) 빌드 테스트** — r8g 인스턴스, Highway SVE

## 높은 우선순위 (비즈니스/운영)
- [x] **프로덕션 배포 가이드** ✅ 완료 (2026-03-22)
  - `docs/deployment/PRODUCTION_DEPLOYMENT.md` - 베어메탈 vs 클라우드 선택 가이드
  - `scripts/tune_bare_metal.sh` - 베어메탈 자동 튜닝 스크립트
  - `Dockerfile` - 클라우드 네이티브 이미지
  - `k8s/deployment.yaml` - Kubernetes 배포 (HPA, PVC, LoadBalancer)
- [x] **Python 에코시스템 통합** ✅ 완료 (2026-03-22)
  - `apex_py.from_polars(df, pipeline)` — zero-copy (polars .to_numpy() + ingest_batch())
    - df.slice() 무복사 청킹, Series.to_numpy() Arrow 버퍼 직접 참조
    - price_scale 파라미터: float64 가격 → int64 (예: ×100 = 센트)
    - sym_col/price_col/vol_col 커스텀 컬럼 매핑
  - `apex_py.from_pandas(df, pipeline)` — vectorized (.to_numpy() + ingest_batch())
    - iterrows() 완전 제거 → numpy 배치 경로 (100-1000x 빠름)
    - float64 가격 자동 처리 (price_scale)
  - `apex_py.from_arrow(table, pipeline)` — Arrow Table 벡터화 ingest
    - to_numpy(zero_copy_only=False)로 컬럼 추출 → ingest_batch() 1회 호출
    - null 안전 처리 (pc.if_else fill_null)
  - `ArrowSession.ingest_arrow_columnar()` — 컬럼별 Arrow 배열 직접 ingest
  - `apex.Pipeline.ingest_float_batch()` — float64 배열 직접 수용 (C++ 내부 변환)
  - 테스트 38개 추가 (`tests/python/test_fast_ingest.py`)
  - **성능:** 1M rows from_polars ~0.3s (이전 iterrows: ~30s+)
  - **비즈니스 가치:** Jupyter 리서치 → 프로덕션 배포 심리스 전환
- [ ] **제한된 DSL AOT 컴파일** — 프로덕션 배포 & IP 보호
  - **Phase 1 (1주):** Nuitka 통합 - Python DSL → 단일 바이너리 (15x 작음)
  - **Phase 2 (1개월):** Cython 지원 - 핵심 연산 → C 확장 (2-3x 추가 속도)
  - **Phase 3 (3-6개월):** 제한된 DSL 트랜스파일러 - filter/select/groupby → SQL 자동 변환
  - **비즈니스 가치:** 고객 프로덕션 배포 용이성 + 소스코드 IP 보호
- [x] **프로덕션 모니터링 & 로깅** ✅ 완료 (2026-03-22)
  - `/health`, `/ready`, `/metrics` 엔드포인트
  - Prometheus OpenMetrics 형식
  - 구조화된 JSON 로깅 (spdlog)
  - Grafana 대시보드 + 9가지 알림 규칙
  - `docs/operations/PRODUCTION_OPERATIONS.md` 운영 가이드
- [x] **백업 & 복구 자동화** ✅ 완료 (2026-03-22)
  - `scripts/backup.sh` - HDB/WAL/Config 백업, S3 업로드
  - `scripts/restore.sh` - 재해 복구, WAL replay
  - `scripts/eod_process.sh` - EOD 프로세스 자동화
  - cron: 백업 (02:00), EOD (18:00)
- [x] **프로덕션 서비스 설치** ✅ 완료 (2026-03-22)
  - `scripts/install_service.sh` - 원스텝 설치
  - `scripts/apex-db.service` - systemd 서비스
  - 자동 재시작, CPU affinity, OOM 방지
  - 로그 로테이션 (30일)
- [x] **Feed Handler Toolkit (완전 버전)** ✅ 완료 (2026-03-22)
  - **구현 (8 헤더 + 5 구현):**
    - `src/feeds/fix_parser.cpp` - FIX 프로토콜 (350ns 파싱)
    - `src/feeds/fix_feed_handler.cpp` - FIX TCP 리시버 (비동기, 재연결)
    - `src/feeds/multicast_receiver.cpp` - 멀티캐스트 UDP (<1μs)
    - `src/feeds/nasdaq_itch.cpp` - NASDAQ ITCH 5.0 (250ns 파싱)
    - `src/feeds/optimized/fix_parser_fast.cpp` - 최적화 버전 (zero-copy, SIMD)
  - **테스트 (27개 단위 + 10개 벤치마크):**
    - `tests/feeds/test_fix_parser.cpp` - 15개 테스트 (100% 커버리지)
    - `tests/feeds/test_nasdaq_itch.cpp` - 12개 테스트 (100% 커버리지)
    - `tests/feeds/benchmark_feed_handlers.cpp` - 성능 검증
  - **최적화 (6가지 기법):**
    - Zero-copy 파싱 (2-3x), SIMD AVX2 (5-10x), Memory Pool (10-20x)
    - Lock-free Ring Buffer (3-5x), 빠른 숫자 파싱 (2-3x), Cache-line alignment (2-4x)
  - **통합 예제:**
    - `examples/feed_handler_integration.cpp` - FIX/ITCH/성능 테스트
  - **문서:**
    - `docs/feeds/FEED_HANDLER_GUIDE.md` - 사용 가이드
    - `docs/feeds/PERFORMANCE_OPTIMIZATION.md` - 최적화 가이드
    - `docs/feeds/FEED_HANDLER_COMPLETE.md` - 완료 보고서
  - `src/feeds/nasdaq_itch.cpp` - NASDAQ ITCH 5.0 파서 (바이너리)
  - `include/apex/feeds/binance_feed.h` - Binance WebSocket (인터페이스)
  - `examples/feed_handler_integration.cpp` - 통합 예제
  - `docs/feeds/FEED_HANDLER_GUIDE.md` - 사용 가이드
  - **비즈니스 가치:** HFT 시장 진입 ($2.5M-12M), 거래소 직접 연결, kdb+ 완전 대체
- [x] **마이그레이션 툴킷** ✅ 완료 (2026-03-22)
  - **kdb+ → APEX-DB** ✅ 완료
    - `include/apex/migration/q_parser.h` - AST 아키텍처
    - `src/migration/q_lexer.cpp` - q 언어 Lexer
    - `src/migration/q_parser.cpp` - q AST Parser
    - `src/migration/q_to_sql.cpp` - q→SQL Transformer (wavg/xbar/aj/wj)
    - `include/apex/migration/hdb_loader.h` + `src/migration/hdb_loader.cpp` - HDB 스플레이 테이블 로더 (mmap 기반)
    - `tools/apex-migrate.cpp` - CLI (query/hdb/clickhouse/duckdb/timescaledb 모드)
    - `tests/migration/test_q_to_sql.cpp` - 20개 테스트
    - `tests/migration/test_hdb_loader.cpp` - 15개 테스트
  - **ClickHouse → APEX-DB** ✅ 완료
    - `include/apex/migration/clickhouse_migrator.h` + `src/migration/clickhouse_migrator.cpp`
    - DDL 생성 (MergeTree/LowCardinality/Gorilla codec), kdb+ 타입 매핑
    - ASOF JOIN 변환, xbar→toStartOfInterval, FIRST/LAST→argMin/argMax
    - `tests/migration/test_clickhouse.cpp` - 18개 테스트
  - **DuckDB 상호운용성** ✅ 완료
    - `include/apex/migration/duckdb_interop.h` + `src/migration/duckdb_interop.cpp`
    - Parquet 내보내기 (SNAPPY/ZSTD/GZIP), hive partitioning, Arrow 스키마
    - DuckDB setup.sql 생성, Jupyter notebook 템플릿 생성
    - `tests/migration/test_duckdb.cpp` - 17개 테스트
  - **TimescaleDB → APEX-DB** ✅ 완료
    - `include/apex/migration/timescaledb_migrator.h` + `src/migration/timescaledb_migrator.cpp`
    - Hypertable DDL, Continuous Aggregate (candlestick/vwap/ohlcv), Compression Policy
    - xbar→time_bucket, FIRST/LAST→first(col,ts), ASOF→LATERAL 변환
    - TimescaleDB Toolkit: candlestick_agg, stats_agg 예제 생성
    - `tests/migration/test_timescaledb.cpp` - 18개 테스트
  - **총 테스트: 70개** (q→SQL 20, HDB 15, ClickHouse 18, DuckDB 17, TimescaleDB 18)
  - **전략적 백로그: Snowflake/Delta Lake Hybrid 지원** (4주, $3.5M ARR)
    - Snowflake 커넥터 (2주) - JDBC/ODBC 통합, Cold data 쿼리
    - Delta Lake Reader (2주) - Parquet + transaction log 읽기
    - Hybrid 아키텍처 가이드 - "Snowflake for batch, APEX-DB for real-time"
    - **타겟 워크로드:**
      - 실시간 금융 분석 (20 고객 × $50K = $1M)
      - IoT/센서 데이터 (10 고객 × $50K = $500K)
      - 광고 테크 실시간 입찰 (10 고객 × $100K = $1M)
      - 규제 산업 온프레미스 (5 고객 × $200K = $1M)
    - **비즈니스 가치:** 보완재 전략, Snowflake 고객의 실시간 pain 해결
- [ ] **베어메탈 튜닝 상세 가이드** — CPU pinning, NUMA, io_uring
- [ ] **Kubernetes 운영 가이드** — Helm, monitoring, troubleshooting
- [ ] **웹사이트 & 문서 사이트** — apex-db.io, docs.apex-db.io

## 중간 우선순위
- [ ] **분산 쿼리 스케줄러** — DistributedQueryScheduler 구현 (UCX transport 위)
  - PartialAggResult FlatBuffers 직렬화
  - scatter: fragments → UCX send → 각 노드 실행
  - gather: UCX recv → PartialAggResult::merge()
  - 멀티노드 벤치마크 (2-node scatter/gather 지연)
  - **참고:** 단일 노드 병렬화는 이미 완료 (LocalQueryScheduler, 8T = 3.48x)
- [ ] **Data/Compute 노드 분리** — JOIN을 별도 Compute Node에서 RDMA remote_read로 실행, Data Node 영향 제로
- [ ] **CHUNKED 모드 활성화** — 단일 대형 파티션 행 분할 병렬화
- [ ] **exec_simple_select 병렬화** — 현재는 집계만 병렬, SELECT도 병렬화
- [ ] **DuckDB 임베딩 (복잡한 JOIN 위임)** — Arrow zero-copy 전달
- [ ] **JIT SIMD emit** — LLVM JIT에서 AVX2/512 벡터 IR 생성
- [ ] **멀티스레드 drain** — sharded drain threads
- [ ] **Ring Buffer 동적 조정** — direct-to-storage 경로
- [ ] **HugePages 튜닝** — 자동화
- [ ] **리소스 격리** — realtime(코어0-3) vs analytics(코어4-7) CPU pinning

## 스토리지 & 포맷 확장
- [x] **Parquet HDB 저장** ✅ 완료 (2026-03-22, devlog #012)
  - `ParquetWriter`: Partition → Apache Parquet 직렬화 (Arrow C++ API)
  - ColumnType → Arrow DataType 자동 매핑 (TIMESTAMP_NS UTC 포함)
  - 압축 선택: SNAPPY(기본) / ZSTD / LZ4_RAW
  - `flush_to_file()` / `flush_to_buffer()` (S3 직접 스트리밍용)
  - FlushConfig에 `HDBOutputFormat::PARQUET / BINARY / BOTH` 통합
  - **비즈니스 가치:** DuckDB/Polars/Spark 직접 쿼리, 데이터 레이크 연동
- [x] **S3 HDB flush** ✅ 완료 (2026-03-22, devlog #012)
  - `S3Sink`: 로컬 파일 / in-memory buffer → S3 PutObject
  - 파티션 경로 규칙: `s3://{bucket}/{prefix}/{symbol}/{hour}.parquet`
  - MinIO 호환 (`use_path_style=true`)
  - 비동기 업로드 (`upload_file_async()`)
  - `delete_local_after_s3` 옵션 (스토리지 절약)
  - **비즈니스 가치:** 재해 복구, 클라우드 데이터 레이크, 장기 보관
- [ ] **Parquet 읽기** — S3/로컬 Parquet → APEX-DB 쿼리
  - `HDBReader` Parquet 지원 (`read_parquet()`)
  - S3에서 직접 읽기 (로컬 캐시 없이)
  - Parquet 메타데이터 기반 파티션 프루닝
- [ ] **Arrow Flight 서버** — 네트워크로 Arrow 포맷 전송
  - 분산 쿼리 결과를 Arrow 배치로 스트리밍
  - Pandas/Polars 클라이언트 직접 연결
  - **비즈니스 가치:** 데이터 엔지니어링 팀 채택 가속

## 보안 & 엔터프라이즈
- [ ] **TLS/SSL** — HTTPS 엔드포인트, mTLS 노드 간 통신
- [ ] **API Key / JWT 인증** — HTTP API 접근 제어
- [ ] **RBAC (역할 기반 접근 제어)** — 테이블/컬럼 레벨 권한
- [ ] **Audit Log** — 누가 언제 어떤 쿼리를 실행했는지 추적

## SQL 완성도
- [ ] **Subquery / CTE (WITH 절)**
- [ ] **CASE WHEN**
- [ ] **UNION / INTERSECT / EXCEPT**
- [ ] **NULL 처리 표준화**
- [ ] **날짜/시간 함수**
- [ ] **String 함수**
- [ ] **RIGHT JOIN / FULL OUTER JOIN**
- [ ] **EXPLAIN**

## 클라이언트 생태계
- [ ] **JDBC/ODBC 드라이버**
- [ ] **ClickHouse wire protocol**
- [ ] **공식 Python 패키지** — `pip install apex-db`

## 스트리밍 데이터 연동
- [ ] **Apache Kafka 컨슈머**
- [ ] **Kafka Connect Sink**
- [ ] **Apache Pulsar 컨슈머**
- [ ] **Redpanda 호환**
- [ ] **AWS Kinesis 컨슈머**

## Physical AI / 산업 특화
- [ ] **ROS2 플러그인**
- [ ] **NVIDIA Isaac 통합**
- [ ] **OPC-UA 커넥터**
- [ ] **MQTT 인제스션**

## HA & 복제
- [ ] **WAL 기반 비동기 레플리케이션**
- [ ] **자동 페일오버**
- [ ] **스냅샷 백업**

## DDL & 데이터 관리
- [ ] **CREATE TABLE / DROP TABLE**
- [ ] **Retention Policy**
- [ ] **Schema Evolution**
- [ ] **HDB Compaction**

## 낮은 우선순위 (Phase C-3 이후)
- [ ] **AWS Fleet API 통합**
- [ ] **DynamoDB 메타데이터**
- [ ] **Graph 인덱스 (CSR)**
- [ ] **InfluxDB 마이그레이션**
- [ ] **Graviton (ARM) 빌드 테스트**

## 완료
- [x] Phase E — End-to-End Pipeline MVP (5.52M ticks/sec)
- [x] Phase B — Highway SIMD + LLVM JIT (filter 272μs, VWAP 532μs)
- [x] Phase B v2 — BitMask filter (11x), JIT O3 (2.6x)
- [x] Phase A — HDB Tiered Storage + LZ4 (4.8 GB/s flush)
- [x] Phase D — Python Bridge (pybind11, zero-copy 522ns)
- [x] **병렬 쿼리 엔진** — LocalQueryScheduler + WorkerPool (8T = 3.48x)
- [x] **asof JOIN** — AsofJoinOperator (투 포인터 O(n))
- [x] **Hash JOIN (inner/equi)** — HashJoinOperator
- [x] **GROUP BY 집계** — sum/avg/min/max/count
- [x] **Window 함수** — SUM/AVG/MIN/MAX/ROW_NUMBER/RANK/DENSE_RANK/LAG/LEAD OVER
- [x] **금융 함수** — VWAP, xbar, EMA, DELTA, RATIO, FIRST, LAST, Window JOIN (wj)
- [x] SQL Parser — 기본 SELECT/WHERE/GROUP BY/JOIN/OVER
- [x] HTTP API — port 8123, ClickHouse 호환
- [x] Distributed Cluster Transport — UCXBackend, SharedMemBackend, PartitionRouter (2ns)
- [ ] Phase C — Distributed Memory (UCX 완료, 쿼리 스케줄러 TODO)
