# 레이어 4: 클라이언트 및 트랜스파일 레이어

*최종 업데이트: 2026-03-22 (Python 에코시스템 — zepto_py 패키지 완료)*

이 문서는 ZeptoDB의 클라이언트 인터페이스 레이어를 다룹니다: HTTP API, Python DSL/에코시스템, C++ 직접 API, SQL 지원, 마이그레이션 툴킷.

---

## 1. 구현된 인터페이스 (3가지)

### 1-A. HTTP API (포트 8123, ClickHouse 호환)

```
POST /          SQL 쿼리 실행 → JSON 응답
GET  /ping      헬스 체크 (ClickHouse 호환)
GET  /health    Kubernetes liveness probe
GET  /ready     Kubernetes readiness probe
GET  /stats     파이프라인 통계
GET  /metrics   Prometheus OpenMetrics
```

```bash
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume) FROM trades WHERE symbol = 1'
# → {"columns":["vwap"],"data":[[15037.2]],"rows":1,"execution_time_us":52.3}
```

**구현:** `cpp-httplib` 헤더 전용, 경량, Grafana ClickHouse 플러그인 호환.

### 1-B. Python DSL (pybind11 + Lazy Evaluation)

**원래 설계:** nanobind → **실제 구현:** pybind11 (안정성 우선)

```python
import zeptodb

db = zeptodb.Pipeline()
db.start()
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

# 직접 호출
result = db.vwap(symbol=1)          # C++ 직접, 50μs
result = db.count(symbol=1)         # 0.12μs

# 제로카피 numpy (핵심 기능)
prices  = db.get_column(symbol=1, name="price")   # numpy, 522ns, 복사 없음
volumes = db.get_column(symbol=1, name="volume")

# Lazy DSL (Polars 스타일)
from zepto_py.dsl import DataFrame
df = DataFrame(db, symbol=1)
result = df[df['price'] > 15000]['volume'].sum().collect()
```

**Polars vs ZeptoDB 비교 (100K 행):**
| | APEX | Polars | 비율 |
|---|---|---|---|
| VWAP | 56.9μs | 228.7μs | **4x** |
| COUNT | 716ns | 26.3μs | **37x** |
| get_column | 522ns | 760ns | **1.5x** |

### 1-C. C++ 직접 API

```cpp
ZeptoPipeline pipeline;
pipeline.start();

// C++ 직접 — 최저 레이턴시
auto result = pipeline.query_vwap(symbol=1);  // 51μs
auto col = pipeline.partition_manager()
    .get_or_create(1, ts)
    .get_column("price");  // 직접 포인터, 오버헤드 0
```

---

## 2. Python 에코시스템 — zepto_py 패키지 ✅ (2026-03-22 완료)

`zepto_py` 패키지는 ZeptoDB와 과학 Python 스택 간의 원활한 데이터 교환을 제공합니다. 분석가가 Jupyter 노트북에서 프로토타이핑하고 직렬화 오버헤드 없이 프로덕션 규모의 실시간 쿼리를 위해 ZeptoDB에 데이터를 인제스트할 수 있게 하는 연구-프로덕션 격차를 해소합니다.

### 패키지 구조

```
zepto_py/
├── __init__.py       — 공개 API 표면
├── connection.py     — HTTP 클라이언트 (ApexConnection, QueryResult)
├── dataframe.py      — 벡터화 인제스트/익스포트 변환기
├── arrow.py          — ArrowSession: 제로카피 Arrow / DuckDB
├── streaming.py      — StreamingSession: 고처리량 배치 인제스트
└── utils.py          — 의존성 검사기 (check_dependencies, versions)
```

### 인제스트 경로 (빠른 순서)

```python
import zepto_py as apex

# 1. from_arrow() — Arrow 버퍼 직접 (벡터화 ingest_batch)
import pyarrow as pa
tbl = pa.table({"sym": [1,2], "price": [150.0, 200.0], "volume": [100, 200]})
zeptodb.from_arrow(tbl, pipeline)

# 2. from_polars_arrow() — Polars Arrow 버퍼 → ingest_batch (제로카피)
import polars as pl
df_pl = pl.DataFrame({"sym": [1], "price": [150.0], "volume": [100]})
zeptodb.from_polars_arrow(df_pl, pipeline)

# 3. from_polars() — .to_numpy() 제로카피 → ingest_batch
zeptodb.from_polars(df_pl, pipeline, batch_size=100_000)

# 4. from_pandas() — numpy 벡터화 추출 → ingest_batch
import pandas as pd
df_pd = pd.DataFrame({"sym": [1], "price": [150.0], "volume": [100]})
zeptodb.from_pandas(df_pd, pipeline, price_scale=100)  # 센트로 저장

# 5. StreamingSession — 진행 표시 + 에러 처리가 있는 배치 인제스트
sess = zeptodb.StreamingSession(pipeline, batch_size=50_000, on_error="skip")
sess.ingest_pandas(df_pd, show_progress=True)
sess.ingest_polars(df_pl, use_arrow=True)
sess.ingest_iter(tick_generator())   # 메모리 효율적인 제너레이터
```

모든 인제스트 함수는 **벡터화** — Python 수준의 행 반복 없음:
1. 열을 numpy 배열로 추출 (`series.to_numpy()` / `batch.to_numpy()`)
2. float→int64 변환이 필요한 경우 `price_scale` 적용
3. 청크당 한 번 `pipeline.ingest_batch(symbols, prices, volumes)` 호출

### 익스포트 경로 (제로카피)

```python
from zepto_py import ArrowSession

sess = ArrowSession(pipeline)

tbl    = sess.to_arrow(symbol=1)                          # pa.Table
reader = sess.to_record_batch_reader(symbol=1)            # 스트리밍
conn   = sess.to_duckdb(symbol=1, table_name="trades")    # DuckDB 제로카피
df_pl  = sess.to_polars_zero_copy(symbol=1)               # Arrow를 통한 Polars
```

### HTTP 클라이언트 (쿼리 → DataFrame)

```python
db = zeptodb.connect("localhost", 8123)
df  = db.query_pandas("SELECT sym, avg(price) FROM trades GROUP BY sym")
df  = db.query_polars("SELECT * FROM trades WHERE sym=1 LIMIT 1000")
arr = db.query_numpy("SELECT price FROM trades WHERE sym=1")
db.ingest_pandas(trades_df)
db.ingest_polars(trades_pl)
```

### 상호운용성 매트릭스

| ZeptoDB → | pandas | polars | numpy | Arrow | DuckDB |
|-----------|--------|--------|-------|-------|--------|
| HTTP 쿼리 | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — |
| 파이프라인 익스포트 | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` |
| 제로카피 | numpy 뷰 | Arrow 경유 | 직접 | 예 | Arrow 등록 |

| → ZeptoDB | pandas | polars | Arrow | 제너레이터 |
|-----------|--------|--------|-------|-----------|
| 벡터화 | `from_pandas()` | `from_polars()` | `from_arrow()` | `ingest_iter()` |
| 스트리밍 | `StreamingSession` | `StreamingSession` | `ArrowSession` | `ingest_iter()` |

### 테스트 커버리지 (208 테스트, 모두 통과)

```
tests/python/
├── test_ingest_batch.py       47  — from_pandas/polars/arrow, _require_cols
├── test_arrow_integration.py  46  — ArrowSession, 타입 매핑, DuckDB, 라운드트립
├── test_pandas_integration.py 20  — query_to_pandas, VWAP, OHLCV, connection
├── test_polars_integration.py 16  — query_to_polars, VWAP, ASOF, window
└── test_streaming.py          41  — StreamingSession: pandas/polars/iter/stats/perf
```

---

## 3. SQL 지원 (현재 구현)

```sql
-- 기본 집계
SELECT count(*), sum(volume), avg(price), vwap(price, volume)
FROM trades WHERE symbol = 1

-- GROUP BY
SELECT symbol, sum(volume) FROM trades GROUP BY symbol

-- ASOF JOIN (시계열 핵심 연산)
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- Hash JOIN / LEFT JOIN
SELECT t.price, r.risk_score
FROM trades t JOIN risk_factors r ON t.symbol = r.symbol

-- 윈도우 함수
SELECT symbol, price,
       AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20,
       LAG(price, 1) OVER (PARTITION BY symbol) AS prev_price,
       RANK() OVER (ORDER BY price DESC) AS rank
FROM trades

-- 금융 함수 (kdb+ 호환)
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,   last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- EMA, DELTA, RATIO
SELECT EMA(price, 0.1) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema,
       DELTA(price) OVER (ORDER BY timestamp) AS change
FROM trades

-- Window JOIN (wj)
SELECT t.price, wj_avg(q.bid) AS avg_bid
FROM trades t
WINDOW JOIN quotes q ON t.symbol = q.symbol
AND q.timestamp BETWEEN t.timestamp - 5000000000 AND t.timestamp + 5000000000
```

---

## 4. 설계 결정: 원래 vs 실제

| 항목 | 원래 설계 | 실제 구현 | 이유 |
|---|---|---|---|
| Python 바인딩 | nanobind | **pybind11** | 빌드 안정성 |
| AST 직렬화 | FlatBuffers | **직접 C++ 호출** | 복잡성 감소 |
| DSL → JIT | Python AST → LLVM | **Lazy Eval → C++ API** | 점진적 빌드 |
| 클라이언트 프로토콜 | 커스텀 | **HTTP (ClickHouse 호환)** | 에코시스템 |
| Python 인제스트 | 행별 | **벡터화 ingest_batch()** | 100–1000x 처리량 |
| 널 처리 | 행별 스킵 | **numpy 캐스트 전 fill_null(0)** | 벡터화 경로 안전성 |

---

## 5. 병렬 쿼리 (QueryScheduler DI)

**현재:** `LocalQueryScheduler` — scatter/gather, 8스레드에서 3.48x

```cpp
auto scheduler = std::make_unique<LocalQueryScheduler>(8);
QueryExecutor executor(pipeline, std::move(scheduler));
auto result = executor.execute(ast);
// 0.248ms vs 0.862ms 직렬
```

**미래:** `DistributedQueryScheduler` (UCX 기반) — 멀티노드에서 API 변경 없음.

---

## 6. 로드맵

- [x] HTTP API + ClickHouse 호환성 ✅
- [x] pybind11 제로카피 Python 바인딩 ✅
- [x] 마이그레이션 툴킷 (kdb+/ClickHouse/DuckDB/TimescaleDB) ✅
- [x] **Python 에코시스템** (`zepto_py` 전체 패키지 — 벡터화 ingest_batch) ✅
- [ ] SQL Window RANGE 모드 (현재 ROWS만)
- [ ] Python DSL → LLVM JIT 직접 컴파일
- [ ] Arrow Flight 서버 (네트워크를 통해 Arrow로 결과 스트리밍)
- [ ] `pip install zeptodb` PyPI 패키지

---

## 7. 스트리밍 데이터 소스 커넥터 (백로그)

- Kafka/Redpanda/Pulsar (librdkafka, C++ 클라이언트)
- AWS Kinesis, Azure Event Hubs, Google Pub/Sub
- PostgreSQL WAL (CDC), MySQL binlog, MongoDB Change Streams
- 거래소 직접: CME FAST, OPRA, CBOE PITCH, Coinbase, Bybit
