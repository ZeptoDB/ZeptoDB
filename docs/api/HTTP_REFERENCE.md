# ZeptoDB HTTP API Reference

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
./zepto_server --port 8123

# Start with TLS + auth enabled
./zepto_server --port 8123 --tls-cert server.crt --tls-key server.key
```

### Run your first query

```bash
# Health check
curl http://localhost:8123/ping
# Ok

# Simple aggregation (string symbol)
curl -s -X POST http://localhost:8123/ \
  -d "SELECT vwap(price, volume) AS vwap, count(*) AS n FROM trades WHERE symbol = 'AAPL'"
# {"columns":["vwap","n"],"data":[[15037.2,1000]],"rows":1,"execution_time_us":52.3}

# Integer symbol ID also supported
curl -s -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume) AS vwap, count(*) AS n FROM trades WHERE symbol = 1'
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
./zepto_server --gen-key --role admin
# zepto_a1b2c3d4e5f6...  (64 hex chars)

# Use the key
export APEX_KEY="zepto_a1b2c3d4e5f6..."

curl -s -X POST http://localhost:8123/ \
  -H "Authorization: Bearer $APEX_KEY" \
  -d 'SELECT count(*) FROM trades'
```

### Python client (zepto_py)

```python
import zepto_py as apex

db = zeptodb.connect("localhost", 8123)

# DataFrame results
df = db.query_pandas("SELECT symbol, avg(price) FROM trades GROUP BY symbol")
print(df)
```

---

## Endpoints

| Method | Path | Auth required | Description |
|--------|------|:---:|-------------|
| `POST` | `/` | yes | Execute SQL query |
| `GET` | `/` | yes | Execute SQL via `?query=` param |
| `GET` | `/ping` | no | Health check — returns `"Ok\n"` |
| `GET` | `/health` | no | Kubernetes liveness probe |
| `GET` | `/ready` | no | Kubernetes readiness probe |
| `GET` | `/whoami` | yes | Return authenticated role and subject |
| `GET` | `/stats` | yes | Pipeline statistics (JSON) |
| `GET` | `/metrics` | no | Prometheus OpenMetrics |
| `GET` | `/admin/keys` | admin | List API keys |
| `POST` | `/admin/keys` | admin | Create API key |
| `DELETE` | `/admin/keys/:id` | admin | Revoke API key |
| `GET` | `/admin/queries` | admin | List running queries |
| `DELETE` | `/admin/queries/:id` | admin | Kill a running query |
| `GET` | `/admin/audit` | admin | Audit log (last N events) |
| `GET` | `/admin/nodes` | admin | Cluster node status |
| `POST` | `/admin/nodes` | admin | Add remote node to cluster |
| `DELETE` | `/admin/nodes/:id` | admin | Remove node from cluster |
| `GET` | `/admin/cluster` | admin | Cluster overview |
| `GET` | `/admin/metrics/history` | admin | Metrics time-series history |

Public paths (`/ping`, `/health`, `/ready`) are always exempt from authentication.

Every response includes an `X-Request-Id` header for tracing (e.g., `X-Request-Id: r0001a3`).

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

Format: `zepto_` followed by 64 hex characters (256-bit entropy).

```bash
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer zepto_a1b2c3d4...64hexchars" \
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
  -H "Authorization: Bearer zepto_..."
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
# HELP zepto_ticks_ingested_total Total ticks ingested
# TYPE zepto_ticks_ingested_total counter
zepto_ticks_ingested_total 5000000

# HELP zepto_ticks_stored_total Total ticks stored to column store
# TYPE zepto_ticks_stored_total counter
zepto_ticks_stored_total 4999800

# HELP zepto_ticks_dropped_total Total ticks dropped (queue overflow)
# TYPE zepto_ticks_dropped_total counter
zepto_ticks_dropped_total 200

# HELP zepto_queries_executed_total Total SQL queries executed
# TYPE zepto_queries_executed_total counter
zepto_queries_executed_total 12345

# HELP zepto_rows_scanned_total Total rows scanned
# TYPE zepto_rows_scanned_total counter
zepto_rows_scanned_total 50000000

# HELP zepto_partitions_total Total partitions created
# TYPE zepto_partitions_total gauge
zepto_partitions_total 10

# HELP zepto_last_ingest_latency_ns Last ingest latency in nanoseconds
# TYPE zepto_last_ingest_latency_ns gauge
zepto_last_ingest_latency_ns 181
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

## /admin/metrics/history

Returns time-series metrics snapshots captured by the server's internal `MetricsCollector` (3-second interval, 1-hour ring buffer). No external infrastructure required — ZeptoDB monitors itself.

### Resource Protection

The metrics collector is designed to never impact the main trading workload:

| Protection | Detail |
|-----------|--------|
| Memory hard limit | 256 KB default (`max_memory_bytes`), ~3500 snapshots max |
| Fixed circular buffer | O(1) write, zero allocation after init, no `erase()` |
| Lock-free capture | Atomic write index, `memory_order_relaxed` reads of stats |
| SCHED_IDLE thread | Linux: only runs when no other thread wants CPU |
| Response limit | Max 600 snapshots per API response (30 min at 3s interval) |
| Client-side bound | Web UI requests `?since=<30min_ago>&limit=600` |

```bash
# All history (capped by response_limit=600)
curl http://localhost:8123/admin/metrics/history \
  -H "Authorization: Bearer $ADMIN_KEY"

# Since a specific epoch-ms, with explicit limit
curl "http://localhost:8123/admin/metrics/history?since=1711234567000&limit=100" \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
[
  {
    "timestamp_ms": 1711234567000,
    "node_id": 0,
    "ticks_ingested": 5000000,
    "ticks_stored": 4999800,
    "ticks_dropped": 200,
    "queries_executed": 12345,
    "total_rows_scanned": 50000000,
    "partitions_created": 10,
    "last_ingest_latency_ns": 181
  }
]
```

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ms` | `int64` | Epoch milliseconds when snapshot was taken |
| `node_id` | `uint16` | Node identifier (0 for standalone) |
| `ticks_ingested` | `uint64` | Cumulative ticks ingested at snapshot time |
| `ticks_stored` | `uint64` | Cumulative ticks stored |
| `ticks_dropped` | `uint64` | Cumulative ticks dropped |
| `queries_executed` | `uint64` | Cumulative queries executed |
| `total_rows_scanned` | `uint64` | Cumulative rows scanned |
| `partitions_created` | `uint64` | Cumulative partitions created |
| `last_ingest_latency_ns` | `int64` | Most recent ingest latency (ns) |

Query parameter: `?since=<epoch_ms>` — returns only snapshots with `timestamp_ms >= since`.
Query parameter: `?limit=<N>` — max snapshots to return (default: 600).

---

## Roles & Permissions

| Role | Query | Ingest | Stats | Metrics | Admin |
|------|:-----:|:------:|:-----:|:-------:|:-----:|
| `admin` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `writer` | ✅ | ✅ | ✅ | ❌ | ❌ |
| `reader` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `analyst` | ✅ | ❌ | ✅ | ✅ | ❌ |
| `metrics` | ❌ | ❌ | ✅ | ✅ | ❌ |

### Table-Level ACL

API keys can be restricted to specific tables. When `allowed_tables` is set,
queries against any other table return `403 Forbidden`.

```bash
# Create a key restricted to trades and quotes tables only
curl -X POST https://zepto:8443/admin/keys \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"name":"desk-1","role":"reader","tables":["trades","quotes"]}'

# This key can query trades and quotes, but not risk_positions
```

When `allowed_tables` is empty (default), the key has access to all tables.
Table ACL is enforced at the HTTP layer before SQL execution — the SQL parser
extracts the target table from the query and checks it against the key's whitelist.

| ACL Type | Scope | Example |
|----------|-------|---------|
| Symbol ACL | Row-level filter by symbol | `allowed_symbols: ["AAPL","GOOGL"]` |
| Table ACL | Table-level access control | `allowed_tables: ["trades","quotes"]` |

Both can be combined: a key with `allowed_tables: ["trades"]` and
`allowed_symbols: ["AAPL"]` can only query AAPL data from the trades table.

---

*See also: [SQL Reference](SQL_REFERENCE.md) · [Config Reference](CONFIG_REFERENCE.md) · [Python Reference](PYTHON_REFERENCE.md) · [C++ Reference](CPP_REFERENCE.md)*
