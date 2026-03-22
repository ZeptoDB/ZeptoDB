<div align="center">

# 🏛️ APEX-DB

### Ultra-Low Latency In-Memory Database

*실시간 + 분석을 통합하는 초저지연 인메모리 데이터베이스*

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![LLVM 19](https://img.shields.io/badge/LLVM-19-orange)
![Highway SIMD](https://img.shields.io/badge/SIMD-Highway-green)
![Tests](https://img.shields.io/badge/tests-221%2B%20passing-brightgreen)
![kdb+ 대체율](https://img.shields.io/badge/kdb%2B%20%EB%8C%80%EC%B2%B4%EC%9C%A8-95%25-success)

</div>

---

## Overview

APEX-DB는 HFT 특화로 설계된 초저지연 인메모리 데이터베이스로,
**kdb+의 성능**과 **ClickHouse의 범용성**, **Polars의 Python 생태계**를 통합합니다.

### 🎯 kdb+ 대체율 (2026-03-22)

| 영역 | 대체율 | 상태 |
|------|--------|------|
| **HFT** (틱 처리 + 실시간) | **95%** | ✅ 상용 가능 |
| **퀀트** (백테스트 + 리서치) | **90%** | ✅ 상용 가능 |
| **리스크/컴플라이언스** | **95%** | ✅ 상용 가능 |

**핵심 금융 함수 완비:**
- ✅ xbar (시간 바 집계) — 5분봉/1시간봉 캔들차트
- ✅ EMA (지수이동평균) — 기술적 지표
- ✅ Window JOIN (wj) — 시간 윈도우 조인 (HFT 호가 분석)
- ✅ LEFT JOIN, ASOF JOIN, Hash JOIN
- ✅ DELTA/RATIO (행간 차이/비율)
- ✅ FIRST/LAST (OHLC)

### 핵심 성능 (실측)

| 메트릭 | 수치 |
|---|---|
| 인제스션 | **5.52M ticks/sec** |
| filter 1M rows | **272μs** (kdb+ 범위 진입) |
| VWAP 1M rows | **532μs** |
| **xbar (시간 바)** | **24ms** (1M → 3,334 bars) |
| **EMA 1M rows** | **2.2ms** |
| Window SUM 1M | **1.36ms** (O(n) prefix sum) |
| **병렬 GROUP BY 8T** | **248μs** (3.48x vs 1T) |
| SQL 파싱 | **1.5~4.5μs** |
| Python zero-copy | **522ns** (numpy view) |
| HDB flush | **4.8 GB/s** |
| Partition routing | **2ns** |

### vs kdb+ / ClickHouse

| | kdb+ | ClickHouse | **APEX-DB** |
|---|---|---|---|
| 인제스션 | ~5M/sec | 배치 최적화 | **5.52M/sec** |
| filter 1M | ~100-300μs | ms 단위 | **272μs** ✅ |
| VWAP 1M | ~200-500μs | ms 단위 | **532μs** ✅ |
| SQL | q 언어 | 표준 SQL | **표준 SQL** |
| Python | PyKX (IPC) | clickhouse-connect | **zero-copy** |
| 오픈소스 | ❌ 유료 | ✅ | ✅ |

---

## Architecture

```
┌───────────────────────────────────────────────────┐
│  Layer 5: Client Interface                         │
│  HTTP API (port 8123) · Python DSL · C++ API      │
├───────────────────────────────────────────────────┤
│  Layer 4: SQL + Query Planning                     │
│  Recursive descent parser · AST executor          │
├───────────────────────────────────────────────────┤
│  Layer 3: Execution Engine                         │
│  Highway SIMD · LLVM JIT · JOIN (ASOF/Hash/LEFT)  │
│  Window Functions (EMA/DELTA/RATIO/SUM/LAG/LEAD)  │
│  금융 함수 (xbar/FIRST/LAST/Window JOIN)           │
│  QueryScheduler (Local/Distributed DI 패턴)       │
├───────────────────────────────────────────────────┤
│  Layer 2: Ingestion (Tick Plant)                   │
│  MPMC Ring Buffer · UCX/RDMA · WAL                │
│  Feed Handlers (FIX, NASDAQ ITCH, Binance)        │
├───────────────────────────────────────────────────┤
│  Layer 1: Storage Engine (DMMT)                    │
│  Arena Allocator · Column Store · HDB (LZ4)       │
├───────────────────────────────────────────────────┤
│  Migration Toolkit                                 │
│  kdb+ · ClickHouse · DuckDB · TimescaleDB         │
├───────────────────────────────────────────────────┤
│  Layer 0: Distributed Cluster                      │
│  Transport (UCX→CXL) · Consistent Hash · Health  │
└───────────────────────────────────────────────────┘
```

---

## Quick Start

### SQL via HTTP (ClickHouse compatible)

```bash
# 서버 시작
./apex_server --port 8123

# 쿼리 (curl)
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1'

# Grafana: ClickHouse 데이터소스로 바로 연결
```

### Python DSL (zero-copy)

```python
import apex
from apex_py.dsl import DataFrame

db = apex.Pipeline()
db.start()

# 틱 인제스트
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

# zero-copy numpy (복사 없음)
prices = db.get_column(symbol=1, name="price")  # 522ns

# Lazy DSL (Polars 스타일)
df = DataFrame(db, symbol=1)
ma20 = df['price'].rolling(20).mean().collect()  # C++ 실행
```

### SQL 예시

```sql
-- 5분봉 OHLCV 캔들차트 (kdb+ xbar 스타일)
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- 지수이동평균 (EMA) + 행간 차이/비율
SELECT symbol, price,
       EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change,
       RATIO(price) OVER (ORDER BY timestamp) AS price_ratio
FROM trades

-- ASOF JOIN (시계열 핵심)
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- Window JOIN (wj) — 시간 윈도우 조인
SELECT t.price, wj_avg(q.bid) AS avg_bid, wj_count(q.bid) AS quote_count
FROM trades t
WINDOW JOIN quotes q
ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000

-- LEFT JOIN
SELECT t.price, t.volume, r.risk_score
FROM trades t
LEFT JOIN risk_factors r ON t.symbol = r.symbol

-- Window 함수
SELECT symbol, price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       LAG(price, 1) OVER (PARTITION BY symbol) AS prev_price,
       RANK() OVER (ORDER BY price DESC) AS rank
FROM trades
```

---

## Build

```bash
# Dependencies (Amazon Linux 2023 / Fedora)
sudo dnf install -y clang19 clang19-devel llvm19-devel \
  highway-devel numactl-devel ucx-devel ninja-build lz4-devel

mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)

# Tests
./tests/apex_tests
python3 -m pytest ../tests/test_python.py -v
```

---

## Project Structure

```
apex-db/
├── include/apex/
│   ├── storage/        # Arena, ColumnStore, PartitionManager, HDB
│   ├── ingestion/      # RingBuffer, TickPlant, WAL
│   ├── execution/      # VectorizedEngine, JIT, JOIN, WindowFunctions, QueryScheduler
│   ├── sql/            # Tokenizer, Parser, AST, Executor
│   ├── server/         # HttpServer (ClickHouse compatible)
│   ├── feeds/          # FIX, NASDAQ ITCH, Binance feed handlers
│   ├── migration/      # kdb+/ClickHouse/DuckDB/TimescaleDB migrators
│   ├── core/           # ApexPipeline (E2E integration)
│   ├── cluster/        # Transport, PartitionRouter, HealthMonitor
│   └── transpiler/     # Python binding (pybind11)
├── src/                # Implementation
├── tests/
│   ├── unit/           # Google Test (151+ tests)
│   ├── feeds/          # Feed handler tests (37 tests)
│   ├── migration/      # Migration toolkit tests (70 tests)
│   └── bench/          # Benchmarks
├── scripts/
│   ├── tune_bare_metal.sh  # 베어메탈 자동 튜닝
│   ├── backup.sh       # 백업 자동화
│   └── install_service.sh  # systemd 서비스 설치
├── k8s/                # Kubernetes 배포 YAML
├── monitoring/         # Grafana 대시보드 + Prometheus 알림
├── tools/
│   └── apex-migrate.cpp  # Migration CLI
└── docs/
    ├── design/         # Architecture + Layer design docs
    ├── business/       # 비즈니스 전략
    ├── deployment/     # 배포 가이드
    ├── operations/     # 운영 가이드
    ├── feeds/          # Feed handler 가이드
    ├── requirements/   # PRD/SRS
    ├── bench/          # Benchmark results
    └── devlog/         # Development log (000~011)
```

---

## Target Markets

| 분야 | 사용 사례 | kdb+ 대체율 |
|---|---|---|
| HFT/금융 | 틱 처리, ASOF JOIN, xbar, Window JOIN | **95%** |
| 퀀트 리서치 | 백테스트, EMA, Window 함수, Python DSL | **90%** |
| 리스크/컴플라이언스 | 포지션 계산, LEFT JOIN, 병렬 GROUP BY | **95%** |
| OLAP | ClickHouse 대체, SQL, HTTP API, Grafana | — |
| IoT/모니터링 | 시계열 집계, xbar, LZ4 압축 | — |
| ML Feature Store | 실시간 feature 서빙, zero-copy numpy | — |

---

## Production Ready

### 배포 옵션
- **베어메탈 (권장)**: HFT 레이턴시 일관성, `scripts/tune_bare_metal.sh` 자동 튜닝
- **클라우드**: Docker + Kubernetes, AWS Graviton4 최적화

### 모니터링
- Prometheus `/metrics` (OpenMetrics 형식)
- Grafana 대시보드 + 9가지 알림 규칙
- `/health`, `/ready`, `/metrics` 엔드포인트

### 운영 자동화
- `scripts/backup.sh` — HDB/WAL/Config 백업 + S3
- `scripts/restore.sh` — 재해 복구
- `scripts/install_service.sh` — systemd 서비스 원스텝 설치

📖 **상세 가이드:**
- 배포: `docs/deployment/PRODUCTION_DEPLOYMENT.md`
- 운영: `docs/operations/PRODUCTION_OPERATIONS.md`
- Feed Handlers: `docs/feeds/FEED_HANDLER_GUIDE.md`

---

## Development Phases

### 완료 ✅
- [x] **Phase E** — E2E Pipeline MVP (5.52M ticks/sec)
- [x] **Phase B** — SIMD + JIT (BitMask 11x, filter kdb+ 범위)
- [x] **Phase A** — HDB Tiered Storage (LZ4, 4.8GB/s flush)
- [x] **Phase D** — Python Bridge (zero-copy, 4x vs Polars)
- [x] **Phase C** — Distributed Cluster (UCX transport, 2ns routing)
- [x] **SQL + HTTP** — Parser (1.5~4.5μs) + ClickHouse API (port 8123)
- [x] **JOIN** — ASOF, Hash, LEFT, Window JOIN
- [x] **Window 함수** — EMA, DELTA, RATIO, SUM, AVG, LAG, LEAD, ROW_NUMBER, RANK
- [x] **금융 함수** — xbar, FIRST, LAST, Window JOIN (kdb+ 93% 대체)
- [x] **병렬 쿼리** — LocalQueryScheduler (scatter/gather, 3.48x@8T)
- [x] **Feed Handlers** — FIX, NASDAQ ITCH (350ns 파싱)
- [x] **프로덕션 운영** — 모니터링, 백업, systemd service
- [x] **마이그레이션 툴킷** — kdb+ HDB loader, q→SQL, ClickHouse DDL/쿼리 변환, DuckDB Parquet, TimescaleDB hypertable

### 진행 중 🚧
- [ ] SQL 파서 완성 (서브쿼리, 복잡한 쿼리)
- [ ] 시간 범위 인덱스
- [x] **마이그레이션 툴킷** — kdb+/ClickHouse/DuckDB/TimescaleDB (70 tests)
- [ ] Python 에코시스템 (Polars/Pandas 통합)
- [ ] ARM Graviton 빌드 검증
- [ ] 분산 쿼리 스케줄러 (DistributedQueryScheduler + UCX)

## License

Proprietary — All rights reserved.
