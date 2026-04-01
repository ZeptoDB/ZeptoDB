# ZeptoDB Quick Start

*From zero to first query in 5 minutes.*

---

## 1. Start the Server

### Option A: Docker (recommended)

```bash
docker run -p 8123:8123 -p 8815:8815 zeptodb/zeptodb:latest --demo
```

The `--demo` flag preloads sample trade/quote data so you can query immediately.

### Option B: Build from source

```bash
git clone https://github.com/zeptodb/zeptodb.git && cd zeptodb
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)
./zepto_server --port 8123 --demo
```

---

## 2. Verify It's Running

```bash
curl http://localhost:8123/ping
# Ok
```

---

## 3. Insert Data

Skip this if you used `--demo` — sample data is already loaded.

```bash
curl -s -X POST http://localhost:8123/ -d "
CREATE TABLE IF NOT EXISTS trades (
  symbol STRING, price INT64, volume INT64, timestamp INT64
)"

curl -s -X POST http://localhost:8123/ -d "
INSERT INTO trades VALUES
  ('AAPL', 18750, 100, 1711234567000000000),
  ('AAPL', 18760, 200, 1711234568000000000),
  ('AAPL', 18755, 150, 1711234569000000000),
  ('GOOG', 17800, 80,  1711234567000000000),
  ('GOOG', 17810, 120, 1711234568000000000)
"
```

---

## 4. Query

```bash
# VWAP by symbol
curl -s -X POST http://localhost:8123/ \
  -d "SELECT symbol, vwap(price, volume) AS vwap, count(*) AS n FROM trades GROUP BY symbol"

# 5-minute OHLCV bars
curl -s -X POST http://localhost:8123/ -d "
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS vol
FROM trades WHERE symbol = 'AAPL'
GROUP BY xbar(timestamp, 300000000000)
"

# EMA with delta
curl -s -X POST http://localhost:8123/ -d "
SELECT symbol, price,
       EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20,
       DELTA(price) OVER (ORDER BY timestamp) AS change
FROM trades
"
```

---

## 5. Connect from Python

```bash
pip install zeptodb pyarrow
```

### Via Arrow Flight (recommended for remote)

```python
import pyarrow.flight as fl

client = fl.connect("grpc://localhost:8815")
table = client.do_get(fl.Ticket(
    "SELECT symbol, vwap(price, volume) AS vwap FROM trades GROUP BY symbol"
)).read_all()
print(table.to_pandas())
```

### Via HTTP client

```python
import zepto_py as zp

db = zp.connect("localhost", 8123)
df = db.query_pandas("SELECT * FROM trades WHERE symbol = 'AAPL' ORDER BY timestamp")
print(df)
```

### In-process (maximum performance)

```python
import zeptodb
from zepto_py import from_polars, query_to_pandas
import polars as pl

pipeline = zeptodb.Pipeline()
pipeline.start()

df = pl.DataFrame({
    "symbol": [1, 1, 2], "price": [15000, 15010, 20000],
    "volume": [100, 150, 80],
})
from_polars(df, pipeline, symbol_col="symbol", price_col="price", volume_col="volume")
pipeline.drain()

result = query_to_pandas(pipeline, "SELECT symbol, vwap(price, volume) AS vwap FROM trades GROUP BY symbol")
print(result)
pipeline.stop()
```

---

## 6. Open the Web UI

Navigate to [http://localhost:8123](http://localhost:8123) in your browser.

The built-in query editor supports:
- SQL autocomplete (schema-aware + ZeptoDB functions)
- Multi-tab editor with independent results
- Chart view (line, bar, candlestick)
- Export to CSV/JSON
- Query history

---

## Next Steps

| What | Link |
|------|------|
| Full SQL syntax | [SQL Reference](../api/SQL_REFERENCE.md) |
| Python API | [Python Reference](../api/PYTHON_REFERENCE.md) |
| Arrow Flight | [Flight Reference](../api/FLIGHT_REFERENCE.md) |
| HTTP endpoints | [HTTP Reference](../api/HTTP_REFERENCE.md) |
| Configuration | [Config Reference](../api/CONFIG_REFERENCE.md) |
| Production deployment | [Deployment Guide](../deployment/PRODUCTION_DEPLOYMENT.md) |
| Migrate from kdb+/ClickHouse | [Migration](#) |
