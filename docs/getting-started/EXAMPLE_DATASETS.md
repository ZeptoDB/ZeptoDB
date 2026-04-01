# Example Dataset Bundle

*Preloaded sample data so new users never face an empty database.*

---

## Overview

The `--demo` server flag loads realistic sample datasets on startup. This eliminates the "blank canvas" problem — users can immediately run meaningful queries.

```bash
./zepto_server --port 8123 --demo
# or
docker run -p 8123:8123 zeptodb/zeptodb:latest --demo
```

---

## Datasets

### `trades` — Equity tick data

5 symbols × ~20K ticks each = **100K rows**

| Column | Type | Example |
|--------|------|---------|
| symbol | STRING | `AAPL` |
| price | INT64 | `18750` (= $187.50) |
| volume | INT64 | `100` |
| timestamp | INT64 | `1711234567000000000` |

Symbols: `AAPL`, `GOOG`, `MSFT`, `TSLA`, `AMZN`

Price distribution: realistic random walk around each symbol's base price. Volume follows a U-shaped intraday pattern (high at open/close).

### `quotes` — Bid/ask book

5 symbols × ~40K quotes each = **200K rows**

| Column | Type | Example |
|--------|------|---------|
| symbol | STRING | `AAPL` |
| bid | INT64 | `18748` |
| ask | INT64 | `18752` |
| bid_size | INT64 | `500` |
| ask_size | INT64 | `300` |
| timestamp | INT64 | `1711234567000000000` |

Quotes arrive ~2× the rate of trades. Spread varies by symbol volatility.

### `sensors` — IoT telemetry

10 devices × ~5K readings each = **50K rows**

| Column | Type | Example |
|--------|------|---------|
| device_id | STRING | `sensor_01` |
| temperature | INT64 | `2350` (= 23.50°C) |
| humidity | INT64 | `6520` (= 65.20%) |
| timestamp | INT64 | `1711234567000000000` |

Devices: `sensor_01` through `sensor_10`. Temperature follows a sinusoidal daily pattern with noise. `sensor_07` has injected anomalies for detection demos.

---

## Starter Queries

Printed to stdout on `--demo` startup:

```
═══════════════════════════════════════════════════════
  ZeptoDB Demo Mode — Sample data loaded
  trades: 100K rows | quotes: 200K rows | sensors: 50K rows
═══════════════════════════════════════════════════════

  Try these queries:

  1. SELECT symbol, vwap(price, volume) AS vwap, count(*) AS n
     FROM trades GROUP BY symbol

  2. SELECT xbar(timestamp, 300000000000) AS bar,
            first(price) AS open, max(price) AS high,
            min(price) AS low, last(price) AS close
     FROM trades WHERE symbol = 'AAPL'
     GROUP BY xbar(timestamp, 300000000000) ORDER BY bar

  3. SELECT t.price, q.bid, q.ask
     FROM trades t ASOF JOIN quotes q
     ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
     WHERE t.symbol = 'TSLA' LIMIT 10

  Web UI: http://localhost:8123
═══════════════════════════════════════════════════════
```

---

## Data Generation

Demo data is generated deterministically (seeded PRNG) so results are reproducible across runs.

### Generation parameters

```yaml
demo:
  seed: 42
  trading_day: "2024-03-23"
  market_hours: "09:30-16:00 ET"
  symbols:
    AAPL: { base_price: 18750, volatility: 0.001 }
    GOOG: { base_price: 17800, volatility: 0.0012 }
    MSFT: { base_price: 42500, volatility: 0.0008 }
    TSLA: { base_price: 17200, volatility: 0.002 }
    AMZN: { base_price: 18100, volatility: 0.0011 }
  sensors:
    count: 10
    interval_sec: 15
    anomaly_device: "sensor_07"
    anomaly_count: 5
```

### Implementation location

- Data generator: `src/demo/demo_data_generator.cpp`
- Server flag handling: `tools/zepto_http_server.cpp` (`--demo`)
- Config schema: `include/zeptodb/demo/demo_config.h`

---

## Memory Footprint

| Dataset | Rows | Columns | Approx. Memory |
|---------|------|---------|----------------|
| trades | 100K | 4 | ~3.2 MB |
| quotes | 200K | 6 | ~9.6 MB |
| sensors | 50K | 4 | ~1.6 MB |
| **Total** | **350K** | | **~14.4 MB** |

Negligible overhead — safe to enable even on constrained environments.
