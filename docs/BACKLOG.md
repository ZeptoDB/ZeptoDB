# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 726 tests passing
>
> Recent: ✅ String column (dictionary-encoded) | ✅ Repo restructure (`deploy/`, `.ai/`, CMakePresets)

---

## P1 — "보여줄 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Web UI / Admin Console** | curl 데모는 임팩트 제로. 투자자/고객 미팅에서 화면 필수 | M |
| ↳ Query editor + result table | SQL 입력 → 테이블/차트 출력 | |
| ↳ Cluster status dashboard | 노드 수, 파티션, ingestion rate | |
| ↳ Table schema browser | CREATE TABLE 결과 확인 | |

Web UI 없으면 데모 불가. `web/` 폴더, React+Vite, `localhost:8123`에서 static serve.

---

## P2 — "찾을 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Website (zeptodb.io)** | GitHub README만으로는 신뢰도 부족 | S |
| ↳ Landing page | 벤치마크 숫자가 핵심 셀링 포인트 | |
| ↳ Docs site (docs.zeptodb.io) | mkdocs 이미 있음, 배포만 하면 됨 | |

---

## P3 — "Python 퀀트가 쓸 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Arrow Flight server** | Pandas/Polars에서 `flight://` 직접 연결, 원격 zero-copy급 | M |

현재 zero-copy는 C++ 임베딩에서만 동작. Arrow Flight이면 원격 Python 클라이언트도 가능.

---

## P4 — 기존 도구 연결

| Task | Why | Effort |
|------|-----|--------|
| **ClickHouse wire protocol** | DBeaver, DataGrip, Grafana 네이티브 연결 | L |
| **JDBC/ODBC drivers** | Tableau, Excel, Power BI | L |

---

## P5 — 데이터 파이프라인

| Task | Why | Effort |
|------|-----|--------|
| **Kafka Connect Sink** | 엔터프라이즈 데이터 파이프라인 표준 | M |
| **CDC connector** | PostgreSQL/MySQL → 실시간 싱크 | M |
| **AWS Kinesis consumer** | AWS 네이티브 스트리밍 | S |
| **Apache Pulsar consumer** | Kafka 대안 | S |

---

## P6 — 엔터프라이즈 / 클라우드

| Task | Why | Effort |
|------|-----|--------|
| **Cloud Marketplace** | AWS/GCP 원클릭 배포 | M |
| **Vault-backed API Key Store** | 프로덕션 시크릿 관리 | S |
| **Geo-replication** | 멀티 리전, 글로벌 트레이딩 데스크 | L |

---

## P7 — 성능 / 엔진

| Task | Why | Effort |
|------|-----|--------|
| **JIT SIMD emit** | AVX2/512 벡터 IR from LLVM JIT | L |
| **DuckDB embedding** | 복잡한 JOIN을 Arrow zero-copy로 위임 | M |
| **Bare-metal tuning guide** | CPU pinning, NUMA, io_uring 상세 가이드 | S |
| **Limited DSL AOT compilation** | Nuitka/Cython → 단일 바이너리 | M |

---

## P8 — 분산 / 클러스터

| Task | Why | Effort |
|------|-----|--------|
| **Live rebalancing** | 무중단 파티션 이동 | L |
| **Tier C cold query offload** | 최근 → in-memory, 과거 → DuckDB on S3 | M |
| **PTP clock sync detection** | ASOF JOIN strict mode | S |
| **Global symbol registry** | 분산 환경에서 string symbol Tier A 직접 라우팅 | M |
| **Multi-node benchmark execution** | EKS 가이드 준비됨, ~$12/run | S |

---

## P9 — Physical AI / Industry

| Task | Why | Effort |
|------|-----|--------|
| **MQTT ingestion** | IoT 디바이스 | S |
| **OPC-UA connector** | Siemens S7, 산업 PLC | M |
| **ROS2 plugin** | ROS2 topics → ZeptoDB | M |

---

## P10 — 확장 / 장기

| Task | Why | Effort |
|------|-----|--------|
| **User-Defined Functions** | Python/WASM UDF, 커스텀 알파 시그널 | L |
| **Pluggable partition strategy** | symbol_affinity / hash_mod / site_id | M |
| **Edge mode** (`--mode edge`) | 단일 노드 + 비동기 클라우드 싱크 | M |
| **HyperLogLog** | 분산 approximate COUNT DISTINCT | S |
| **Variable-length strings** | 로그, 코멘트 등 free-text | M |
| HDB Compaction | Parquet merge (Glue/Spark 외부) | S |
| Snowflake/Delta Lake hybrid | | M |
| Graph index (CSR) | 자금 흐름 추적 | L |
| InfluxDB migration | InfluxQL → SQL | S |

---

**핵심 경로: P1(Web UI) → P2(Website) → P3(Arrow Flight)**

이 3개가 "발견 → 시도 → 사용"의 최소 경로.
