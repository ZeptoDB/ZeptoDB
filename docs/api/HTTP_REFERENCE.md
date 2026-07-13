# ZeptoDB HTTP API Reference

*Last updated: 2026-07-04*

The HTTP server (port 8123) is **ClickHouse-compatible**. Grafana can connect directly
using the ClickHouse data source plugin with no modification.

---

## Table of Contents

- [Endpoints](#endpoints)
- [AI Agent Memory API](#ai-agent-memory-api)
- [SQL Query — POST /](#sql-query--post-)
- [Arrow IPC Ingest — POST /insert/arrow](#arrow-ipc-ingest--post-insertarrow)
- [MessagePack Columnar Ingest — POST /insert/msgpack](#messagepack-columnar-ingest--post-insertmsgpack)
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

# Start with default settings (port 8123, no auth, in-memory)
./zepto_http_server --port 8123

# Persist catalog & HDB tier to disk (survives restart)
./zepto_http_server --port 8123 --hdb-dir /var/lib/zeptodb/hdb

# Start with TLS + auth enabled
./zepto_http_server --port 8123 --tls-cert server.crt --tls-key server.key
```

### Storage CLI flags

| Flag | Value | Effect |
|------|-------|--------|
| `--hdb-dir <path>` | directory path | Enables tiered storage (RDB + HDB) rooted at `<path>`. `_schema.json` and column files live under this dir. Implies `--storage-mode tiered`. |
| `--storage-mode <mode>` | `pure` (default) \| `tiered` | `pure` = in-memory only (fastest, lost on exit). `tiered` = persist to `--hdb-dir` or `/tmp/zepto_hdb` if unset. |
| `--agent-memory-dir <path>` | directory path | Enables Agent Memory sidecar persistence. Standalone mode writes directly under this path; routed mode writes under `node-{node_id}/shard-0/`. When HDB is enabled and this flag is omitted, defaults to `<hdb-dir>/agent_memory`. |
| `--agent-memory-flush-every N` | mutation count | Saves Agent Memory sidecar snapshots after `N` memory/cache mutations. `0` means flush only during server stop. Default: `100`. |
| `--agent-memory-max-memories N` | entry count | Maximum retained Agent Memory records. `0` means unbounded. Pinned memories are protected from capacity eviction. Default: `0`. |
| `--agent-memory-max-cache-entries N` | entry count | Maximum retained Agent Cache records. `0` means unbounded. Default: `0`. |
| `--agent-memory-replication-mode local\|routed\|quorum\|sync` | mode | Owner WAL replica acknowledgement policy. `local`/`routed` acknowledge after the owner-local commit marker. `quorum` waits for enough replica WAL ACKs to form a majority with the owner commit. `sync` waits for every configured replica before owner commit. Default: `routed`. |
| `--agent-memory-ann off\|auto\|sparse_projection\|hnsw\|ivf` | mode | Enables optional ANN candidate generation for memory search/context. `hnsw` requires `ZEPTO_ENABLE_HNSWLIB=ON`; `ivf` uses the dependency-free inverted-file baseline. Default: `off`. |
| `--agent-memory-ann-min-records N` | entry count | Auto-mode threshold before ANN is used. Default: `50000`. |
| `--agent-memory-ann-max-candidates N` | entry count | Maximum ANN candidates reranked per search. Default: `50000`. |
| `--agent-memory-ann-ivf-centroids N` | list count | IVF centroid/list count per tenant/namespace partition. Default: `256`. |
| `--agent-memory-ann-ivf-probe N` | list count | IVF lists probed per query. Default: `8`. |
| `--agent-memory-ring-epoch N` | integer | Initial routed Agent Memory ring epoch used in owner-scoped ids and remote RPC fencing. Default: `1`. |
| `--failover-enabled` | flag | In non-HA cluster mode, starts `HealthMonitor` + `FailoverManager` and wires Agent Memory deterministic owner failover. |
| `--health-heartbeat-port N` | port | UDP heartbeat port for `--failover-enabled`. Default: `9100`. |
| `--health-tcp-port N` | port | TCP heartbeat probe port for `--failover-enabled`. Default: `9101`. |
| `--health-suspect-ms N` | milliseconds | Time before a missing peer becomes `SUSPECT`. Default: `3000`. |
| `--health-dead-ms N` | milliseconds | Time before a missing peer becomes `DEAD` and triggers failover. Default: `10000`. |
| `--tenant <id:namespace>` | `tenant-id:prefix` | **(repeatable, devlog 091)** Register a tenant at startup with a table-namespace prefix. Any request carrying `X-Zepto-Tenant-Id: <id>` is then limited to tables whose names start with `<namespace>`. A tenant with an empty namespace is unrestricted. Example: `--tenant deska_:deska_` restricts tenant `deska_` to tables like `deska_trades`, `deska_quotes`. |

When `--hdb-dir` is supplied, `CREATE TABLE` DDL is persisted to
`<path>/_schema.json` on every call and reloaded on server start, so tables
survive restarts. See [devlog 086](../devlog/086_residual_limits_closed.md)
for the underlying mechanism.

### Tenant identity headers

| Header | Purpose |
|--------|---------|
| `X-Zepto-Tenant-Id` | Identifies the calling tenant. In authenticated mode this header is stamped automatically from the API key / JWT claim. In `--no-auth` mode the client supplies it directly. When present **and** the server was started with a matching `--tenant <id:namespace>`, every statement's touched table is checked against the namespace prefix — non-matching requests get `403 "Tenant 't' cannot access table 'x'"`. |
| `X-Zepto-Allowed-Tables` | Comma-separated list of tables the caller is permitted to touch (SELECT/INSERT/UPDATE/DELETE/CREATE/DROP/ALTER/DESCRIBE). Stamped from API key / JWT `allowed_tables` claim. |

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
import zepto_py as zepto

db = zepto.connect("localhost", 8123)

# DataFrame results
df = db.query_pandas("SELECT symbol, avg(price) FROM trades GROUP BY symbol")
print(df)
```

---

## Endpoints

| Method | Path | Auth required | Description |
|--------|------|:---:|-------------|
| `POST` | `/` | yes | Execute SQL query |
| `POST` | `/insert/arrow` | yes | Ingest Apache Arrow IPC RecordBatchStream rows |
| `POST` | `/insert/msgpack` | yes | Ingest MessagePack map-of-column-arrays rows |
| `GET` | `/` | yes | Execute SQL via `?query=` param |
| `GET` | `/ping` | no | Health check — returns `"Ok\n"` |
| `GET` | `/health` | no | Kubernetes liveness probe |
| `GET` | `/ready` | no | Kubernetes readiness probe |
| `GET` | `/whoami` | yes | Return authenticated role and subject |
| `GET` | `/stats` | yes | Pipeline statistics (JSON) |
| `GET` | `/metrics` | no | Prometheus OpenMetrics |
| `GET` | `/admin/keys` | admin | List API keys |
| `GET` | `/api/license` | no | Current license edition and features |
| `GET` | `/api/ai/stats` | yes | Agent Memory local or cluster counts, eviction counters, snapshot health, and retention config |
| `POST` | `/api/ai/memories` | yes | Store or update an agent memory |
| `GET` | `/api/ai/memories/:memory_id` | yes | Fetch one agent memory by id |
| `DELETE` | `/api/ai/memories/:memory_id` | yes | Delete one agent memory by id |
| `POST` | `/api/ai/memories/search` | yes | Filtered memory search with optional embedding similarity |
| `POST` | `/api/ai/context` | yes | Assemble ranked memories under a token budget |
| `POST` | `/api/ai/cache/store` | yes | Store an exact/semantic LLM cache entry |
| `DELETE` | `/api/ai/cache` | yes | Delete one exact prompt cache entry |
| `POST` | `/api/ai/cache/lookup` | yes | Exact prompt cache lookup with semantic fallback |
| `GET` | `/admin/license` | admin | Full license details |
| `POST` | `/admin/license` | admin | Upload license key |
| `POST` | `/admin/license/trial` | admin | Generate and load 30-day trial |
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
| `GET` | `/admin/edge-fleet-connector` | admin | Controlled-pilot Physical AI edge/fleet connector lifecycle status |
| `POST` | `/admin/edge-fleet-connector` | admin | Configure and optionally enable the controlled-pilot connector |
| `DELETE` | `/admin/edge-fleet-connector` | admin | Disable and clear the controlled-pilot connector config |
| `GET` | `/admin/action-outcome-supervisor` | admin | Experimental Physical AI Action-Outcome supervisor lifecycle status |
| `POST` | `/admin/action-outcome-supervisor` | admin | Configure and optionally enable the experimental supervisor |
| `DELETE` | `/admin/action-outcome-supervisor` | admin | Disable and clear the experimental supervisor config |
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

## AI Agent Memory API

The AI memory endpoints are an additive in-memory layer for agent working memory,
context retrieval, and application-level LLM cache hits. Embeddings are always
client-supplied `float32[]`; the server does not call embedding providers or LLMs.
Use `--agent-memory-dir <path>` to persist memory/cache records across server
restarts. When HDB is enabled, the default path is `<hdb-dir>/agent_memory`.
Standalone mode stores `records.bin` and `vectors.bin` directly in this
directory. Routed multi-node mode stores this pod's local shard at
`<path>/node-{node_id}/shard-0/` with a `manifest.json` that validates
`node_id`, `shard_id`, and `ring_epoch` before startup load. Mutations after the
last snapshot are appended to `wal.log` as prepared records plus commit markers,
replayed on restart only after commit, and truncated after the next successful
snapshot.
Snapshots flush after `--agent-memory-flush-every` mutations and again during
server stop. Set `--agent-memory-flush-every 1` for immediate write-through
snapshots, or `0` for stop-only snapshots with WAL replay in between.
Set `--agent-memory-replication-mode quorum` or `sync` in routed mode to copy
prepared WAL records and commit markers to configured Agent Memory replicas
before acknowledging memory/cache writes. Failed persisted writes are rolled
back from the live owner store for the primary upsert/delete and for any
write-triggered eviction side effects whose tombstones are not durable. Replicas
write those records under the source owner shard path and do not expose them in
live search until explicit shard adoption.

Bound memory growth with `--agent-memory-max-memories` and
`--agent-memory-max-cache-entries`. Capacity eviction removes expired entries
first, then the lowest-retention unpinned memories or least-retained cache
entries based on recency, importance, and access count. Pinned memories are
protected from capacity eviction, so the memory count can exceed the configured
limit when all remaining candidates are pinned. Owner-side HTTP/RPC writes
persist delete tombstones for automatic TTL, tenant-quota, and capacity
evictions they trigger, so WAL replay and later shard adoption preserve the
live owner's eviction state. If tombstone persistence fails after partial
success, the owner restores only the failed and later evicted entries before
returning an error, leaving already durable tombstones applied.

The default retrieval path uses exact filtered top-K scan and parallelizes large
full scans internally. Optional experimental ANN candidate generation can be
enabled with `--agent-memory-ann auto`, `--agent-memory-ann sparse_projection`,
in hnswlib-enabled builds, `--agent-memory-ann hnsw`, or
`--agent-memory-ann ivf`. ANN only selects semantic candidates; the final result
still applies
tenant/session/type filters, TTL checks, and the normal
pinned/importance/recency/access-count ranking. The index is derived state and is
rebuilt from memory vectors after snapshot load. Clean indexes maintain appends,
embedding updates, deletes, tombstones, and compacting row-id remaps
incrementally; if maintenance cannot be applied, search falls back to exact scan
until the background rebuild publishes a fresh index.

When `X-Zepto-Tenant-Id` is present it is copied into the request tenant scope. If
the JSON body also has `tenant_id` and it differs, the request returns `400`.

In multi-node routed mode, memory/cache writes and deletes route to the Agent
Memory owner node over internal `TcpRpc`. Remote write clients carry the
configured `ring_epoch`; when the owner data node has `TcpRpcServer` fencing
enabled, stale-epoch mutations are rejected. If persistence is enabled on the
owner node, the owner appends a prepared WAL record and commit marker before
acknowledging the write/delete. `GET /api/ai/memories/:memory_id` routes point
lookup to the memory owner, exact prompt cache lookup routes to the prompt owner,
and search/context fan out to Agent Memory nodes before merging the global
top-K. Semantic cache fallback also fans out to configured Agent Memory nodes
after the exact prompt owner misses.

### `GET /api/ai/stats`

Returns Agent Memory counts, eviction counters, embedding dimension, snapshot
health, last local owner-failover status, and the current eviction config. It
does not expose memory content, prompts, responses, or metadata.

Query parameters:

| Parameter | Default | Description |
| --- | --- | --- |
| `scope` | `local` | `local` returns this HTTP server's node-local stats and eviction config. `cluster` scatters stats to routed Agent Memory nodes and returns aggregate plus per-node counters. Invalid values return `400`. |

```bash
curl -s http://localhost:8123/api/ai/stats
```

Response:

```json
{
  "memory_count": 1,
  "cache_count": 0,
  "embedding_dim": 2,
  "evicted_memory_count": 1,
  "evicted_cache_count": 0,
  "snapshot_records_bytes": 5242880,
  "snapshot_vectors_bytes": 67108864,
  "snapshot_total_bytes": 72351744,
  "snapshot_latency_seconds": 0.0042,
  "snapshot_failures_total": 0,
  "tenant_quota_count": 2,
  "ann": {
    "mode": "auto",
    "enabled": true,
    "min_records": 50000,
    "oversample": 8,
    "indexed_vectors": 100000,
    "partitions": 2,
    "buckets": 16342,
    "max_bucket_size": 345,
    "memory_bytes": 9830400,
    "tombstone_entries": 12,
    "rebuild_count": 1,
    "last_rebuild_ms": 138.37,
    "search_count": 10,
    "fallback_count": 0
  },
  "eviction_config": {
    "max_memories": 100000,
    "max_cache_entries": 10000,
    "tenant_quota_count": 2,
    "evict_expired_on_write": true,
    "protect_pinned": true
  },
  "failover": {
    "source_node_id": 1,
    "replacement_node_id": 2,
    "source_ring_epoch": 7,
    "new_ring_epoch": 8,
    "ok": true,
    "adopted": false,
    "replica_promoted": false,
    "degraded": true,
    "replay_source_missing": true,
    "degraded_reason": "source agent memory shard is empty",
    "error": ""
  }
}
```

Cluster response:

```bash
curl -s 'http://localhost:8123/api/ai/stats?scope=cluster'
```

```json
{
  "scope": "cluster",
  "partial_failures": 1,
  "aggregate": {
    "memory_count": 3,
    "cache_count": 1,
    "embedding_dim": 128,
    "evicted_memory_count": 2,
    "evicted_cache_count": 0,
    "snapshot_records_bytes": 10485760,
    "snapshot_vectors_bytes": 134217728,
    "snapshot_total_bytes": 144703488,
    "snapshot_latency_seconds": 0.006,
    "snapshot_failures_total": 0,
    "tenant_quota_count": 2,
    "ann": {
      "enabled": true,
      "indexed_vectors": 3,
      "partitions": 2,
      "buckets": 128,
      "max_bucket_size": 4,
      "memory_bytes": 196608,
      "tombstone_entries": 0,
      "rebuild_count": 1,
      "last_rebuild_ms": 10.4,
      "search_count": 7,
      "fallback_count": 0
    }
  },
  "nodes": [
    {
      "node_id": 1,
      "local": true,
      "stats": {
        "memory_count": 1,
        "cache_count": 0,
        "embedding_dim": 128,
        "evicted_memory_count": 0,
        "evicted_cache_count": 0,
        "snapshot_records_bytes": 5242880,
        "snapshot_vectors_bytes": 67108864,
        "snapshot_total_bytes": 72351744,
        "snapshot_latency_seconds": 0.004,
        "snapshot_failures_total": 0,
        "tenant_quota_count": 1,
        "ann": {
          "enabled": true,
          "indexed_vectors": 1,
          "partitions": 1,
          "buckets": 64,
          "max_bucket_size": 2,
          "memory_bytes": 98304,
          "tombstone_entries": 0,
          "rebuild_count": 1,
          "last_rebuild_ms": 10.4,
          "search_count": 4,
          "fallback_count": 0
        }
      }
    },
    {
      "node_id": 2,
      "local": false,
      "error": "missing Agent Memory stats RPC client for node 2"
    }
  ]
}
```

### `DELETE /api/ai/memories/:memory_id`

Deletes one memory by id and records a WAL tombstone when persistence is enabled.
Query parameters match point lookup: `tenant_id` and `namespace`.

```bash
curl -s -X DELETE \
  'http://localhost:8123/api/ai/memories/mem_2_11_7?tenant_id=tenant_a&namespace=agent'
```

Response:

```json
{"ok":true,"memory_id":"mem_2_11_7"}
```

Missing memories return `404`.

### `POST /api/ai/memories`

Stores or updates a memory object. In routed multi-node mode, the owner node
stores the record and generates the returned `memory_id` when the caller omits
one.

```bash
curl -s -X POST http://localhost:8123/api/ai/memories \
  -H 'Content-Type: application/json' \
  -H 'X-Zepto-Tenant-Id: tenant_a' \
  -d '{
    "namespace": "agent",
    "user_id": "u1",
    "session_id": "s1",
    "agent_id": "planner",
    "type": "preference",
    "content": "User prefers concise answers.",
    "metadata_json": "{\"source\":\"profile\",\"trusted\":true}",
    "embedding": [1.0, 0.0],
    "token_count": 5,
    "importance": 2.0,
    "pinned": true
  }'
```

Response:

```json
{"ok":true,"memory_id":"mem_1"}
```

Supported fields: `memory_id`, `tenant_id`, `namespace`, `user_id`,
`session_id`, `agent_id`, `type`, `content`, `metadata_json`, `embedding`,
`token_count`, `importance`, `created_at_ns`, `expires_at_ns`, and `pinned`.

### `GET /api/ai/memories/:memory_id`

Fetches one memory by id. Query parameters:

| Param | Default | Description |
|-------|---------|-------------|
| `tenant_id` | empty | Optional tenant scope. `X-Zepto-Tenant-Id` also applies and must not conflict. |
| `namespace` | `default` | Namespace used for caller-supplied id routing. Owner-scoped ids route by embedded owner id. |

In routed multi-node mode, owner-scoped ids such as
`mem_<node_id>_<epoch>_<counter>` route directly to the owner. For caller-supplied
ids, pass the same namespace used at write time when it is not `default`.

```bash
curl -s 'http://localhost:8123/api/ai/memories/mem_2_11_7?tenant_id=tenant_a&namespace=agent'
```

Response:

```json
{
  "found": true,
  "memory": {
    "memory_id": "mem_2_11_7",
    "tenant_id": "tenant_a",
    "namespace": "agent",
    "content": "User prefers concise answers."
  }
}
```

Missing memories return `404` with `{"found":false}`. Remote owner errors return
`502`.

### `POST /api/ai/memories/search`

Searches memories after applying tenant, namespace, user, session, agent, type,
and TTL filters. Ranking combines embedding cosine similarity, pinned status,
importance, recency, and access count. The implementation keeps only the top-K
matches requested by `limit` instead of sorting every candidate.

In routed multi-node mode, the coordinator sends the query to Agent Memory nodes,
merges each node's local top-K by `score` and `created_at_ns`, then trims to the
requested `limit`. Remote shard failures return `502`.

```bash
curl -s -X POST http://localhost:8123/api/ai/memories/search \
  -H 'Content-Type: application/json' \
  -d '{
    "tenant_id": "tenant_a",
    "namespace": "agent",
    "user_id": "u1",
    "query_embedding": [1.0, 0.0],
    "limit": 5
  }'
```

Response:

```json
{
  "matches": [
    {
      "memory_id": "mem_1",
      "tenant_id": "tenant_a",
      "namespace": "agent",
      "content": "User prefers concise answers.",
      "score": 18.0,
      "similarity": 1.0
    }
  ],
  "rows": 1
}
```

### `POST /api/ai/context`

Assembles the same ranked memories under an optional token budget. Duplicate
content is omitted. In routed multi-node mode, context assembly first performs
the same global fan-out search merge, then applies deduplication and the token
budget. Remote shard failures return `502`.

```bash
curl -s -X POST http://localhost:8123/api/ai/context \
  -H 'Content-Type: application/json' \
  -d '{
    "tenant_id": "tenant_a",
    "namespace": "agent",
    "query_embedding": [1.0, 0.0],
    "token_budget": 64,
    "limit": 20
  }'
```

Response:

```json
{
  "memories": [],
  "token_count": 0,
  "rows": 0
}
```

### `POST /api/ai/cache/store`

Stores an application-level LLM cache entry. The exact key is normalized prompt
text scoped by `(tenant_id, namespace)`, with the embedding retained for semantic
fallback. In routed multi-node mode, the normalized prompt routes to one owner
node and that owner stores the cache entry.

```bash
curl -s -X POST http://localhost:8123/api/ai/cache/store \
  -H 'Content-Type: application/json' \
  -d '{
    "tenant_id": "tenant_a",
    "namespace": "agent",
    "prompt": "Summarize the latest task",
    "response": "Short task summary",
    "embedding": [0.9, 0.1],
    "token_count": 12
  }'
```

Response:

```json
{"ok":true,"cache_id":"cache_1"}
```

### `DELETE /api/ai/cache`

Deletes one exact prompt cache entry by `(tenant_id, namespace, prompt)` and
records a WAL tombstone when persistence is enabled. Query parameters:
`tenant_id`, `namespace` (default `default`), and required `prompt`.

```bash
curl -s -X DELETE \
  'http://localhost:8123/api/ai/cache?tenant_id=tenant_a&namespace=agent&prompt=Summarize%20the%20latest%20task'
```

Response:

```json
{"ok":true,"cache_id":"cache_1"}
```

Missing cache entries return `404`.

### `POST /api/ai/cache/lookup`

Performs exact normalized prompt lookup first, then semantic lookup when an
embedding is supplied and a candidate is above `semantic_threshold`. In routed
multi-node mode, the exact prompt lookup routes to the prompt owner. Semantic
fallback fans out to configured Agent Memory nodes after the exact lookup misses.

```bash
curl -s -X POST http://localhost:8123/api/ai/cache/lookup \
  -H 'Content-Type: application/json' \
  -d '{
    "tenant_id": "tenant_a",
    "namespace": "agent",
    "prompt": " summarize   THE latest task ",
    "embedding": [0.88, 0.12],
    "semantic_threshold": 0.92
  }'
```

Miss response:

```json
{"hit":false}
```

Hit response:

```json
{
  "hit": true,
  "kind": "exact",
  "score": 1.0,
  "entry": {
    "cache_id": "cache_1",
    "prompt": "Summarize the latest task",
    "response": "Short task summary"
  }
}
```

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

## Arrow IPC Ingest — POST /insert/arrow

`POST /insert/arrow` accepts an Apache Arrow IPC RecordBatchStream and ingests
each row through ZeptoDB's table-aware tick path without building per-row SQL.
It is intended for high-throughput DataFrame, Polars, PyArrow, and batched
client ingestion.

**Request**:

| Field | Value |
|-------|-------|
| Method | `POST` |
| Path | `/insert/arrow` |
| Body | Arrow IPC stream bytes |
| Content-Type | Optional; `application/vnd.apache.arrow.stream` recommended |

**Query parameters**:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `table` / `table_name` | empty | Destination ZeptoDB table. Empty uses legacy `table_id=0`. Non-empty names must already exist. |
| `sym_col` / `symbol_col` | `sym` | Symbol column. The default also accepts an Arrow column named `symbol`. Integer IDs and utf8 strings are supported; strings are dictionary-interned. |
| `price_col` | `price` | Numeric price column. Values are converted to int64 after `price_scale`. |
| `vol_col` / `volume_col` | `volume` | Numeric volume column. Values are converted to int64 after `volume_scale`. |
| `ts_col` / `timestamp_col` | `timestamp` | Optional timestamp column. If absent, ZeptoDB assigns nanosecond timestamps at ingest time. Arrow timestamp arrays are converted to ns. |
| `msg_type_col` | `msg_type` | Optional uint8-compatible message type column. Missing values default to `0` (trade). |
| `price_scale` | `1.0` | Multiplier applied before int64 conversion. Use `100` to store cents from decimal prices. |
| `volume_scale` / `vol_scale` | `1.0` | Multiplier applied before int64 conversion. |

**Response**:

```json
{"inserted": 1000, "failed": 0}
```

The response also includes `X-Zepto-Format: arrow-stream` on success.

**Errors**:

| Status | Meaning |
|--------|---------|
| `400` | Malformed Arrow stream, missing required column, null in required column, unsupported type, invalid scale, or unknown table. |
| `403` | Table ACL or tenant namespace rejection. If table ACL is active, `table=` is required. |
| `406` | Server was built without Arrow IPC support. Rebuild with `-DZEPTO_USE_FLIGHT=ON` or install Arrow / pyarrow before reconfiguring. |

**Example**:

```bash
python3 - <<'PY'
import pyarrow as pa
import pyarrow.ipc as ipc

table = pa.table({
    "symbol": ["AAPL", "AAPL"],
    "price": [15000, 15010],
    "volume": [100, 120],
    "timestamp": [1711000000000000000, 1711000000000000001],
})

with pa.OSFile("/tmp/trades.arrow", "wb") as sink:
    with ipc.new_stream(sink, table.schema) as writer:
        writer.write_table(table)
PY

curl -s -X POST \
  'http://localhost:8123/insert/arrow?table=trades&sym_col=symbol' \
  -H 'Content-Type: application/vnd.apache.arrow.stream' \
  --data-binary @/tmp/trades.arrow
```

---

## MessagePack Columnar Ingest — POST /insert/msgpack

`POST /insert/msgpack` accepts a MessagePack top-level map whose values are
column arrays. It is a dependency-light binary ingest path for Telegraf-style
and embedded clients that can batch rows but do not need the full Arrow runtime.

**Request**:

| Field | Value |
|-------|-------|
| Method | `POST` |
| Path | `/insert/msgpack` |
| Body | MessagePack map of column arrays |
| Content-Type | Optional; `application/msgpack` recommended |

The default payload shape is:

```text
{
  "sym": ["AAPL", "AAPL"],
  "price": [15000, 15010],
  "volume": [100, 120],
  "timestamp": [1711000000000000000, 1711000000000000001],
  "msg_type": [0, 0]
}
```

`timestamp` and `msg_type` are optional. If `timestamp` is absent, ZeptoDB
assigns ingest-time nanosecond timestamps. If `msg_type` is absent, rows default
to trade (`0`).

**Query parameters**:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `table` / `table_name` | empty | Destination ZeptoDB table. Empty uses legacy `table_id=0`. Non-empty names must already exist. |
| `sym_col` / `symbol_col` | `sym` | Symbol column. The default also accepts a payload key named `symbol`. Integer IDs and strings are supported; strings are dictionary-interned. |
| `price_col` | `price` | Numeric price column. Values are converted to int64 after `price_scale`. |
| `vol_col` / `volume_col` | `volume` | Numeric volume column. Values are converted to int64 after `volume_scale`. |
| `ts_col` / `timestamp_col` | `timestamp` | Optional timestamp column in nanoseconds. |
| `msg_type_col` | `msg_type` | Optional uint8-compatible message type column. Missing values default to `0` (trade). |
| `price_scale` | `1.0` | Multiplier applied before int64 conversion. Use `100` to store cents from decimal prices. |
| `volume_scale` / `vol_scale` | `1.0` | Multiplier applied before int64 conversion. |

**Supported MessagePack values**:

- Top-level: map with string keys.
- Columns: arrays of strings or numeric values.
- Strings: `fixstr`, `str8`, `str16`, `str32`.
- Integers: positive fixint, negative fixint, `uint8/16/32/64`, `int8/16/32/64`.
- Floats: `float32`, `float64`.
- `nil` is accepted only for optional `timestamp` / `msg_type` entries.

**Response**:

```json
{"inserted": 1000, "failed": 0}
```

The response also includes `X-Zepto-Format: msgpack-columnar` on success.

**Errors**:

| Status | Meaning |
|--------|---------|
| `400` | Malformed MessagePack, missing required column, column length mismatch, unsupported type, invalid scale, integer overflow, unknown table, or tenant-scoped request without `table=`. |
| `403` | Table ACL or tenant namespace rejection. If table ACL is active, `table=` is required. |

**Example**:

```bash
python3 - <<'PY'
import msgpack

payload = {
    "symbol": ["AAPL", "AAPL"],
    "price": [15000, 15010],
    "volume": [100, 120],
    "timestamp": [1711000000000000000, 1711000000000000001],
}

with open("/tmp/trades.msgpack", "wb") as f:
    f.write(msgpack.packb(payload, use_bin_type=True))
PY

curl -s -X POST \
  'http://localhost:8123/insert/msgpack?table=trades&sym_col=symbol' \
  -H 'Content-Type: application/msgpack' \
  --data-binary @/tmp/trades.msgpack
```

---

## Response Format

Responses are JSON unless the client explicitly requests a binary Arrow query
response from `POST /`.

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

### Arrow IPC response (binary)

`POST /` will return an Apache Arrow IPC stream (RecordBatchStream format)
instead of JSON when the client opts in. Same DuckDB engine, ~2–3× faster
than JSON on large result sets — the JSON encoding is the bottleneck. Drop-in
for Pandas/Polars/BI tools (devlog 119).

**Trigger** (any one of):

| Method | Example |
|--------|---------|
| `Accept` header | `Accept: application/vnd.apache.arrow.stream` |
| ClickHouse-compatible query param | `?default_format=Arrow` (or `ArrowStream`) |
| Short-form query param | `?format=arrow` |

**Response**:

| Field | Value |
|-------|-------|
| Content-Type | `application/vnd.apache.arrow.stream` |
| Body | Arrow IPC stream bytes (one RecordBatch) |
| `X-Zepto-Format` | `arrow-stream` (debug aid) |

**Notes**:

- Errors (parse, executor, ACL, tenant denial) **always** return JSON
  regardless of `Accept`. Only successful result sets are encoded as Arrow.
  Matches ClickHouse behaviour.
- If the server returns 406, the binary was built without Arrow support.
  Rebuild with `-DZEPTO_USE_FLIGHT=ON` (default ON when Arrow C++ is found at
  configure time) or install Arrow / pyarrow before reconfiguring.
- The encoder converts `STRING` and `SYMBOL` columns (with the result's
  `symbol_dict`) to Arrow `utf8`. Other types map directly: `INT64` → int64,
  `FLOAT64` → double, `FLOAT32` → float.

**Example**:

```bash
curl -s -X POST http://localhost:8123/ \
     -H 'Accept: application/vnd.apache.arrow.stream' \
     -d 'SELECT * FROM trades LIMIT 1000' \
     --output result.arrow

python3 -c "import pyarrow.ipc as ipc; \
            r = ipc.RecordBatchStreamReader('result.arrow'); \
            print(r.read_all().to_pandas())"
```

ClickHouse-style query parameter form (no special header needed):

```bash
curl -s -X POST 'http://localhost:8123/?default_format=Arrow' \
     -d 'SELECT vwap(price, volume), count(*) FROM trades' \
     --output result.arrow
```

**Why use it**: removes JSON encoding overhead (~2–3× faster on large result
sets in the Arc 26.05 bench, identical engine) and lands data in Arrow memory
layout for zero-copy hand-off to Pandas/Polars/DuckDB.

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
  "last_ingest_latency_ns": 181,
  "small_table_join": {
    "candidates": 4,
    "accepted": 4,
    "rejected_row_cap": 0,
    "errors": 0,
    "rows_materialized": 327,
    "last_left_rows": 72,
    "last_right_rows": 6
  }
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

When the HTTP server is wired to a `QueryCoordinator`, `/stats` also includes
bounded small-table JOIN telemetry:

| Field | Description |
|-------|-------------|
| `small_table_join.candidates` | Bounded small-table JOIN candidates seen by the coordinator |
| `small_table_join.accepted` | Candidates accepted and executed through coordinator-local materialization |
| `small_table_join.rejected_row_cap` | Candidates rejected because a JOIN side exceeded the row cap |
| `small_table_join.rejected_byte_cap` | Candidates rejected because estimated materialized bytes exceeded the byte cap |
| `small_table_join.rejected_latency_cap` | Candidates rejected because coordinator-local execution exceeded the latency cap |
| `small_table_join.errors` | Non-cap failures in the bounded small-table JOIN path |
| `small_table_join.rows_materialized` | Rows replayed into temporary coordinator-local JOIN tables |
| `small_table_join.bytes_materialized` | Estimated bytes replayed into temporary coordinator-local JOIN tables for accepted JOINs |
| `small_table_join.last_left_rows` | Left-side row count from the most recent bounded JOIN attempt |
| `small_table_join.last_right_rows` | Right-side row count from the most recent bounded JOIN attempt |
| `small_table_join.last_materialized_bytes` | Estimated materialized bytes from the most recent bounded JOIN attempt |
| `small_table_join.last_latency_us` | Coordinator-local latency of the most recent bounded JOIN attempt, in microseconds |

These fields describe the narrow coordinator-local small-table JOIN path. They
are not a signal that arbitrary distributed JOIN planning is supported.

`/stats` also includes bounded full-data window materialization telemetry:

| Field | Description |
|-------|-------------|
| `window_materialization.candidates` | Full-data materialization candidates seen by the coordinator |
| `window_materialization.accepted` | Candidates accepted and executed through coordinator-local materialization |
| `window_materialization.rejected_row_cap` | Candidates rejected because fetched rows exceeded the row cap |
| `window_materialization.rejected_byte_cap` | Candidates rejected because estimated materialized bytes exceeded the byte cap |
| `window_materialization.rejected_latency_cap` | Candidates rejected because coordinator-local execution exceeded the latency cap |
| `window_materialization.errors` | Non-cap failures in the full-data materialization path |
| `window_materialization.rows_materialized` | Rows replayed into temporary coordinator-local tables for accepted attempts |
| `window_materialization.bytes_materialized` | Estimated bytes replayed into temporary coordinator-local tables for accepted attempts |
| `window_materialization.last_rows` | Base row count from the most recent materialization attempt |
| `window_materialization.last_materialized_bytes` | Estimated materialized bytes from the most recent materialization attempt |
| `window_materialization.last_latency_us` | Coordinator-local latency of the most recent materialization attempt, in microseconds |

Cap rejections fail closed with an explicit SQL error. The coordinator does not
fall back to partial scatter semantics for these queries.

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

# HELP zepto_small_table_join_candidates_total Bounded small-table JOIN candidates seen by the coordinator
# TYPE zepto_small_table_join_candidates_total counter
zepto_small_table_join_candidates_total 4

# HELP zepto_small_table_join_accepted_total Bounded small-table JOINs accepted and executed by the coordinator
# TYPE zepto_small_table_join_accepted_total counter
zepto_small_table_join_accepted_total 4

# HELP zepto_small_table_join_row_cap_rejections_total Bounded small-table JOINs rejected because a side exceeded the row cap
# TYPE zepto_small_table_join_row_cap_rejections_total counter
zepto_small_table_join_row_cap_rejections_total 0

# HELP zepto_small_table_join_byte_cap_rejections_total Bounded small-table JOINs rejected because estimated materialized bytes exceeded the cap
# TYPE zepto_small_table_join_byte_cap_rejections_total counter
zepto_small_table_join_byte_cap_rejections_total 0

# HELP zepto_small_table_join_latency_cap_rejections_total Bounded small-table JOINs rejected because coordinator-local latency exceeded the cap
# TYPE zepto_small_table_join_latency_cap_rejections_total counter
zepto_small_table_join_latency_cap_rejections_total 0

# HELP zepto_small_table_join_errors_total Bounded small-table JOIN errors outside configured cap rejections
# TYPE zepto_small_table_join_errors_total counter
zepto_small_table_join_errors_total 0

# HELP zepto_small_table_join_rows_materialized_total Rows materialized into coordinator-local temporary tables for bounded small-table JOINs
# TYPE zepto_small_table_join_rows_materialized_total counter
zepto_small_table_join_rows_materialized_total 327

# HELP zepto_small_table_join_bytes_materialized_total Estimated bytes materialized into coordinator-local temporary tables for accepted bounded small-table JOINs
# TYPE zepto_small_table_join_bytes_materialized_total counter
zepto_small_table_join_bytes_materialized_total 65536

# HELP zepto_small_table_join_last_left_rows Last bounded small-table JOIN left-side row count
# TYPE zepto_small_table_join_last_left_rows gauge
zepto_small_table_join_last_left_rows 72

# HELP zepto_small_table_join_last_right_rows Last bounded small-table JOIN right-side row count
# TYPE zepto_small_table_join_last_right_rows gauge
zepto_small_table_join_last_right_rows 6

# HELP zepto_small_table_join_last_materialized_bytes Last bounded small-table JOIN estimated materialized bytes
# TYPE zepto_small_table_join_last_materialized_bytes gauge
zepto_small_table_join_last_materialized_bytes 8192

# HELP zepto_small_table_join_last_latency_us Last bounded small-table JOIN coordinator-local latency in microseconds
# TYPE zepto_small_table_join_last_latency_us gauge
zepto_small_table_join_last_latency_us 2100

# HELP zepto_window_materialization_candidates_total Cluster full-data window materialization candidates seen by the coordinator
# TYPE zepto_window_materialization_candidates_total counter
zepto_window_materialization_candidates_total 3

# HELP zepto_window_materialization_accepted_total Cluster full-data window materializations accepted and executed by the coordinator
# TYPE zepto_window_materialization_accepted_total counter
zepto_window_materialization_accepted_total 3

# HELP zepto_window_materialization_row_cap_rejections_total Cluster full-data window materializations rejected because rows exceeded the cap
# TYPE zepto_window_materialization_row_cap_rejections_total counter
zepto_window_materialization_row_cap_rejections_total 0

# HELP zepto_window_materialization_byte_cap_rejections_total Cluster full-data window materializations rejected because estimated materialized bytes exceeded the cap
# TYPE zepto_window_materialization_byte_cap_rejections_total counter
zepto_window_materialization_byte_cap_rejections_total 0

# HELP zepto_window_materialization_latency_cap_rejections_total Cluster full-data window materializations rejected because coordinator-local latency exceeded the cap
# TYPE zepto_window_materialization_latency_cap_rejections_total counter
zepto_window_materialization_latency_cap_rejections_total 0

# HELP zepto_window_materialization_errors_total Cluster full-data window materialization errors outside configured cap rejections
# TYPE zepto_window_materialization_errors_total counter
zepto_window_materialization_errors_total 0

# HELP zepto_window_materialization_rows_materialized_total Rows materialized into coordinator-local temporary tables for accepted full-data window queries
# TYPE zepto_window_materialization_rows_materialized_total counter
zepto_window_materialization_rows_materialized_total 144

# HELP zepto_window_materialization_bytes_materialized_total Estimated bytes materialized into coordinator-local temporary tables for accepted full-data window queries
# TYPE zepto_window_materialization_bytes_materialized_total counter
zepto_window_materialization_bytes_materialized_total 32768

# HELP zepto_window_materialization_last_rows Last full-data window materialization row count
# TYPE zepto_window_materialization_last_rows gauge
zepto_window_materialization_last_rows 48

# HELP zepto_window_materialization_last_materialized_bytes Last full-data window materialization estimated bytes
# TYPE zepto_window_materialization_last_materialized_bytes gauge
zepto_window_materialization_last_materialized_bytes 8192

# HELP zepto_window_materialization_last_latency_us Last full-data window materialization coordinator-local latency in microseconds
# TYPE zepto_window_materialization_last_latency_us gauge
zepto_window_materialization_last_latency_us 2600

# HELP zepto_rows_scanned_total Total rows scanned
# TYPE zepto_rows_scanned_total counter
zepto_rows_scanned_total 50000000

# HELP zepto_partitions_total Total partitions created
# TYPE zepto_partitions_total gauge
zepto_partitions_total 10

# HELP zepto_last_ingest_latency_ns Last ingest latency in nanoseconds
# TYPE zepto_last_ingest_latency_ns gauge
zepto_last_ingest_latency_ns 181

# HELP zepto_agent_memory_records Current Agent Memory record count
# TYPE zepto_agent_memory_records gauge
zepto_agent_memory_records 1

# HELP zepto_agent_cache_entries Current Agent Cache entry count
# TYPE zepto_agent_cache_entries gauge
zepto_agent_cache_entries 0

# HELP zepto_agent_memory_evictions_total Total Agent Memory records evicted by TTL or capacity policy
# TYPE zepto_agent_memory_evictions_total counter
zepto_agent_memory_evictions_total 1

# HELP zepto_agent_cache_evictions_total Total Agent Cache entries evicted by TTL or capacity policy
# TYPE zepto_agent_cache_evictions_total counter
zepto_agent_cache_evictions_total 0

# HELP zepto_agent_memory_tenant_quotas Configured Agent Memory tenant quota count
# TYPE zepto_agent_memory_tenant_quotas gauge
zepto_agent_memory_tenant_quotas 2

# HELP zepto_agent_memory_snapshot_latency_seconds Last Agent Memory snapshot attempt duration in seconds
# TYPE zepto_agent_memory_snapshot_latency_seconds gauge
zepto_agent_memory_snapshot_latency_seconds 0.0042

# HELP zepto_agent_memory_snapshot_failures_total Total failed Agent Memory snapshot attempts
# TYPE zepto_agent_memory_snapshot_failures_total counter
zepto_agent_memory_snapshot_failures_total 0

# HELP zepto_agent_memory_snapshot_records_bytes Last Agent Memory records sidecar size in bytes
# TYPE zepto_agent_memory_snapshot_records_bytes gauge
zepto_agent_memory_snapshot_records_bytes 5242880

# HELP zepto_agent_memory_snapshot_vectors_bytes Last Agent Memory vectors sidecar size in bytes
# TYPE zepto_agent_memory_snapshot_vectors_bytes gauge
zepto_agent_memory_snapshot_vectors_bytes 67108864

# HELP zepto_agent_memory_snapshot_total_bytes Last Agent Memory sidecar total size in bytes
# TYPE zepto_agent_memory_snapshot_total_bytes gauge
zepto_agent_memory_snapshot_total_bytes 72351744

# HELP zepto_agent_memory_ann_indexed_vectors Agent Memory vectors indexed by ANN
# TYPE zepto_agent_memory_ann_indexed_vectors gauge
zepto_agent_memory_ann_indexed_vectors 100000

# HELP zepto_agent_memory_ann_rebuilds_total Agent Memory ANN index rebuild count
# TYPE zepto_agent_memory_ann_rebuilds_total counter
zepto_agent_memory_ann_rebuilds_total 1

# HELP zepto_agent_memory_ann_last_rebuild_ms Last Agent Memory ANN rebuild duration in ms
# TYPE zepto_agent_memory_ann_last_rebuild_ms gauge
zepto_agent_memory_ann_last_rebuild_ms 138.37

# HELP zepto_agent_memory_ann_searches_total Agent Memory searches served from ANN candidates
# TYPE zepto_agent_memory_ann_searches_total counter
zepto_agent_memory_ann_searches_total 10

# HELP zepto_agent_memory_ann_fallbacks_total Agent Memory ANN searches that fell back to filtered scan
# TYPE zepto_agent_memory_ann_fallbacks_total counter
zepto_agent_memory_ann_fallbacks_total 0

# HELP zepto_agent_memory_ann_memory_bytes Estimated Agent Memory ANN index bytes
# TYPE zepto_agent_memory_ann_memory_bytes gauge
zepto_agent_memory_ann_memory_bytes 9830400

# HELP zepto_agent_memory_ann_tombstone_entries Agent Memory ANN tombstone entries retained by incremental maintenance
# TYPE zepto_agent_memory_ann_tombstone_entries gauge
zepto_agent_memory_ann_tombstone_entries 12
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

### Admin: License

#### `GET /api/license` — Current license info (public)

No authentication required. Returns the current edition, enabled features, and trial/expiry status.

```bash
curl http://localhost:8123/api/license
```

Community edition (no key):
```json
{
  "edition": "community",
  "features": [],
  "max_nodes": 1,
  "trial": false,
  "expired": false,
  "upgrade_url": "https://zeptodb.com/pricing"
}
```

Enterprise edition:
```json
{
  "edition": "enterprise",
  "features": ["cluster","sso","audit_export","advanced_rbac","kafka","migration","geo_replication","rolling_upgrade","iot_connectors"],
  "max_nodes": 16,
  "trial": false,
  "expired": false,
  "company": "Acme Corp",
  "expires": "2027-04-01"
}
```

#### `GET /admin/license` — Full license details (admin)

Same as `/api/license` but includes additional fields: `company`, `tenant_id`, `grace_days`, `issued_at`.

```bash
curl http://localhost:8123/admin/license -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{
  "edition": "enterprise",
  "features": ["cluster","sso"],
  "max_nodes": 16,
  "trial": false,
  "expired": false,
  "company": "Acme Corp",
  "tenant_id": "",
  "grace_days": 30,
  "issued_at": 1711234567,
  "expires": "2027-04-01"
}
```

#### `POST /admin/license` — Upload license key (admin)

Accepts a raw JWT license key string as the request body. Loads and validates it.

```bash
curl -X POST http://localhost:8123/admin/license \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d 'eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9...'
```

Success:
```json
{"loaded": true, "edition": "enterprise", "company": "Acme Corp"}
```

Failure:
```json
{"loaded": false, "error": "Invalid license key"}
```

#### `POST /admin/license/trial` — Generate 30-day trial (admin)

Generates and loads an unsigned 30-day Enterprise trial key. Trial keys are single-node only (`max_nodes=1`) with all features enabled.

```bash
curl -X POST http://localhost:8123/admin/license/trial \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{"loaded": true, "edition": "enterprise", "trial": true, "expires": "2026-05-15"}
```

---

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

#### `POST /admin/table-placement` — Set experimental declared table placement

Sets or clears an experimental placement policy for a declared table in cluster
mode. This is intended for small operational/control tables whose rows often
use `symbol_id=0`. Successful updates are now persisted into the local schema
catalog, so a coordinator can re-apply the placement after restart when the
schema catalog is loaded.

```bash
curl -X POST http://localhost:8123/admin/table-placement \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{"table":"action_outcome_vendor_suppressions_010","policy":"pinned_node","node_id":1}'
```

```json
{"ok": true, "table": "action_outcome_vendor_suppressions_010", "policy": "pinned_node", "node_id": 1}
```

Supported `policy` values:

| Policy | Behavior |
| --- | --- |
| `hash_by_table_and_symbol` | Default table-scoped routing: table id and row symbol both contribute to owner selection |
| `hash_by_table` | All rows for the table route through the same table-level hash key |
| `pinned_node` | All rows for the table route to `node_id`; `node_id` is required |
| `clear` | Remove the override and return to default routing |

The table must already exist in the local schema registry. Placement can also
be declared at create time:

```sql
CREATE TABLE action_outcome_vendor_suppressions_010 (
  query_id STRING,
  candidate_id STRING,
  timestamp_ns TIMESTAMP_NS
) WITH (placement = pinned_node, node_id = 1)
```

Supported DDL placement values are `hash_by_table`,
`hash_by_table_and_symbol`, and `pinned_node` with `node_id`. This remains an
experimental placement feature: persisted metadata is available, but it is not
a rebalance/failover policy.

#### `GET /admin/edge-fleet-connector` — Controlled-pilot connector lifecycle status

Returns server-owned lifecycle state for the Physical AI edge/fleet connector.
The supported rollout status is controlled pilot only: admin-gated, opt-in,
SQL/HTTP contract deployments that follow
`docs/operations/PHYSICAL_AI_EDGE_FLEET_CONTROLLED_PILOT.md`.

```bash
curl http://localhost:8123/admin/edge-fleet-connector \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{
  "configured": true,
  "enabled": true,
  "name": "physical_ai_edge_fleet",
  "edge_outbox_table": "physical_ai_edge_feed_outbox_016",
  "fleet_ack_table": "physical_ai_fleet_feed_ack_016",
  "checkpoint_path": "/var/lib/zeptodb/edge-fleet.checkpoint",
  "batch_limit": 128,
  "max_inflight": 128,
  "max_retries_per_event": 1,
  "max_failures_per_pass": 16,
  "retry_backoff_ms": 0,
  "allow_late_events": true,
  "worker_enabled": true,
  "worker_hooks_configured": true,
  "worker_running": true,
  "worker_poll_interval_ms": 1000,
  "acked_count": 0,
  "highest_acked_stream_seq": 0,
  "configure_total": 1,
  "start_total": 1,
  "stop_total": 0,
  "start_failures_total": 0,
  "stop_failures_total": 0,
  "worker_start_total": 1,
  "worker_passes_total": 12,
  "worker_load_errors_total": 0,
  "worker_observer_errors_total": 0,
  "last_pass": {
    "outbox_events_seen": 52,
    "batch_event_count": 12,
    "attempted_count": 12,
    "acked_count": 12,
    "transient_failure_count": 0,
    "permanent_failure_count": 0,
    "duplicate_count": 0,
    "late_count": 0,
    "rejected_count": 0,
    "failure_budget_exhausted": false
  },
  "last_error": ""
}
```

#### `POST /admin/edge-fleet-connector` — Configure and optionally enable

```bash
curl -X POST http://localhost:8123/admin/edge-fleet-connector \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "physical_ai_edge_fleet",
    "enabled": true,
    "edge_outbox_table": "physical_ai_edge_feed_outbox_016",
    "fleet_ack_table": "physical_ai_fleet_feed_ack_016",
    "checkpoint_path": "/var/lib/zeptodb/edge-fleet.checkpoint",
    "batch_limit": 128,
    "max_inflight": 128,
    "max_retries_per_event": 1,
    "max_failures_per_pass": 16,
    "retry_backoff_ms": 0,
    "allow_late_events": true,
    "worker_enabled": false,
    "worker_poll_interval_ms": 1000,
    "sql_adapter_enabled": true,
    "sql_adapter_create_tables": true
  }'
```

`batch_limit`, `max_inflight`, `max_retries_per_event`,
`max_failures_per_pass`, and `worker_poll_interval_ms` must be positive;
`retry_backoff_ms` must be non-negative. If `enabled` is omitted, the server
enables the configured connector. If `worker_enabled=true`, either the
embedding application must have installed `EdgeFleetConnectorRuntimeHooks` or
the request must set `sql_adapter_enabled=true`; otherwise `start()` fails with
HTTP 400.

Set `sql_adapter_enabled=true` to install the built-in SQL/HTTP adapter instead
of embedding-provided worker hooks. Empty `edge_sql_url` and `fleet_sql_url`
use the server-local `QueryExecutor`; non-empty URLs execute SQL against
ZeptoDB HTTP `POST /`. Set `sql_adapter_create_tables=true` to create the
default local Experiment 016 edge outbox, fleet inbox, fleet final, ACK, and
telemetry tables. Table bootstrap is intentionally local-only; remote HTTP
endpoints should be migrated by the operator.

SQL/HTTP adapter fields:

| Field | Default | Description |
| --- | --- | --- |
| `sql_adapter_enabled` | `false` | Install server-owned SQL/HTTP outbox loader, fleet sink, and pass telemetry hooks. |
| `sql_adapter_create_tables` | `false` | Create default local Experiment 016 contract tables when URLs are empty. |
| `edge_sql_url` | `""` | Optional ZeptoDB HTTP SQL endpoint for loading the edge outbox. Empty uses the local executor. |
| `fleet_sql_url` | `""` | Optional ZeptoDB HTTP SQL endpoint for fleet inbox/final/ACK/telemetry inserts. Empty uses the local executor. |
| `fleet_inbox_table` | `physical_ai_fleet_feed_inbox_016` | Fleet delivery attempt table. |
| `fleet_decision_table` | `physical_ai_fleet_edge_decisions_016` | Fleet materialized decision table. |
| `fleet_retrieval_table` | `physical_ai_fleet_retrieval_016` | Fleet materialized retrieval evidence table. |
| `fleet_suppression_table` | `physical_ai_fleet_suppressions_016` | Fleet materialized suppression evidence table. |
| `fleet_telemetry_table` | `physical_ai_fleet_feed_telemetry_016` | Fleet worker pass telemetry table. |
| `outbox_query_limit` | `128` | Required positive SQL page size and maximum unacked events returned to one worker pass. The loader pages past rows already present in the fleet ACK ledger so small limits do not starve newer rows. |
| `max_outbox_bytes` | `1048576` | Required positive decoded SQL cell byte budget for one outbox load, including ACKed rows scanned while paging. |
| `record_pass_telemetry` | `true` | Insert one telemetry row after each successful worker pass. |

Embedding applications can enable durable SQL/HTTP adapter configuration by
calling `HttpServer::set_edge_fleet_connector_config_persistence(path)` before
serving traffic. Successful SQL-adapter `POST` requests then write a versioned
JSON config file. On restart, a new server instance reloads the config,
reinstalls SQL/HTTP hooks, recreates local default tables when requested, and
starts the connector if the persisted config was enabled. ACK/cursor state is
separate: set `checkpoint_path` to persist acknowledged event ids and highest
stream sequence, and the runtime reloads it before processing outbox rows. The
SQL/HTTP loader also checks the fleet ACK ledger before delivery and pages past
already-ACKed rows to keep bounded loads moving after restarts.

Idempotent sink contract:

- `feed_event_id` is the event idempotency key and must be stable across edge
  retries, HTTP reconnects, and process restarts.
- The fleet ACK table is the durable delivery ledger. The SQL/HTTP sink checks
  it before applying fleet final rows and inserts an ACK only after the final
  row succeeds.
- Remote SQL/HTTP ACK checks narrow by numeric `stream_seq` and compare
  `feed_event_id` client-side, so bounded replay can page past ACKed prefixes
  even when the remote SQL string-column comparison path differs from local
  symbol encoding.
- If final-row materialization succeeds but ACK insertion fails, the sink
  returns `AppliedButAckFailed`; the next pass replays the event and must be
  safe for duplicate final-row attempts.
- Operators should enforce uniqueness or dedupe policy on `feed_event_id` in
  downstream fleet projections when external storage supports it. ZeptoDB v0
  table bootstrap creates append-only tables, so consumers should treat the ACK
  ledger as the source of truth.

#### `DELETE /admin/edge-fleet-connector` — Disable and clear config

```bash
curl -X DELETE http://localhost:8123/admin/edge-fleet-connector \
  -H "Authorization: Bearer $ADMIN_KEY"
```

This endpoint stops the connector when enabled, saves checkpoint state when a
checkpoint path is configured, clears the process-local runtime config, and
removes the persisted SQL/HTTP adapter config file when config persistence is
enabled.

This lifecycle surface is controlled-pilot only. The server runtime can run a
bounded worker with embedding-provided hooks or with the built-in SQL/HTTP
adapter, but broad GA/operator positioning remains blocked until a later
production gate explicitly changes the rollout status.
Connector configuration is file-persisted when enabled by embedding code, but
the path is still experimental and should not be described as a promoted
replication feature until the promotion gates in
`docs/research/EXPERIMENT_GOVERNANCE.md` pass.

#### `GET /admin/action-outcome-supervisor` - Experimental supervisor lifecycle status

Returns server-owned lifecycle state for the experimental Physical AI
Action-Outcome supervisor. The surface is shadow-only and does not publish
actuator commands.

```bash
curl http://localhost:8123/admin/action-outcome-supervisor \
  -H "Authorization: Bearer $ADMIN_KEY"
```

```json
{
  "configured": true,
  "enabled": true,
  "worker_running": true,
  "worker_hooks_configured": true,
  "failure_budget_exhausted": false,
  "name": "physical_ai_action_outcome",
  "mode": "shadow",
  "rollout_stage": "controlled_shadow_pilot",
  "history_table": "physical_ai_action_history",
  "proposal_table": "physical_ai_action_proposals",
  "decision_table": "physical_ai_supervision_decisions",
  "evidence_table": "physical_ai_supervision_evidence",
  "fail_closed_action": "manual_review",
  "worker_poll_interval_ms": 1000,
  "batch_limit": 128,
  "max_consecutive_failures": 3,
  "max_decision_errors_per_pass": 16,
  "max_sink_errors_per_pass": 16,
  "consecutive_failures": 0,
  "configure_total": 1,
  "start_total": 1,
  "stop_total": 0,
  "start_failures_total": 0,
  "stop_failures_total": 0,
  "worker_start_total": 1,
  "worker_wakeups_total": 12,
  "worker_passes_total": 12,
  "worker_idle_passes_total": 5,
  "worker_failures_total": 0,
  "proposals_processed_total": 42,
  "proposals_duplicate_total": 4,
  "proposals_rejected_total": 1,
  "decisions_allow_total": 30,
  "decisions_suppress_total": 12,
  "fail_closed_total": 2,
  "evidence_rows_written_total": 126,
  "last_pass": {
    "proposals_seen": 10,
    "batch_proposals": 10,
    "processed_count": 8,
    "duplicate_count": 1,
    "rejected_count": 1,
    "decision_error_count": 0,
    "sink_error_count": 0,
    "allow_count": 6,
    "suppress_count": 2,
    "fail_closed_count": 0,
    "evidence_rows_written": 24,
    "latency_us": 512
  },
  "last_error": ""
}
```

#### `POST /admin/action-outcome-supervisor` - Configure and optionally enable

```bash
curl -X POST http://localhost:8123/admin/action-outcome-supervisor \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "physical_ai_action_outcome",
    "enabled": true,
    "mode": "shadow",
    "rollout_stage": "controlled_shadow_pilot",
    "history_table": "physical_ai_action_history",
    "proposal_table": "physical_ai_action_proposals",
    "decision_table": "physical_ai_supervision_decisions",
    "evidence_table": "physical_ai_supervision_evidence",
    "fail_closed_action": "manual_review",
    "worker_enabled": false,
    "worker_poll_interval_ms": 1000,
    "batch_limit": 128,
    "max_consecutive_failures": 3,
    "max_decision_errors_per_pass": 16,
    "max_sink_errors_per_pass": 16,
    "sql_adapter_enabled": true,
    "sql_adapter_create_tables": true,
    "history_evidence_limit": 64,
    "suppress_min_failure_count": 1,
    "suppress_outcome_score_below": 0,
    "worker_ownership_required": true,
    "worker_lease_enabled": true,
    "worker_owner_id": "node-a",
    "worker_lease_ttl_ms": 15000
  }'
```

Only `mode="shadow"` and `rollout_stage="controlled_shadow_pilot"` are
accepted. `rollout_stage="promoted_operator_feature"` is intentionally rejected
until the GA/operator gates in `docs/research/EXPERIMENT_GOVERNANCE.md` are
explicitly reopened. `batch_limit`,
`worker_poll_interval_ms`, `max_consecutive_failures`,
`max_decision_errors_per_pass`, and `max_sink_errors_per_pass` must be
positive. The decision and sink error budgets bound one pass during
backpressure or fault injection; exhausting either budget fails that pass. If
`enabled` is omitted, the server enables the configured supervisor. If
`worker_enabled=true`, the embedding application must have installed
`ActionOutcomeSupervisorRuntimeHooks` or set `sql_adapter_enabled=true`;
otherwise `start()` fails with HTTP 400.

Set `sql_adapter_enabled=true` to install server-owned ZeptoDB SQL hooks
instead of custom embedding hooks. Set `sql_adapter_create_tables=true` to
create the default contract tables when they are missing; production
deployments should still manage schemas through normal migrations.
When embedding code has enabled
`HttpServer::set_action_outcome_supervisor_config_persistence(path)`,
successful POST requests with `sql_adapter_enabled=true` also rewrite the
server-local durable SQL adapter config file. A later HTTP server instance can
load that file and reinstall SQL-backed hooks before serving requests.
When embedding code has enabled
`HttpServer::set_action_outcome_supervisor_catalog_config(table, name, ...)`,
successful SQL-adapter POST requests also append a versioned JSON config row to
that SQL catalog table. A later HTTP server instance can reload the latest row,
reinstall SQL hooks, and restart the supervisor when the persisted row was
enabled.

SQL adapter fields:

| Field | Default | Meaning |
| --- | --- | --- |
| `sql_adapter_enabled` | `false` | Install SQL-backed proposal/history/decision/evidence hooks. |
| `sql_adapter_create_tables` | `false` | Create default contract tables with `CREATE TABLE IF NOT EXISTS`. |
| `proposal_query_limit` | `0` | Proposal query bound; `0` uses `batch_limit`. |
| `history_evidence_limit` | `64` | Maximum matching history rows scanned per proposal. |
| `suppress_min_failure_count` | `1` | Suppress when at least this many history rows are below threshold. |
| `suppress_outcome_score_below` | `0` | Outcome scores below this value count as failures. |
| `proposal_id_column` | `proposal_id` | Proposal idempotency key column. |
| `proposal_source_type_column` | `source_type` | Proposal source type column. |
| `proposal_action_column` | `proposed_action` | Proposed action column. |
| `proposal_ts_column` | `source_ts_ns` | Proposal timestamp column. |
| `history_action_column` | `action` | Historical action column. |
| `history_outcome_score_column` | `outcome_score` | Historical outcome score column. |
| `decision_proposal_id_column` | `proposal_id` | Decision table proposal key column. |
| `decision_decision_column` | `decision` | Decision label column. |
| `decision_final_action_column` | `final_action` | Advisory final action column. |
| `decision_reason_column` | `reason` | Decision reason column. |
| `decision_evidence_count_column` | `evidence_count` | Decision evidence count column. |
| `decision_fail_closed_column` | `fail_closed` | Fail-closed marker column. |
| `decision_ts_column` | `decided_ts_ns` | Decision timestamp column. |
| `evidence_proposal_id_column` | `proposal_id` | Evidence table proposal key column. |
| `evidence_count_column` | `evidence_count` | Evidence count column. |
| `evidence_reason_column` | `reason` | Evidence reason column. |
| `evidence_ts_column` | `written_ts_ns` | Evidence timestamp column. |
| `commit_table` | `physical_ai_supervision_commits` | Atomic commit ledger table for complete decision/evidence state. |
| `commit_proposal_id_column` | `proposal_id` | Commit table proposal key column. |
| `commit_decision_column` | `decision` | Committed decision label column. |
| `commit_final_action_column` | `final_action` | Committed advisory final action column. |
| `commit_reason_column` | `reason` | Committed reason column. |
| `commit_evidence_count_column` | `evidence_count` | Committed evidence count column. |
| `commit_fail_closed_column` | `fail_closed` | Committed fail-closed marker column. |
| `commit_decision_written_column` | `decision_written` | Projection repair marker for decision row. |
| `commit_evidence_written_column` | `evidence_written` | Projection repair marker for evidence row. |
| `commit_ts_column` | `committed_ts_ns` | Commit timestamp column. |
| `worker_ownership_required` | `false` | Require a matching SQL ownership row before loading proposals. |
| `worker_lease_enabled` | `false` | Acquire/renew an expiring SQL ownership lease before loading proposals. Requires `worker_ownership_required=true`. |
| `worker_owner_id` | `""` | This server/worker owner id. Required when ownership is enabled. |
| `worker_owner_epoch` | `0` | Fencing epoch; must match the ownership row. |
| `worker_lease_ttl_ms` | `15000` | Managed SQL lease time-to-live. |
| `ownership_table` | `physical_ai_supervisor_ownership` | SQL table used for owner id/epoch fencing. |
| `ownership_supervisor_column` | `supervisor_name` | Supervisor name column in the ownership table. |
| `ownership_owner_id_column` | `owner_id` | Owner id column in the ownership table. |
| `ownership_epoch_column` | `owner_epoch` | Owner epoch column in the ownership table. |
| `ownership_lease_expires_at_column` | `lease_expires_at_ns` | Lease expiry timestamp column. |
| `ownership_heartbeat_ts_column` | `heartbeat_ts_ns` | Last heartbeat timestamp column. |

Default SQL contract:

```sql
CREATE TABLE IF NOT EXISTS physical_ai_action_proposals (
  proposal_id STRING,
  source_type STRING,
  proposed_action STRING,
  source_ts_ns TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_action_history (
  action STRING,
  outcome_score INT64,
  source_ts_ns TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_supervision_decisions (
  proposal_id STRING,
  decision STRING,
  final_action STRING,
  reason STRING,
  evidence_count INT64,
  fail_closed BOOL,
  decided_ts_ns TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_supervision_evidence (
  proposal_id STRING,
  evidence_count INT64,
  reason STRING,
  written_ts_ns TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_supervision_commits (
  proposal_id STRING,
  decision STRING,
  final_action STRING,
  reason STRING,
  evidence_count INT64,
  fail_closed BOOL,
  decision_written BOOL,
  evidence_written BOOL,
  committed_ts_ns TIMESTAMP_NS
);

CREATE TABLE IF NOT EXISTS physical_ai_supervisor_ownership (
  supervisor_name STRING,
  owner_id STRING,
  owner_epoch INT64,
  lease_expires_at_ns INT64,
  heartbeat_ts_ns TIMESTAMP_NS
);
```

The SQL adapter writes one atomic commit ledger row before repairing the
decision and evidence projection tables. Duplicate checks read the commit
ledger; retries repair missing projection rows without duplicating decision or
evidence rows. This provides an effectively-once sink contract for the current
supervisor path, but it is not a generic multi-table SQL transaction. Proposal
loading reads committed proposals first as projection-repair candidates, then
reads undecided proposals through a commit-ledger anti-join so committed
prefixes cannot starve new work at `proposal_query_limit`. When
`worker_lease_enabled=true`, the adapter acquires or renews the SQL ownership
lease and can take over an expired owner with a higher epoch; this is not a
full consensus election subsystem.

#### `DELETE /admin/action-outcome-supervisor` - Disable and clear config

```bash
curl -X DELETE http://localhost:8123/admin/action-outcome-supervisor \
  -H "Authorization: Bearer $ADMIN_KEY"
```

This endpoint stops the worker when enabled and clears the process-local
runtime config. If server-local SQL adapter config persistence is enabled, it
also removes the persisted adapter config file. If SQL catalog-backed config
is enabled, it clears matching catalog rows.

This lifecycle surface is experimental. Supervisor configuration can be
server-local or SQL catalog-backed when the embedding server enables those
persistence hooks. Decisions are advisory, and the runtime must not be
described as actuator enforcement until the promotion gates in
`docs/research/EXPERIMENT_GOVERNANCE.md` pass.

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

Covered statement kinds (devlog 090):

- `SELECT ... FROM <table>`
- `INSERT INTO <table> ...`
- `UPDATE <table> ...`
- `DELETE FROM <table> ...`
- `CREATE TABLE <table> ...`
- `DROP TABLE <table>`
- `ALTER TABLE <table> ...`
- `DESCRIBE <table>`

A key with `allowed_tables: ["trades"]` attempting `CREATE TABLE forbidden (...)`
or `DESCRIBE secret` receives `403 Forbidden`. `SHOW TABLES` is not table-scoped
and is not gated by this ACL.

### Tenant Namespace Enforcement (devlog 090)

When an API key / JWT carries a non-empty `tenant_id` and the server has a
`TenantManager` configured with a `table_namespace` for that tenant, every
POST / query is additionally checked against the namespace prefix. A tenant
with `table_namespace: "deskA."` cannot query / write / describe any table
whose name does not start with `deskA.` — the server returns:

```
HTTP/1.1 403 Forbidden
{"error":"Tenant 'deskA' cannot access table 'deskB.trades'"}
```

Tenant namespace enforcement runs after the `allowed_tables` check and
before query execution. Tenants with empty `table_namespace` are unrestricted.

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
