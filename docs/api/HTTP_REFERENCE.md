# ZeptoDB HTTP API Reference

*Last updated: 2026-03-25*

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
- [Admin API](#admin-api)
  - [API Keys](#admin-api-keys)
  - [Active Queries](#admin-active-queries)
  - [Audit Log](#admin-audit-log)
  - [Sessions](#admin-sessions)
  - [Cluster Nodes](#admin-cluster-nodes)
  - [Cluster Overview](#admin-cluster-overview)
  - [Metrics History](#admin-metrics-history)
  - [Rebalance](#admin-rebalance)
  - [Version](#admin-version)
- [Roles & Permissions](#roles--permissions)

> Enterprise security guide: [Security Operations Guide](SECURITY_OPERATIONS_GUIDE.md) · [SSO Integration Guide](SSO_INTEGRATION_GUIDE.md)

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
| `GET` | `/admin/sessions` | admin | Active client sessions |
| `GET` | `/admin/version` | admin | Server version info |
| `GET` | `/admin/nodes` | admin | Cluster node status |
| `POST` | `/admin/nodes` | admin | Add remote node to cluster |
| `DELETE` | `/admin/nodes/:id` | admin | Remove node from cluster |
| `GET` | `/admin/cluster` | admin | Cluster overview |
| `GET` | `/admin/metrics/history` | admin | Metrics time-series history |
| `GET` | `/admin/rebalance/status` | admin | Current rebalance status |
| `POST` | `/admin/rebalance/start` | admin | Start rebalance (add/remove node) |
| `POST` | `/admin/rebalance/pause` | admin | Pause current rebalance |
| `POST` | `/admin/rebalance/resume` | admin | Resume paused rebalance |
| `POST` | `/admin/rebalance/cancel` | admin | Cancel current rebalance |
| `GET` | `/admin/rebalance/history` | admin | Past rebalance events (up to 50, most recent last) |

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

HTTP status codes: `200` on success (even for SQL errors — check `error` field), `401` for auth failure, `403` for permission denied, `408` for query timeout/cancelled.

---

## Admin API

All admin endpoints require the `admin` role. Returns `401` without credentials, `403` with non-admin credentials.

### Admin: API Keys

#### `GET /admin/keys` — List all API keys

```bash
curl http://localhost:8123/admin/keys -H "Authorization: Bearer $ADMIN_KEY"
```

```json
[
  {
    "id": "ak_7f3k8a2b",
    "name": "trading-desk-1",
    "role": "writer",
    "enabled": true,
    "created_at_ns": 1711234567000000000,
    "last_used_ns": 1711234590000000000,
    "expires_at_ns": 0,
    "tenant_id": "hft_desk_1",
    "allowed_symbols": ["AAPL", "MSFT"],
    "allowed_tables": ["trades", "quotes"]
  }
]
```

#### `POST /admin/keys` — Create API key

```bash
curl -X POST http://localhost:8123/admin/keys \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"name":"algo-service","role":"writer","symbols":["AAPL"],"tables":["trades"],"tenant_id":"desk_1","expires_at_ns":1743465600000000000}'
```

All fields except `name` are optional:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | (required) | Human-readable label |
| `role` | string | `"reader"` | `admin` / `writer` / `reader` / `analyst` / `metrics` |
| `symbols` | string[] | `[]` | Symbol whitelist (empty = unrestricted) |
| `tables` | string[] | `[]` | Table whitelist (empty = unrestricted) |
| `tenant_id` | string | `""` | Bind key to a tenant |
| `expires_at_ns` | int64 | `0` | Expiry timestamp in nanoseconds (0 = never) |

```json
{"key": "zepto_a1b2c3d4e5f6..."}
```

The full key is shown exactly once. Store it securely.

#### `PATCH /admin/keys/:id` — Update API key

Update mutable fields of an existing key. Only provided fields are modified.

```bash
curl -X PATCH http://localhost:8123/admin/keys/ak_7f3k8a2b \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"symbols":["AAPL","GOOG"],"enabled":true,"tenant_id":"desk_2","expires_at_ns":0}'
```

| Field | Type | Description |
|-------|------|-------------|
| `symbols` | string[] | Replace symbol whitelist |
| `tables` | string[] | Replace table whitelist |
| `enabled` | bool | Enable/disable key |
| `tenant_id` | string | Change tenant binding |
| `expires_at_ns` | int64 | Change expiry (0 = never) |

```json
{"updated": true}
```

#### `DELETE /admin/keys/:id` — Revoke API key

```bash
curl -X DELETE http://localhost:8123/admin/keys/ak_7f3k8a2b \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"revoked": true}
```

---

### Admin: Auth / JWKS

#### `POST /admin/auth/reload` — Force refresh JWKS keys

Triggers an immediate re-fetch of the JWKS endpoint. Useful after IdP key rotation.

```bash
curl -X POST http://localhost:8123/admin/auth/reload \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"refreshed": true, "keys_loaded": 2}
```

Returns `502` if no JWKS URL is configured or the fetch fails.

---

### SSO / OAuth2 Endpoints

> Full guide: [SSO Integration Guide](SSO_INTEGRATION_GUIDE.md)

#### `GET /auth/login` — Redirect to IdP

Redirects the browser to the configured OIDC authorization endpoint. No authentication required.

```
GET /auth/login → 302 https://idp.example.com/authorize?response_type=code&client_id=...
```

Returns `503` if OIDC is not configured.

#### `GET /auth/callback` — OAuth2 code exchange

Handles the IdP redirect after user authentication. Exchanges the authorization code for tokens, resolves identity, creates a server-side session, and redirects to `/query`.

Query params: `code` (required), `state` (optional)

Returns `502` if token exchange fails, `401` if identity resolution fails.

#### `POST /auth/session` — Create session from Bearer token

Creates a server-side session from an existing Bearer token (API key or JWT). Returns a `Set-Cookie` header.

```bash
curl -X POST http://localhost:8123/auth/session \
  -H "Authorization: Bearer $TOKEN"
```

```json
{"session": true, "role": "writer", "subject": "user@example.com"}
```

#### `POST /auth/logout` — Destroy session

Destroys the server-side session and clears the cookie. No authentication required.

```bash
curl -X POST http://localhost:8123/auth/logout -b "zepto_sid=SESSION_ID"
```

```json
{"ok": true}
```

#### `POST /auth/refresh` — Refresh OAuth2 token

Refreshes the OAuth2 access token using the stored refresh token. Requires a valid session cookie.

```bash
curl -X POST http://localhost:8123/auth/refresh -b "zepto_sid=SESSION_ID"
```

```json
{"refreshed": true, "expires_in": 3600}
```

Returns `401` if the session is invalid or refresh fails.

#### `GET /auth/me` — Current identity

Returns the authenticated identity from session cookie or Bearer token.

```bash
curl http://localhost:8123/auth/me -b "zepto_sid=SESSION_ID"
# or
curl http://localhost:8123/auth/me -H "Authorization: Bearer $TOKEN"
```

```json
{"subject": "user@example.com", "role": "writer", "source": "sso:okta-prod"}
```

---

### Admin: Active Queries

#### `GET /admin/queries` — List running queries

```bash
curl http://localhost:8123/admin/queries -H "Authorization: Bearer $ADMIN_KEY"
```

```json
[
  {
    "id": "q_a1b2c3",
    "subject": "ak_7f3k8a2b",
    "sql": "SELECT * FROM trades WHERE...",
    "started_at_ns": 1711234567000000000
  }
]
```

#### `DELETE /admin/queries/:id` — Kill a running query

Cancels the query at the next partition scan boundary via `CancellationToken`.

```bash
curl -X DELETE http://localhost:8123/admin/queries/q_a1b2c3 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"cancelled": true}
```

---

### Admin: Audit Log

#### `GET /admin/audit` — Recent audit events

Query parameter: `?n=<count>` (default: 100).

```bash
curl "http://localhost:8123/admin/audit?n=50" -H "Authorization: Bearer $ADMIN_KEY"
```

```json
[
  {
    "ts": 1711234567000000000,
    "subject": "ak_7f3k8a2b",
    "role": "writer",
    "action": "query",
    "detail": "SELECT count(*) FROM trades",
    "from": "10.0.1.5"
  }
]
```

---

### Admin: Sessions

#### `GET /admin/sessions` — Active client sessions

Lists connected clients (equivalent to kdb+ `.z.po`).

```bash
curl http://localhost:8123/admin/sessions -H "Authorization: Bearer $ADMIN_KEY"
```

```json
[
  {
    "remote_addr": "10.0.1.5",
    "user": "ak_7f3k8a2b",
    "connected_at_ns": 1711234567000000000,
    "last_active_ns": 1711234590000000000,
    "query_count": 42
  }
]
```

---

### Admin: Cluster Nodes

#### `GET /admin/nodes` — List cluster nodes

```bash
curl http://localhost:8123/admin/nodes -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{
  "nodes": [
    {
      "id": 1,
      "host": "10.0.1.1",
      "port": 8123,
      "state": "ACTIVE",
      "ticks_ingested": 5000000,
      "ticks_stored": 4999800,
      "queries_executed": 12345
    }
  ]
}
```

Node states: `ACTIVE`, `SUSPECT`, `DEAD`, `JOINING`, `LEAVING`.

#### `POST /admin/nodes` — Add remote node at runtime

```bash
curl -X POST http://localhost:8123/admin/nodes \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"id":4,"host":"10.0.1.4","port":8123}'
```

```json
{"added": true, "id": 4, "host": "10.0.1.4", "port": 8123}
```

Requires cluster mode (`set_coordinator()` must be called). Idempotent — adding an existing node ID is a no-op.

#### `DELETE /admin/nodes/:id` — Remove node from cluster

```bash
curl -X DELETE http://localhost:8123/admin/nodes/4 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"removed": true, "id": 4}
```

The node is removed from the routing table immediately. Safe to call for non-existent IDs.

---

### Admin: Cluster Overview

#### `GET /admin/cluster` — Cluster summary

```bash
curl http://localhost:8123/admin/cluster -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{
  "mode": "standalone",
  "node_count": 1,
  "partitions_created": 10,
  "partitions_evicted": 0,
  "ticks_ingested": 5000000,
  "ticks_stored": 4999800,
  "ticks_dropped": 200,
  "queries_executed": 12345,
  "total_rows_scanned": 50000000,
  "last_ingest_latency_ns": 181
}
```

---

### Admin: Metrics History

#### `GET /admin/metrics/history` — Time-series metrics

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

### Admin: Rebalance

Live partition rebalancing control. Requires `set_rebalance_manager()` to be called on the server. Returns `503` if rebalance is not available (standalone mode without RebalanceManager).

#### `GET /admin/rebalance/status` — Current rebalance status

```bash
curl http://localhost:8123/admin/rebalance/status -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{
  "state": "RUNNING",
  "total_moves": 10,
  "completed_moves": 3,
  "failed_moves": 0,
  "current_symbol": "42"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | `IDLE`, `RUNNING`, `PAUSED`, or `CANCELLING` |
| `total_moves` | int | Total partition moves planned |
| `completed_moves` | int | Moves successfully committed |
| `failed_moves` | int | Moves that failed (after retries) |
| `current_symbol` | string | Symbol currently being migrated (empty if idle) |

#### `POST /admin/rebalance/start` — Start rebalance

```bash
# Scale-out: add a new node
curl -X POST http://localhost:8123/admin/rebalance/start \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"action":"add_node","node_id":4}'

# Scale-in: remove a node
curl -X POST http://localhost:8123/admin/rebalance/start \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"action":"remove_node","node_id":2}'
```

| Field | Type | Description |
|-------|------|-------------|
| `action` | string | `"add_node"`, `"remove_node"`, or `"move_partitions"` |
| `node_id` | int | Target node ID (required for `add_node`/`remove_node`) |
| `moves` | array | Array of `{symbol, from, to}` objects (required for `move_partitions`) |

**Partial move example:**

```bash
# Move specific partitions between existing nodes
curl -X POST http://localhost:8123/admin/rebalance/start \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"action":"move_partitions","moves":[{"symbol":42,"from":1,"to":2},{"symbol":99,"from":2,"to":3}]}'
```

Success: `{"ok": true}`
Already running: `{"ok": false, "error": "already running"}`

#### `POST /admin/rebalance/pause` — Pause rebalance

Pauses after the current in-flight move completes.

```bash
curl -X POST http://localhost:8123/admin/rebalance/pause \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"ok": true}
```

#### `POST /admin/rebalance/resume` — Resume rebalance

Resumes a paused rebalance.

```bash
curl -X POST http://localhost:8123/admin/rebalance/resume \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"ok": true}
```

#### `POST /admin/rebalance/cancel` — Cancel rebalance

Cancels the current rebalance. Already-committed moves stay committed.

```bash
curl -X POST http://localhost:8123/admin/rebalance/cancel \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"ok": true}
```

---

#### GET /admin/rebalance/history

Returns an array of past rebalance events:

```json
[
  {
    "action": "add_node",
    "node_id": 4,
    "total_moves": 3,
    "completed_moves": 3,
    "failed_moves": 0,
    "start_time_ms": 1712930400000,
    "duration_ms": 1523,
    "cancelled": false
  }
]
```

| Field | Type | Description |
|-------|------|-------------|
| `action` | string | `add_node`, `remove_node`, or `move_partitions` |
| `node_id` | number | Target node (0 for move_partitions) |
| `total_moves` | number | Total partition moves planned |
| `completed_moves` | number | Successfully completed moves |
| `failed_moves` | number | Failed moves |
| `start_time_ms` | number | Start time (epoch milliseconds) |
| `duration_ms` | number | Total duration in milliseconds |
| `cancelled` | boolean | Whether the rebalance was cancelled |

---

### Admin: Version

#### `GET /admin/version` — Server version info

```bash
curl http://localhost:8123/admin/version -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"engine": "ZeptoDB", "version": "0.1.0", "build": "Mar 25 2026"}
```

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

## Rate Limiting

When rate limiting is enabled, requests that exceed the configured threshold return:

```
HTTP 403 Forbidden
{"error": "Rate limit exceeded"}
```

Rate limits are applied per-identity (API key ID or JWT `sub`) and per-IP. Both checks must pass.

See [Security Operations Guide — Rate Limiting](SECURITY_OPERATIONS_GUIDE.md#rate-limiting) for configuration details.

---

*See also: [Security Operations Guide](SECURITY_OPERATIONS_GUIDE.md) · [SSO Integration Guide](SSO_INTEGRATION_GUIDE.md) · [SQL Reference](SQL_REFERENCE.md) · [Config Reference](CONFIG_REFERENCE.md)*
