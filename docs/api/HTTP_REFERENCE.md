# APEX-DB HTTP API Reference

*Last updated: 2026-03-22*

The HTTP server (port 8123) is **ClickHouse-compatible**. Grafana can connect directly
using the ClickHouse data source plugin with no modification.

---

## Table of Contents

- [Endpoints](#endpoints)
- [SQL Query — POST /](#sql-query--post-)
- [Response Format](#response-format)
- [Authentication](#authentication)
- [/stats](#stats)
- [/metrics (Prometheus)](#metrics-prometheus)
- [Error Responses](#error-responses)
- [Roles & Permissions](#roles--permissions)

---

## Quick Start

### Start the server

```bash
# Build (see README for full build instructions)
cd build && ninja -j$(nproc)

# Start with default settings (port 8123, no auth)
./apex_server --port 8123

# Start with TLS + auth enabled
./apex_server --port 8123 --tls-cert server.crt --tls-key server.key
```

### Run your first query

```bash
# Health check
curl http://localhost:8123/ping
# Ok

# Simple aggregation
curl -s -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume) AS vwap, count(*) AS n FROM trades WHERE symbol = 1'
# {"columns":["vwap","n"],"data":[[15037.2,1000]],"rows":1,"execution_time_us":52.3}
```

### Common query patterns

```bash
# 5-minute OHLCV bars
curl -s -X POST http://localhost:8123/ -d '
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close, sum(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000) ORDER BY bar ASC
' | python3 -m json.tool

# Volume by symbol
curl -s -X POST http://localhost:8123/ \
  -d 'SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol ORDER BY symbol'

# Last 10 minutes of trades for symbol 1
curl -s -X POST http://localhost:8123/ \
  -d "SELECT price, volume, epoch_s(timestamp) AS ts FROM trades
      WHERE symbol = 1 AND timestamp > $(date +%s)000000000 - 600000000000
      ORDER BY timestamp DESC LIMIT 100"
```

### With authentication

```bash
# Generate and store an API key (admin role, server-side tool)
./apex_server --gen-key --role admin
# apex_a1b2c3d4e5f6...  (64 hex chars)

# Use the key
export APEX_KEY="apex_a1b2c3d4e5f6..."

curl -s -X POST http://localhost:8123/ \
  -H "Authorization: Bearer $APEX_KEY" \
  -d 'SELECT count(*) FROM trades'
```

### Python client (apex_py)

```python
import apex_py as apex

db = apex.connect("localhost", 8123)

# DataFrame results
df = db.query_pandas("SELECT symbol, avg(price) FROM trades GROUP BY symbol")
print(df)
```

---

## Endpoints

| Method | Path | Auth required | Description |
|--------|------|:---:|-------------|
| `POST` | `/` | yes | Execute SQL query |
| `GET` | `/ping` | no | Health check — returns `"Ok\n"` |
| `GET` | `/health` | no | Kubernetes liveness probe |
| `GET` | `/ready` | no | Kubernetes readiness probe |
| `GET` | `/stats` | yes | Pipeline statistics (JSON) |
| `GET` | `/metrics` | yes | Prometheus OpenMetrics |

Public paths (`/ping`, `/health`, `/ready`) are always exempt from authentication.

---

## SQL Query — POST /

Send a SQL string as the request body. Content-Type is not required.

```bash
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1'
```

```bash
# GROUP BY query
curl -X POST http://localhost:8123/ \
  -d 'SELECT symbol, sum(volume) AS vol FROM trades GROUP BY symbol ORDER BY symbol'
```

```bash
# Multi-line SQL (use single quotes or heredoc)
curl -X POST http://localhost:8123/ -d '
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,   last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
ORDER BY bar ASC
'
```

---

## Response Format

All responses are JSON.

### Success

```json
{
  "columns": ["vwap(price, volume)", "count(*)"],
  "data": [[15037.2, 1000]],
  "rows": 1,
  "execution_time_us": 52.3
}
```

| Field | Type | Description |
|-------|------|-------------|
| `columns` | `string[]` | Column names in SELECT order |
| `data` | `int64[][]` | Row-major result data. All values are int64. |
| `rows` | `int` | Number of result rows |
| `execution_time_us` | `float` | Query execution time in microseconds |

> **Note:** Prices and timestamps are int64 in the response. Apply your scale factor client-side (e.g. divide by 100 for cents-to-dollars).

### Multi-row example

```json
{
  "columns": ["symbol", "vol"],
  "data": [[1, 104500], [2, 101000]],
  "rows": 2,
  "execution_time_us": 88.1
}
```

---

## Authentication

When `APEX_TLS_ENABLED` is compiled in, the server requires authentication on all
non-public paths.

### API Key

Format: `apex_` followed by 64 hex characters (256-bit entropy).

```bash
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer apex_a1b2c3d4...64hexchars" \
  -d 'SELECT count(*) FROM trades'
```

### JWT (HS256 or RS256)

```bash
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..." \
  -d 'SELECT count(*) FROM trades'
```

JWT claims used:
- `sub` — subject (user identifier)
- `role` — APEX role string: `admin`, `writer`, `reader`, `analyst`, `metrics`
- `exp` — expiration timestamp

### Priority

If the `Authorization` header starts with `ey`, JWT validation is tried first.
If JWT fails, API key validation is attempted as fallback.

---

## /stats

Returns pipeline operational statistics as JSON.

```bash
curl http://localhost:8123/stats \
  -H "Authorization: Bearer apex_..."
```

```json
{
  "ticks_ingested": 5000000,
  "ticks_stored": 4999800,
  "ticks_dropped": 200,
  "queries_executed": 12345,
  "total_rows_scanned": 50000000,
  "partitions_created": 10,
  "last_ingest_latency_ns": 181
}
```

| Field | Description |
|-------|-------------|
| `ticks_ingested` | Total ticks pushed into the ring buffer |
| `ticks_stored` | Ticks successfully written to column store |
| `ticks_dropped` | Ticks dropped due to ring buffer overflow |
| `queries_executed` | Total SQL queries executed |
| `total_rows_scanned` | Cumulative rows scanned across all queries |
| `partitions_created` | Number of partitions allocated |
| `last_ingest_latency_ns` | Latency of the most recent ingest (nanoseconds) |

---

## /metrics (Prometheus)

Returns OpenMetrics-format text for Prometheus scraping.

```bash
curl http://localhost:8123/metrics
```

```
# HELP apex_ticks_ingested_total Total ticks ingested
# TYPE apex_ticks_ingested_total counter
apex_ticks_ingested_total 5000000

# HELP apex_ticks_stored_total Total ticks stored to column store
# TYPE apex_ticks_stored_total counter
apex_ticks_stored_total 4999800

# HELP apex_ticks_dropped_total Total ticks dropped (queue overflow)
# TYPE apex_ticks_dropped_total counter
apex_ticks_dropped_total 200

# HELP apex_queries_executed_total Total SQL queries executed
# TYPE apex_queries_executed_total counter
apex_queries_executed_total 12345

# HELP apex_rows_scanned_total Total rows scanned
# TYPE apex_rows_scanned_total counter
apex_rows_scanned_total 50000000

# HELP apex_partitions_total Total partitions created
# TYPE apex_partitions_total gauge
apex_partitions_total 10

# HELP apex_last_ingest_latency_ns Last ingest latency in nanoseconds
# TYPE apex_last_ingest_latency_ns gauge
apex_last_ingest_latency_ns 181
```

### Grafana setup

1. Add a ClickHouse data source in Grafana
2. Host: `localhost`, Port: `8123`
3. Protocol: HTTP (or HTTPS with TLS)
4. No database required — APEX uses `trades` and `quotes` table names directly in SQL

---

## Error Responses

```json
{
  "columns": [],
  "data": [],
  "rows": 0,
  "execution_time_us": 0,
  "error": "Parse error: unexpected token 'FORM' at position 7"
}
```

Common error strings:

| Error | Cause |
|-------|-------|
| `"Parse error: ..."` | SQL syntax error |
| `"Unknown table: foo"` | Table not found |
| `"Query cancelled"` | Cancelled via CancellationToken |
| `"Unauthorized"` | Missing or invalid credentials |
| `"Forbidden"` | Valid credentials but insufficient role |

HTTP status codes: `200` on success (even for SQL errors — check `error` field), `401` for auth failure, `403` for permission denied.

---

## Roles & Permissions

| Role | Query | Ingest | Stats | Metrics | Admin |
|------|:-----:|:------:|:-----:|:-------:|:-----:|
| `admin` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `writer` | ✅ | ✅ | ✅ | ❌ | ❌ |
| `reader` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `analyst` | ✅ | ❌ | ✅ | ✅ | ❌ |
| `metrics` | ❌ | ❌ | ✅ | ✅ | ❌ |

---

*See also: [SQL Reference](SQL_REFERENCE.md) · [Python Reference](PYTHON_REFERENCE.md) · [C++ Reference](CPP_REFERENCE.md)*
