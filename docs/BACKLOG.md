# ZeptoDB Backlog

> Completed features: [`COMPLETED.md`](COMPLETED.md) | 830 tests passing
>
> Recent: ✅ Arrow Flight server | ✅ String column (dictionary-encoded) | ✅ Cluster stability (P8) complete

---

## P1 — "보여줄 수 있는 제품"

| Task | Why | Effort |
|------|-----|--------|
| **Web UI / Admin Console** | curl 데모는 임팩트 제로. 투자자/고객 미팅에서 화면 필수 | M |
| ↳ Query editor + result table | SQL 입력 → 테이블/차트 출력 | |
| ↳ Cluster status dashboard | 노드 수, 파티션, ingestion rate | |
| ↳ Table schema browser | CREATE TABLE 결과 확인 | |

Web UI 없으면 데모 불가. `web/` 폴더, React+Vite, `localhost:8123`에서 static serve.

### Query Editor Enhancements (`web/src/app/query/page.tsx`)

> ✅ 전체 완료: SQL autocomplete, keyboard shortcuts, run selected text, query history (search & pin), export CSV/JSON, resizable editor, schema sidebar, ZeptoDB function autocomplete, result chart view, multi-tab editor, multi-statement run, dark/light theme, column sorting, column filtering, saved queries, syntax error marker, execution cancel, exec time sparkline, EXPLAIN visualization

---

## P2 — "찾을 수 있는 제품"

### Website & Docs

| Task | Why | Effort |
|------|-----|--------|
| **Website (zeptodb.io)** | GitHub README만으로는 신뢰도 부족. PRD: `docs/business/WEBSITE_PRD.md` | S |
| ↳ Landing page | 벤치마크 숫자가 핵심 셀링 포인트 | |
| ↳ Features / Performance / Use Cases | 기능 상세, 벤치마크 비교, 산업별 사례 | |
| ↳ Pricing / Blog / About | OSS vs Enterprise, devlog 마이그레이션, 회사 소개 | |
| ~~↳ Docs site (docs.zeptodb.io)~~ | ~~mkdocs 이미 있음, 배포만 하면 됨~~ | ✅ |
| ↳ Docs site 배포 자동화 | GitHub Actions → Cloudflare Pages / GitHub Pages CI/CD | XS |
| ~~↳ Docs nav 업데이트~~ | ~~누락된 페이지 40+ 추가 (devlog 024-040, Flight API, multinode_stability 등)~~ | ✅ |
| ~~↳ Performance 비교 페이지~~ | ~~vs kdb+/ClickHouse/TimescaleDB 벤치마크 차트. 이미 `docs/bench/`에 데이터 있음~~ | ✅ |
| ↳ Use Cases 페이지 | HFT, Quant, Crypto, IoT 산업별 사례. 검색 유입 키워드 | S |
| ↳ Blog (devlog 마이그레이션) | 기존 devlog 040개 → 기술 블로그. SEO 롱테일 트래픽 | S |

### Getting Started & Onboarding

| Task | Why | Effort |
|------|-----|--------|
| ~~**Quick Start 가이드**~~ | ~~5분 안에 첫 쿼리까지. `docker run` → INSERT → SELECT → Python. 이탈률 감소~~ | ✅ |
| ~~**Interactive Playground**~~ | ~~브라우저에서 SQL 실행 (WASM 또는 서버 샌드박스). 설치 없이 체험 → 전환율 극대화~~ | ✅ |
| ~~**Example 데이터셋 번들**~~ | ~~샘플 주가/센서 데이터 내장 (`--demo` 플래그). 빈 DB로 시작하면 뭘 해야 할지 모름~~ | ✅ |
| **YouTube / Loom 데모 영상** | 2분 데모 영상. README + 랜딩에 임베드. 텍스트보다 전환율 3x | S |

### Package Distribution

| Task | Why | Effort |
|------|-----|--------|
| **Docker Hub 공식 이미지** | `docker pull zeptodb/zeptodb` 한 줄로 시작. 현재 Dockerfile만 있고 레지스트리 미등록 | S |
| ↳ Multi-arch (amd64 + arm64) | Graviton 빌드 이미 검증됨. M1 Mac 사용자 커버 | |
| **PyPI 패키지 (`pip install zeptodb`)** | Python 퀀트가 가장 먼저 시도하는 경로. Arrow Flight 클라이언트 래퍼 | S |
| **Homebrew Formula** | macOS 개발자 접근성. `brew install zeptodb` | S |
| **GitHub Releases + 바이너리** | 빌드 없이 다운로드. Linux amd64/arm64 tarball, `.deb`, `.rpm` | S |

### SEO & Community

| Task | Why | Effort |
|------|-----|--------|
| **Awesome Time-Series DB 등록** | GitHub awesome 리스트 PR. 무료 트래픽 | XS |
| **DB-Engines 등록** | db-engines.com 카테고리 등록. 인지도 지표 | XS |
| **Hacker News / Reddit 런칭 포스트** | Show HN 포스트. 벤치마크 + 오픈소스 스토리 | XS |
| ~~**SEO 기본 (sitemap, OG, meta)**~~ | ~~검색 엔진 인덱싱 필수. mkdocs-material이 자동 생성~~ | ✅ |
| **GitHub README 리뉴얼** | 배지, GIF 데모, 아키텍처 다이어그램, Quick Start 섹션 강화 | S |
| **Discord / Slack 커뮤니티** | 얼리 어답터 피드백 루프. 문서로 안 되는 질문 대응 | XS |

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

### SSO / Identity 고도화

> 현재: JWT 검증 (HS256/RS256), JWKS 자동 fetch, CLI 플래그, 런타임 리로드 완료.

| Task | Why | Effort |
|------|-----|--------|
| **IdP 그룹 → 역할 매핑** | IdP에 `zepto_role` 커스텀 클레임 추가 불가한 환경 대응 | S |
| **OIDC Discovery** | `--oidc-issuer` 하나로 JWKS URL, issuer, audience 자동 감지 | S |
| **Web UI SSO 로그인 플로우** | OAuth2 Authorization Code Flow | M |
| **JWT Refresh Token** | 토큰 만료 시 자동 갱신 | M |
| **서버 사이드 세션** | JWT 로그인 후 세션 쿠키 발급 | M |
| **SAML 2.0 지원** | 은행/보험 등 SAML-only 환경 대응 | L |

---

## P7 — 성능 / 엔진

> ✅ Tier A 완료: INTERVAL syntax, Prepared statements, Query result cache

| Task | Engine Impact | Effort |
|------|---------------|--------|
| **SAMPLE clause** | 🟢 Positive | S |
| **Composite index** | 🔴 Major | M |
| **MV query rewrite** | 🔴 Major | M |
| **Cost-based planner** | 🔴 Major | L |
| **Scalar subqueries in WHERE** | ⚠️ Mixed | M |
| **JOINs/Window on virtual tables** | 🟠 Moderate | M |
| **JIT SIMD emit** | — | L |
| **DuckDB embedding** | — | M |
| **Limited DSL AOT compilation** | — | M |

---

## P8 — 클러스터

> ✅ P8-Critical, P8-High, P8-Medium 전체 완료.

### P8-RDMA — Transport Layer 실연결

| Task | 효과 | Effort |
|------|------|--------|
| **WAL 복제 RDMA PUT** | ingest throughput 병목 제거. TCP ~50μs → RDMA ~1-2μs | M |
| **원격 컬럼 스캔 RDMA GET** | scatter-gather 쿼리에서 DataNode CPU 부담 제로 | L |
| **파티션 마이그레이션 RDMA GET** | live rebalancing 시 source 노드 서비스 영향 제로 | M |
| **Failover re-replication RDMA GET** | 노드 장애 시 replica 복제 부담 최소화 | M |
| **`remote_ingest_regions_` wire-up** | RDMA ingest 경로 실제 연결 | S |

### P8-Feature — 분산 기능 확장

| Task | Why | Effort |
|------|-----|--------|
| **Live rebalancing** | 무중단 파티션 이동 | L |
| **Tier C cold query offload** | 과거 데이터 → DuckDB on S3 | M |
| **PTP clock sync detection** | ASOF JOIN strict mode | S |
| **Global symbol registry** | 분산 string symbol 라우팅 | M |
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
| **User-Defined Functions** | Python/WASM UDF | L |
| **Pluggable partition strategy** | symbol_affinity / hash_mod / site_id | M |
| **Edge mode** (`--mode edge`) | 단일 노드 + 비동기 클라우드 싱크 | M |
| **HyperLogLog** | 분산 approximate COUNT DISTINCT | S |
| **Variable-length strings** | 로그, 코멘트 등 free-text | M |
| HDB Compaction | Parquet merge | S |
| Snowflake/Delta Lake hybrid | | M |
| Graph index (CSR) | 자금 흐름 추적 | L |
| InfluxDB migration | InfluxQL → SQL | S |

---

**핵심 경로: P1(Web UI polish) → P2(Website + Distribution) → P4(Tool Integration)**

Docs site 빌드 완료 (`~/zeptodb-site`). 남은 P2: 제품 웹사이트(Astro) + 배포 자동화 + 패키지 배포.
