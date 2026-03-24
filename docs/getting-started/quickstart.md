# Quick Start

## SQL via HTTP (ClickHouse Compatible)

```bash
# Start server
./zepto_server --port 8123

# Query via curl
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1'
```

Grafana can connect directly as a ClickHouse data source.

## Python DSL (Zero-Copy)

```python
import zeptodb
from zepto_py.dsl import DataFrame

db = zeptodb.Pipeline()
db.start()

# Ingest ticks
db.ingest(symbol=1, price=15000, volume=100)
db.drain()

# zero-copy numpy (522ns)
prices = db.get_column(symbol=1, name="price")

# Lazy DSL (Polars-style)
df = DataFrame(db, symbol=1)
ma20 = df['price'].rolling(20).mean().collect()
```

## SQL Examples

### OHLCV Candlestick (xbar)

```sql
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
```

### EMA + DELTA/RATIO

```sql
SELECT symbol, price,
       EMA(price, 20) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema20,
       DELTA(price) OVER (ORDER BY timestamp) AS price_change
FROM trades
```

### ASOF JOIN

```sql
SELECT t.price, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q
ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
```

For the full SQL reference, see [SQL Reference](../api/SQL_REFERENCE.md).
