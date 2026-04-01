# Interactive Playground

*Try ZeptoDB in your browser — no installation required.*

---

## Overview

The ZeptoDB Playground is a browser-based SQL editor backed by a sandboxed server instance. Users can execute queries against preloaded sample datasets without installing anything.

**URL:** `https://play.zeptodb.io` (or self-hosted at `/playground`)

---

## Architecture

```
┌──────────────────────────────┐
│  Browser                     │
│  ┌────────────────────────┐  │
│  │  CodeMirror SQL Editor │  │
│  │  (same as Web UI)      │  │
│  └──────────┬─────────────┘  │
└─────────────┼────────────────┘
              │ POST /playground/query
              ▼
┌──────────────────────────────┐
│  Playground Proxy            │
│  - Session isolation         │
│  - Query timeout (5s max)    │
│  - Rate limit (30 req/min)   │
│  - Read-only + INSERT only   │
│  - Max 10K rows per result   │
└──────────┬───────────────────┘
           │
           ▼
┌──────────────────────────────┐
│  ZeptoDB Instance            │
│  --demo --read-mostly        │
│  Preloaded: trades, quotes,  │
│  sensors, logs               │
└──────────────────────────────┘
```

---

## Sandbox Constraints

| Constraint | Value | Reason |
|-----------|-------|--------|
| Query timeout | 5 seconds | Prevent resource abuse |
| Rate limit | 30 requests/min per session | Fair usage |
| Max result rows | 10,000 | Browser memory |
| Allowed DDL | `CREATE TABLE`, `DROP TABLE` (session-scoped) | Experimentation |
| Allowed DML | `INSERT`, `SELECT` | No UPDATE/DELETE on preloaded data |
| Session lifetime | 30 minutes idle | Resource cleanup |
| Max concurrent sessions | 100 | Server capacity |

---

## Preloaded Datasets

### `trades` — Stock tick data

| Column | Type | Description |
|--------|------|-------------|
| symbol | STRING | Ticker (`AAPL`, `GOOG`, `MSFT`, `TSLA`, `AMZN`) |
| price | INT64 | Price in 1/100 cents (e.g., 18750 = $187.50) |
| volume | INT64 | Share count |
| timestamp | INT64 | Nanosecond epoch |

~100K rows, 1 trading day, 5 symbols.

### `quotes` — Bid/ask quotes

| Column | Type | Description |
|--------|------|-------------|
| symbol | STRING | Ticker |
| bid | INT64 | Bid price |
| ask | INT64 | Ask price |
| bid_size | INT64 | Bid volume |
| ask_size | INT64 | Ask volume |
| timestamp | INT64 | Nanosecond epoch |

~200K rows, matching `trades` time range.

### `sensors` — IoT sensor readings

| Column | Type | Description |
|--------|------|-------------|
| device_id | STRING | Device identifier |
| temperature | INT64 | Celsius × 100 |
| humidity | INT64 | Percent × 100 |
| timestamp | INT64 | Nanosecond epoch |

~50K rows, 10 devices, 24 hours.

---

## Example Queries (shown in playground sidebar)

```sql
-- 1. VWAP by symbol
SELECT symbol, vwap(price, volume) AS vwap, count(*) AS trades
FROM trades GROUP BY symbol ORDER BY symbol

-- 2. 5-minute candlestick bars
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close,
       sum(volume) AS vol
FROM trades WHERE symbol = 'AAPL'
GROUP BY xbar(timestamp, 300000000000) ORDER BY bar

-- 3. ASOF JOIN — match trades to latest quote
SELECT t.symbol, t.price, t.volume, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp
WHERE t.symbol = 'AAPL' LIMIT 20

-- 4. EMA crossover signal
SELECT symbol, price,
       EMA(price, 10) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema10,
       EMA(price, 30) OVER (PARTITION BY symbol ORDER BY timestamp) AS ema30
FROM trades WHERE symbol = 'TSLA'

-- 5. IoT anomaly detection
SELECT device_id, temperature / 100.0 AS temp_c,
       AVG(temperature) OVER (PARTITION BY device_id ORDER BY timestamp ROWS 20 PRECEDING) / 100.0 AS avg_20,
       DELTA(temperature) OVER (PARTITION BY device_id ORDER BY timestamp) / 100.0 AS delta
FROM sensors WHERE device_id = 'sensor_01'
```

---

## Self-Hosted Deployment

```bash
# Single container with playground mode
docker run -p 8123:8123 zeptodb/zeptodb:latest \
  --demo --playground \
  --playground-timeout-sec 5 \
  --playground-rate-limit 30 \
  --playground-max-rows 10000
```

Or via config:

```yaml
server:
  port: 8123

playground:
  enabled: true
  timeout_sec: 5
  rate_limit_per_min: 30
  max_result_rows: 10000
  session_ttl_min: 30
  max_sessions: 100
```

---

## Implementation Notes

- Reuses the existing Web UI query editor component (`web/src/app/query/page.tsx`)
- Playground proxy is a thin middleware that enforces constraints before forwarding to the ZeptoDB HTTP API
- Session isolation via ephemeral namespaces — each session gets its own table namespace for DDL
- Preloaded datasets are read-only shared memory across all sessions
