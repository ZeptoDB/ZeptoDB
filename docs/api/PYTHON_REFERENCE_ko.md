# APEX-DB Python API 레퍼런스

*최종 업데이트: 2026-03-22*

두 가지 Python 인터페이스를 제공합니다:

| 인터페이스 | 설명 | 사용 사례 |
|-----------|------|-----------|
| `apex` | 저수준 pybind11 C++ 바인딩 | 인프로세스, 최대 성능 |
| `apex_py` | 고수준 Python 패키지 | Pandas/Polars/Arrow 상호운용, HTTP 클라이언트 |

---

## 목차

- [apex — pybind11 바인딩](#apex--pybind11-바인딩)
  - [apex.Pipeline](#apexpipeline)
  - [apex.sql.QueryExecutor](#apexsqlqueryexecutor)
- [apex_py.connection — HTTP 클라이언트](#apex_pyconnection--http-클라이언트)
- [apex_py.dataframe — 대용량 인제스트/익스포트](#apex_pydataframe--대용량-인제스트익스포트)
- [apex_py.arrow — Arrow / DuckDB 상호운용](#apex_pyarrow--arrow--duckdb-상호운용)
- [apex_py.streaming — 고처리량 인제스트](#apex_pystreaming--고처리량-인제스트)
- [상호운용성 매트릭스](#상호운용성-매트릭스)

---

## 빠른 시작 예제

### 엔드투엔드: polars에서 인제스트 → SQL 쿼리 → pandas 익스포트

```python
import apex
import polars as pl
from apex_py import from_polars, query_to_pandas

# 1. 파이프라인 시작
pipeline = apex.Pipeline()
pipeline.start()

# 2. polars DataFrame 생성 후 인제스트 (제로카피 Arrow 경로)
df = pl.DataFrame({
    "symbol": [1, 1, 1, 2, 2],
    "price":  [15000, 15010, 15020, 20000, 20010],
    "volume": [100, 150, 200, 80, 90],
})
from_polars(df, pipeline,
            symbol_col="symbol", price_col="price", volume_col="volume")
pipeline.drain()

# 3. SQL 실행 → pandas DataFrame
result = query_to_pandas(
    pipeline,
    "SELECT symbol, vwap(price, volume) AS vwap, sum(volume) AS vol "
    "FROM trades GROUP BY symbol ORDER BY symbol"
)
print(result)
#    symbol        vwap  vol
# 0       1  15012.222  450
# 1       2  20004.706  170

# 4. 제로카피 numpy 컬럼 접근
prices = pipeline.get_column(symbol=1, name="price")   # ~522ns
print(prices)  # [15000, 15010, 15020]

pipeline.stop()
```

### HTTP 클라이언트 빠른 시작

```python
import apex_py as apex

db = apex.connect("localhost", 8123)

# SQL → pandas
df = db.query_pandas(
    "SELECT xbar(timestamp, 300000000000) AS bar, "
    "first(price) AS open, last(price) AS close, sum(volume) AS vol "
    "FROM trades WHERE symbol = 1 "
    "GROUP BY xbar(timestamp, 300000000000) ORDER BY bar"
)
print(df)
```

### StreamingSession으로 고처리량 인제스트

```python
import pandas as pd
from apex_py import StreamingSession

sess = StreamingSession(pipeline, batch_size=50_000, error_mode="skip")

big_df = pd.DataFrame({
    "symbol": [1] * 1_000_000,
    "price":  range(15000, 16000000, 15),
    "volume": [100] * 1_000_000,
})
sess.ingest_pandas(big_df, show_progress=True,
                   symbol_col="symbol", price_col="price", volume_col="volume")
# Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)
```

### Arrow / DuckDB 상호운용

```python
from apex_py import ArrowSession

sess = ArrowSession(pipeline)

# Arrow Table로 익스포트 (제로카피)
table = sess.to_arrow(symbol=1)
print(table.schema)

# DuckDB에서 APEX-DB 데이터 직접 쿼리
conn = sess.to_duckdb(symbol=1, table_name="trades")
df = conn.execute(
    "SELECT avg(price), stddev(price) FROM trades"
).fetchdf()
print(df)
```

---

## apex — pybind11 바인딩

`apex` 모듈은 pybind11로 빌드된 저수준 C++ 바인딩입니다.
`cmake -DAPEX_BUILD_PYTHON=ON`으로 빌드합니다.

### apex.Pipeline

#### 생성

```python
import apex

# 기본 설정 (순수 인메모리, 파티션당 32 MB 아레나)
pipeline = apex.Pipeline()

# 커스텀 설정
pipeline = apex.Pipeline(config=apex.PipelineConfig(
    arena_size=32 * 1024 * 1024,
    drain_batch_size=256,
    storage_mode=apex.StorageMode.PURE_IN_MEMORY,
    # storage_mode=apex.StorageMode.TIERED,
    # storage_mode=apex.StorageMode.PURE_ON_DISK,
))
```

#### 생명주기

```python
pipeline.start()   # 백그라운드 드레인 스레드 시작
pipeline.stop()    # 큐 플러시 + 드레인 스레드 종료
pipeline.drain()   # 동기 드레인 (테스트용)
```

#### 인제스트

```python
# 단일 틱
pipeline.ingest(symbol=1, price=15000, volume=100)
pipeline.ingest(symbol=1, price=15010, volume=50, timestamp=1711000000000000000)

# 배치 인제스트 — 벡터화, 단일 C++ 호출, Python 루프 없음
import numpy as np
syms   = np.array([1, 1, 1], dtype=np.int64)
prices = np.array([15000, 15010, 15020], dtype=np.int64)
vols   = np.array([100, 50, 75], dtype=np.int64)
pipeline.ingest_batch(syms, prices, vols)

# float 배치 인제스트 — C++에서 float64 → int64 자동 변환
prices_f = np.array([150.00, 150.10, 150.20], dtype=np.float64)
vols_f   = np.array([100.0, 50.0, 75.0], dtype=np.float64)
pipeline.ingest_float_batch(syms, prices_f, vols_f, price_scale=100.0)
```

#### 직접 쿼리 (C++ 실행)

```python
result = pipeline.vwap(symbol=1)                        # → float
result = pipeline.vwap(symbol=1, from_ns=t0, to_ns=t1)
result = pipeline.count(symbol=1)                       # → int
result = pipeline.sum(symbol=1, col="volume")           # → int
```

#### 제로카피 컬럼 익스포트

APEX-DB 내부 컬럼 버퍼에 대한 numpy 뷰를 반환합니다 — 복사 없음, ~522ns.

```python
prices     = pipeline.get_column(symbol=1, name="price")      # np.ndarray[int64]
volumes    = pipeline.get_column(symbol=1, name="volume")     # np.ndarray[int64]
timestamps = pipeline.get_column(symbol=1, name="timestamp")  # np.ndarray[int64]

assert prices.base is not None        # 뷰임을 확인
assert not prices.flags['OWNDATA']    # 제로카피 확인
```

#### 통계

```python
stats = pipeline.stats()
stats.ticks_ingested          # int — 수신된 총 틱 수
stats.ticks_stored            # int — 컬럼 스토어에 기록된 틱 수
stats.ticks_dropped           # int — 드롭된 틱 (큐 오버플로우)
stats.queries_executed        # int
stats.total_rows_scanned      # int
stats.partitions_created      # int
stats.last_ingest_latency_ns  # int
```

---

### apex.sql.QueryExecutor

```python
from apex.sql import QueryExecutor

executor = QueryExecutor(pipeline)

# 병렬 실행
executor.enable_parallel()                              # 자동 스레드 수
executor.enable_parallel(num_threads=8, row_threshold=100_000)
executor.disable_parallel()

# SQL 실행
result = executor.execute("SELECT vwap(price, volume) FROM trades WHERE symbol = 1")

# 결과 필드
result.column_names       # list[str]
result.rows               # list[list[int]] — 모든 값을 int64로
result.execution_time_us  # float
result.rows_scanned       # int
result.error              # str — 정상이면 빈 문자열
result.ok()               # bool
```

---

## apex_py.connection — HTTP 클라이언트

포트 8123에서 실행 중인 `apex_server`에 연결합니다 (ClickHouse 호환 HTTP API).

```python
import apex_py as apex

# 연결
db = apex.connect("localhost", 8123)
db = apex.connect("localhost", 8123, api_key="apex_<64자 hex>")  # 인증 포함

# 헬스 체크
db.ping()   # → 서버 정상이면 True

# SQL → pandas DataFrame
df = db.query_pandas("SELECT symbol, avg(price) FROM trades GROUP BY symbol")

# SQL → polars DataFrame
df = db.query_polars("SELECT symbol, avg(price) FROM trades GROUP BY symbol")

# SQL → numpy 딕셔너리
arrays = db.query_numpy("SELECT price, volume FROM trades WHERE symbol = 1")
# Returns: dict[str, np.ndarray]

# pandas 인제스트 (HTTP 경유)
db.ingest_pandas(df, symbol_col="symbol", price_col="price", volume_col="volume")

# polars 인제스트 (HTTP 경유)
db.ingest_polars(df_polars)
```

---

## apex_py.dataframe — 대용량 인제스트/익스포트

HTTP 없이 직접 `apex.Pipeline` 객체를 사용하는 컨버터입니다.

```python
from apex_py import (
    from_pandas, from_polars, from_arrow,
    to_pandas, to_polars,
    query_to_pandas, query_to_polars,
)
```

### 인제스트

```python
# pandas에서 (벡터화, Python 행 루프 없음)
from_pandas(
    df, pipeline,
    symbol_col="symbol", price_col="price", volume_col="volume",
    timestamp_col="timestamp",   # 선택사항
    price_scale=1.0,             # float → int64 스케일 팩터
)

# polars에서 (Arrow 버퍼를 통한 제로카피)
from_polars(
    df, pipeline,
    symbol_col="symbol", price_col="price", volume_col="volume",
    price_scale=1.0,
)

# Arrow Table에서
from_arrow(
    table, pipeline,
    symbol_col="symbol", price_col="price", volume_col="volume",
)
```

### 익스포트

```python
# Pipeline → pandas DataFrame
df = to_pandas(pipeline, symbol=1)

# Pipeline → polars DataFrame
df = to_polars(pipeline, symbol=1)

# SQL → pandas
df = query_to_pandas(pipeline, "SELECT avg(price) FROM trades WHERE symbol = 1")

# SQL → polars
df = query_to_polars(pipeline, "SELECT symbol, sum(volume) FROM trades GROUP BY symbol")
```

### 성능 (100만 행)

| 방법 | 처리량 |
|------|--------|
| `from_polars()` | ~330만 행/초 |
| `from_pandas()` | ~200만 행/초 |
| `from_arrow()` | ~300만 행/초 |

---

## apex_py.arrow — Arrow / DuckDB 상호운용

```python
from apex_py import ArrowSession
sess = ArrowSession(pipeline)

# --- 인제스트 ---
sess.ingest_arrow(table, symbol_col="symbol", price_col="price", volume_col="volume")
sess.ingest_record_batch(batch)
sess.ingest_arrow_columnar(symbols=sym_array, prices=px_array, volumes=vol_array)

# --- 익스포트 ---
table  = sess.to_arrow(symbol=1)               # pa.Table (제로카피)
reader = sess.to_record_batch_reader(symbol=1) # 스트리밍
df     = sess.to_polars_zero_copy(symbol=1)    # pl.DataFrame (제로카피)
conn   = sess.to_duckdb(symbol=1, table_name="trades")
result = conn.execute("SELECT avg(price) FROM trades").fetchdf()

schema = sess.get_schema()   # → pa.Schema
```

---

## apex_py.streaming — 고처리량 인제스트

```python
from apex_py import StreamingSession

sess = StreamingSession(
    pipeline,
    batch_size=50_000,      # C++ 호출당 행 수
    error_mode="skip",      # "skip" | "raise" | "warn"
)

sess.ingest_pandas(df, show_progress=True,
                   symbol_col="symbol", price_col="price", volume_col="volume")
# → Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)

sess.ingest_polars(df_polars, show_progress=True)

# 제너레이터에서 인제스트
def tick_gen():
    for row in external_feed:
        yield {"symbol": row.sym, "price": row.px, "volume": row.vol}

sess.ingest_iter(tick_gen(), show_progress=True, total=1_000_000)

# 통계
stats = sess.stats()
# stats.rows_ingested, rows_skipped, batches, elapsed_sec, throughput
```

에러 모드:

| 모드 | 동작 |
|------|------|
| `"skip"` | 잘못된 행 건너뛰고 계속 |
| `"raise"` | 첫 번째 오류에서 예외 발생 |
| `"warn"` | 경고 출력 후 계속 |

---

## 상호운용성 매트릭스

| 출발 \ 목적지 | pandas | polars | numpy | Arrow | DuckDB | HTTP |
|---------------|--------|--------|-------|-------|--------|------|
| **APEX-DB (인프로세스)** | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` | — |
| **APEX-DB (HTTP)** | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — | `POST /` |
| **pandas → APEX** | `from_pandas()` | — | — | — | — | `ingest_pandas()` |
| **polars → APEX** | — | `from_polars()` | — | — | — | `ingest_polars()` |
| **Arrow → APEX** | — | — | — | `ingest_arrow()` | — | — |

---

*참고: [SQL 레퍼런스](SQL_REFERENCE_ko.md) · [C++ 레퍼런스](CPP_REFERENCE_ko.md) · [HTTP 레퍼런스](HTTP_REFERENCE_ko.md)*
