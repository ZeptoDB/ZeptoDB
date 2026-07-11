# Agent Memory Layer

Date: 2026-05-26
Status: v0 implemented

## Purpose

ZeptoDB keeps its core identity as a high-throughput in-memory columnar/time-series
engine. The Agent Memory layer is an additive subsystem on top of that engine for
teams that want to attach LLM agents to live time-series workflows. It provides
fast working memory, context retrieval, and application-level prompt cache hits
before calling an external model provider.

The layer is not intended to reposition ZeptoDB as a standalone vector database
or generic agent-memory product. Its strongest role is to sit beside ZeptoDB's
existing domains — finance, IoT, observability, robotics, games, and other
event-heavy systems — where the time-series engine stores what happened and the
Agent Memory layer stores what an agent learned, decided, or should reuse.

The layer deliberately does not call embedding providers or LLMs. v0 accepts
client-supplied `float32` embeddings and focuses on storage, filtering, ranking,
context assembly, and exact/semantic cache lookup.

## Data Model

`MemoryRecord` contains:

- `memory_id`
- `tenant_id`
- `namespace`
- `user_id`
- `session_id`
- `agent_id`
- `type`
- `content`
- `metadata_json`
- `embedding`
- `token_count`
- `importance`
- `created_at_ns`
- `last_accessed_ns`
- `expires_at_ns`
- `pinned`
- `access_count`

`CacheEntry` contains:

- `cache_id`
- `tenant_id`
- `namespace`
- `prompt`
- `response`
- `metadata_json`
- `embedding`
- `token_count`
- `created_at_ns`
- `last_accessed_ns`
- `expires_at_ns`
- `access_count`

The v0 implementation stores records in an in-memory `AgentMemoryStore` guarded by
a mutex. Embeddings are held as row-major `std::vector<float>` payloads in the AI
subsystem instead of being exposed as a SQL column type. The first non-empty
embedding establishes the store embedding dimension; later non-empty embeddings
must match and must not contain NaN or Inf values.

## Persistence

Agent memory persistence is implemented as sidecar snapshot files instead of SQL
columns. In standalone mode the files live directly under `--agent-memory-dir`:

```
{agent_memory_dir}/records.bin
{agent_memory_dir}/vectors.bin
```

In routed multi-node mode, the HTTP server stores this node's local Agent Memory
snapshot under a shard-local path:

```
{agent_memory_dir}/node-{node_id}/shard-0/records.bin
{agent_memory_dir}/node-{node_id}/shard-0/vectors.bin
{agent_memory_dir}/node-{node_id}/shard-0/manifest.json
{agent_memory_dir}/node-{node_id}/shard-0/wal.log
```

`records.bin` stores scalar fields, strings, counters, timestamps, and vector
offsets. `vectors.bin` stores row-major `float32` embeddings. Both files carry a
small magic/version header. `AgentMemoryStore::save_to_directory()` writes
temporary files and then publishes them with rename; `load_from_directory()`
loads into temporary containers first and only swaps the live store after the
snapshot validates.

`manifest.json` is written by the HTTP server in routed mode and records
`node_id`, `shard_id`, and `ring_epoch`. Startup validates the manifest before
loading the store so a pod does not accidentally load another node's shard
snapshot. The first implementation uses one local shard directory per node
(`shard-0`); multi-shard migration and replica ownership arrive with the WAL and
failover work.

`wal.log` is an append-only mutation log for `PUT_MEMORY` and `STORE_CACHE`.
Current writes are encoded as `PREPARE_MEMORY`/`PREPARE_CACHE` plus a `COMMIT`
marker, while legacy committed `PUT_MEMORY`/`STORE_CACHE` records remain
replayable for upgrade compatibility. Replay applies only committed prepared
records and ignores trailing prepared records without a commit marker, so a
failed quorum/sync write does not resurrect after restart. The HTTP server
truncates the WAL after a successful snapshot publish. When
`--agent-memory-replication-mode quorum` or `sync` is enabled, the owner also
sends the same prepared record and commit marker to configured Agent Memory
replicas. Replicas append it under the source owner path
(`node-{source_node_id}/shard-0/wal.log`) without applying it to their live
store, so it is available for later explicit shard adoption. Explicit HTTP/RPC
memory and cache deletes are recorded as committed tombstones. Owner-side
HTTP/RPC writes also persist tombstones for automatic TTL, tenant-quota, and
capacity evictions caused by that write, so restart replay and replica shard
adoption do not resurrect entries the live owner already evicted.

`zepto_http_server --agent-memory-dir <path>` enables persistence explicitly. If
HDB is enabled via `--hdb-dir` or `--storage-mode tiered`, the default sidecar
path is `<hdb_base>/agent_memory`. The HTTP server saves after every
`--agent-memory-flush-every` memory/cache mutations and again during `stop()`.
The default threshold is `100`, `1` gives write-through snapshots, and `0`
defers automatic snapshots until stop while still retaining replayable local WAL
records. The threshold trades restart replay length for lower mutation-path
snapshot latency.

## Retrieval

`search` applies tenant, namespace, user, session, agent, type, and TTL filters,
then ranks candidates with a bounded top-K heap sized by `limit`. With ANN
disabled, this is an exact filtered scan that avoids a full sort when only a
small context window is requested. Large full scans are parallelized internally
and still return exact top-K results. With ANN enabled, a partitioned
sparse-projection index first returns semantic candidates for the exact
`(tenant_id, namespace)` scope, and the same filter/rank path reranks those
candidates. If the ANN path cannot return enough filtered candidates, search
falls back to the exact scan.
Ranking combines:

- cosine similarity when a query embedding is supplied
- pinned-memory boost
- importance score
- recency score
- access-count score

`get_context` uses the same ranking path, then deduplicates identical content and
selects records under an optional token budget. Records with `token_count == 0`
use a local rough estimate of one token per four bytes.

Search normally updates `last_accessed_ns` and `access_count` for returned
records so recency/access-count ranking reflects live retrievals. Diagnostic and
benchmark callers can set `MemoryQuery::update_access = false` to compare exact
and ANN paths without perturbing later rankings.

The ANN index is derived in-memory state, not a durability boundary. It is
rebuilt from `MemoryRecord` vectors after snapshot load or after mutations mark
the index dirty. Search does not build a dirty ANN index inline. It requests the
store's background ANN worker and falls back to the exact scan while the
replacement builds. Rebuilds take a memory-vector snapshot under the store
mutex, build the next ANN structure outside that mutex, and swap it in only if
no newer mutation superseded the snapshot. A clean ANN index also accepts memory
inserts, embedding updates, deletes, and compacting row-id remaps incrementally,
avoiding a full rebuild for common live maintenance paths. Updates that preserve
tenant, namespace, and embedding do not dirty ANN at all. If incremental
maintenance cannot be applied, the store marks ANN dirty and falls back to exact
scan until the background worker publishes a fresh replacement. TTL/capacity
eviction, snapshot loads, and config changes still wait for a fresh background
index before using ANN candidates again. CLI defaults keep ANN off;
`--agent-memory-ann auto` uses the sparse-projection backend after
`--agent-memory-ann-min-records` is reached, while
`--agent-memory-ann sparse_projection` forces sparse projection for benchmarking.
Builds configured with `ZEPTO_ENABLE_HNSWLIB=ON` also accept
`--agent-memory-ann hnsw`, backed by hnswlib over normalized vectors with L2
distance, which is order-equivalent to cosine similarity. `--agent-memory-ann
ivf` enables a dependency-free inverted-file baseline with
`--agent-memory-ann-ivf-centroids` and `--agent-memory-ann-ivf-probe` tuning.

Current benchmark results show sparse projection can be very fast for candidate
generation, but recall is workload-sensitive. HNSW has much better semantic-only
recall at low search latency, but first-build graph construction is far slower
and mixed ranking recall can still be low because final ranking includes
importance, recency, pinned status, and access count. IVF is available as a
dependency-free baseline between sparse projection and HNSW; it is useful for
comparison and constrained builds, not yet the default production policy.
Recall-sensitive deployments should keep exact scan as the default until larger
production embedding-dump evaluation selects a policy. The `bench_agent_memory`
harness supports `--fixture mixed|semantic|clustered|real`; `clustered` uses
deterministic center-plus-noise embeddings and fixture-matched query vectors,
while `real` loads precomputed vector-only embedding files with
`--embedding-file PATH`. The real fixture accepts comma, semicolon, or
whitespace-separated finite floats with optional brackets and comments, then
uses the loaded vectors for memories, cache entries, and recall queries.
Production policy evaluation can set `--tenant-count` and
`--query-tenant-index` to simulate tenant-filter-heavy distributions where only
a small slice of the global embedding dump is visible to a request. The compare
table includes sparse, IVF, and HNSW profiles plus tenant count, ANN memory
bytes, snapshot sidecar bytes, recall thresholds, build thresholds, optional
memory thresholds, and an explicit policy status (`scan_ok`, `needs_ann`,
`eligible`, or a named rejection reason). This keeps exact scan as the default
until a real production dump passes recall, latency, build, and footprint gates.

## Eviction

`AgentMemoryEvictionConfig` bounds in-memory growth without making eviction part
of SQL storage. A limit of `0` means unbounded. The HTTP server exposes the
capacity knobs as `--agent-memory-max-memories` and
`--agent-memory-max-cache-entries`; the C++ API exposes the full runtime config.

Writes optionally remove expired memory/cache entries first
(`evict_expired_on_write=true` by default), then enforce configured capacity.
Capacity eviction keeps pinned memories by default and ranks eviction candidates
with a retention score based on importance, access count, and recency. Explicit
TTL expiry still removes pinned memories, because expiry is treated as caller
intent. If every remaining memory is pinned and `protect_pinned=true`, the store
allows capacity overflow instead of evicting pinned records.

The store tracks whether any memory/cache entries have TTLs. Write-time expired
cleanup skips the vector scan entirely when no expiring entries exist, which
keeps bulk loads and non-TTL agent memory workloads linear.

## Cache

Exact cache lookup normalizes the prompt by trimming repeated whitespace and
lowercasing before keying by `(tenant_id, namespace, normalized_prompt)`.

Semantic cache lookup scans live entries for the same `(tenant_id, namespace)` and
returns the best embedding similarity above the caller-provided threshold. This is
an application-level cache that runs before provider calls; it complements rather
than replaces provider-native prompt caching.

## HTTP And Python Surfaces

HTTP endpoints:

- `GET /api/ai/stats`
- `POST /api/ai/memories`
- `GET /api/ai/memories/:memory_id`
- `DELETE /api/ai/memories/:memory_id`
- `POST /api/ai/memories/search`
- `POST /api/ai/context`
- `POST /api/ai/cache/store`
- `DELETE /api/ai/cache`
- `POST /api/ai/cache/lookup`

The high-level Python HTTP client exposes matching helpers as `connection.memory`
and `connection.cache`.

## Observability

`GET /api/ai/stats` exposes aggregate counts, eviction counters, embedding
dimension, snapshot health, ANN status, eviction config, and the last local
owner-failover status. It deliberately omits memory content, prompts,
responses, and metadata.

`GET /metrics` exports Prometheus-compatible Agent Memory gauges and counters:

- `zepto_agent_memory_records`
- `zepto_agent_cache_entries`
- `zepto_agent_memory_embedding_dim`
- `zepto_agent_memory_evictions_total`
- `zepto_agent_cache_evictions_total`
- `zepto_agent_memory_max_records`
- `zepto_agent_cache_max_entries`
- `zepto_agent_memory_snapshot_latency_seconds`
- `zepto_agent_memory_snapshot_failures_total`
- `zepto_agent_memory_snapshot_records_bytes`
- `zepto_agent_memory_snapshot_vectors_bytes`
- `zepto_agent_memory_snapshot_total_bytes`
- `zepto_agent_memory_ann_memory_bytes`
- `zepto_agent_memory_ann_tombstone_entries`

AgentOps telemetry is modeled as ordinary ZeptoDB time-series tables beside the
Agent Memory subsystem. The example schema in `examples/agent_memory/` defines
`agent_runs`, `retrieval_events`, `cache_events`, `context_traces`,
`context_replay_events`, `llm_calls`, `llm_errors`, and `tool_calls`. Those
tables track operational facts about agent turns, while Agent Memory keeps the
durable context objects selected for future prompts.

Context trace/replay helpers are available in
`examples/agent_memory/context_trace.py`. They record one row per selected
memory with rank, score, similarity, token count, and a compact reason string,
plus replay rows for the time-series queries observed around the decision.

OpenTelemetry/GenAI span mapping is available in
`examples/agent_memory/otel_mapping.py`. It maps OTLP JSON spans into AgentOps
INSERT statements for LLM provider/model calls, prompt/completion token counts,
cache hits, tool calls, latency, and model errors. This keeps collection
pluggable while preserving a stable ZeptoDB table shape for agent observability.

The EKS bench harness supports a focused Agent Memory validation mode:

```bash
tests/k8s/run_eks_bench.sh --agent-only --skip-wake --keep
```

`--agent-only` preserves the normal EKS wake/node-readiness checks unless
`--skip-wake` is supplied, runs `tests/k8s/test_k8s_agent_memory.py` against the
configured amd64 and arm64 bench images, and skips the general compat/HA and
native engine benchmark stages. This is the preferred iteration path after the
core EKS harness is already green.

## Security And Isolation

Every memory and cache entry carries `tenant_id` and `namespace`. HTTP requests may
also carry `X-Zepto-Tenant-Id`; when present, it is copied into the request object
or rejected if it conflicts with the JSON `tenant_id`.

The layer does not log memory content or prompt/response payloads. Metadata is
stored as caller-provided JSON text so applications can supply retrieval hints such
as `source`, `type`, or `trusted` for future ranking and prompt-injection defenses.

## v0 Limits

- No server-side embedding generation.
- No server-side LLM calls or automatic summarization.
- No SQL vector type.
- Persistence is sidecar snapshot plus owner-local and optional replica WAL for
  memory/cache upserts, explicit delete tombstones, and write-triggered
  automatic TTL/tenant-quota/capacity eviction tombstones; it is not yet
  encrypted or background eviction WAL complete.
- ANN remains experimental. Sparse projection, optional hnswlib HNSW, and IVF
  are candidate-generation backends only; persisted ANN index sidecars and a
  robust production default policy are not yet implemented.
- Full shard migration dual-write/catch-up is not yet implemented.

## Multi-Node Behavior Today

In standalone mode, Agent Memory remains node-local. In routed cluster mode,
`POST /api/ai/memories`, `DELETE /api/ai/memories/:memory_id`,
`POST /api/ai/cache/store`, and `DELETE /api/ai/cache` can route to the owner
node through `TcpRpc` write messages. Point memory lookup and exact prompt-cache
lookup route to the owner through
`AGENT_MEMORY_GET` and `AGENT_CACHE_LOOKUP_EXACT`. Search fans out through
`AGENT_MEMORY_SEARCH`; context uses the same fan-out search and applies
deduplication plus token-budget selection after the global merge. Semantic cache
fallback also fans out to configured Agent Memory nodes after the exact prompt
owner misses. `HttpServer` owns one local `AgentMemoryStore`; remote writes are
applied by the owner pod's store and return the owner-generated id to the
original HTTP caller. As a result:

- A routed write sent to pod A can land on owner pod B.
- Remote write clients inherit the Agent Memory `ring_epoch`, so data nodes with
  the existing `TcpRpcServer` fencing token enabled reject stale owner
  mutations.
- A remote owner returns ACK only after applying the mutation to its local
  `AgentMemoryStore` and appending a committed owner-local WAL transaction when
  persistence is enabled. If persistence or quorum/sync replication fails before
  commit, the owner rolls back the live memory/cache mutation and restores any
  write-triggered eviction side effects whose tombstones are not durable.
- With replication mode `quorum`, the owner prepares locally, waits for enough
  replica prepare/commit acknowledgement to reach a majority with the local
  commit, then ACKs. With `sync`, it waits for every configured replica before
  publishing the local commit marker and ACKing.
- A later point lookup by `memory_id`, or exact cache lookup by normalized prompt,
  can be served from pod C and routed back to owner pod B.
- A search or context request sent to pod C scatters to the current Agent Memory
  nodes, merges each node's local top-K by score and `created_at_ns`, and then
  trims to the requested limit.
- A semantic cache fallback sent to pod C can find cache entries owned by pod B
  when pod B is configured as an Agent Memory remote.
- In routed mode, auto-generated ids use owner-scoped
  `mem_<node_id>_<epoch>_<counter>` / `cache_<node_id>_<epoch>_<counter>`.
  Standalone mode keeps local `mem_N` and `cache_N`.
- Caller-supplied memory ids route by the same logical key used for writes; HTTP
  point lookup callers should pass the original namespace when it is not
  `default`.
- Each pod writes its own shard-local sidecar snapshot path under
  `node-{node_id}/shard-0`, so multiple pods can share the same base
  `--agent-memory-dir` without overwriting each other's snapshot files.
- Eviction and TTL cleanup are local, so global capacity and pinned-memory
  guarantees do not hold cluster-wide.
- `/api/ai/stats?scope=local` and Prometheus gauges are per-pod values.
  `/api/ai/stats?scope=cluster` scatters over configured routed Agent Memory
  nodes and returns aggregate plus per-node counters, with `partial_failures`
  for missing clients or invalid remote stats responses.
- With `zepto_http_server --failover-enabled` in non-HA cluster mode,
  `FailoverManager::on_failover` advances the Agent Memory ring epoch and calls
  deterministic successor adoption for the failed owner shard.

The current safe operating rule is therefore: memory writes/deletes, point reads,
exact cache reads/deletes, search, context, and owner-local restart
snapshots/WAL replay can use routed HTTP. Failed-owner replay is available
through explicit shard adoption or the non-HA `--failover-enabled` process
wiring. Quorum/sync replica WAL durability is available for memory/cache upserts
and explicit or write-triggered automatic eviction tombstones. Cluster-scoped
stats are available through routed RPC scatter. Successor replay promotion and
local degraded-state reporting are available; global eviction/quota
reconciliation is still not cluster-consistent.

## Multi-Node Design

### Goals

- Keep single-node latency and API behavior unchanged by default.
- Make memory/cache writes deterministic under load-balanced HTTP traffic.
- Support cluster-wide search and context assembly with tenant/namespace
  isolation.
- Avoid split-brain writes by reusing the cluster epoch/fencing model.
- Persist per-shard snapshots without shared-directory overwrite hazards.
- Provide clear consistency modes so operators can choose latency vs durability.

### Ownership Model

Agent Memory should use a separate consistent-hash ring keyed by:

```
tenant_id + namespace + logical_subject + entry_kind
```

`logical_subject` is selected in this order:

1. `session_id`, when present.
2. `agent_id`, when present.
3. `user_id`, when present.
4. `memory_id` or normalized cache prompt hash.

This keeps most conversational context co-located while still distributing
tenant-wide workloads. The ring can be implemented with the existing
`PartitionRouter` pattern, but it should be exposed as an `AgentMemoryRouter`
wrapper so memory routing can evolve independently from time-series
`table_id/symbol/hour` partitioning.

### Write Path

`POST /api/ai/memories`, `DELETE /api/ai/memories/:memory_id`,
`POST /api/ai/cache/store`, and `DELETE /api/ai/cache` should route writes to
the owner node:

```
HTTP pod -> AgentMemoryRouter.route(key)
    local owner  -> AgentMemoryStore::put_memory/store_cache/remove_*
    remote owner -> TcpRpc AGENT_MEMORY_PUT / AGENT_CACHE_STORE / *_DELETE
```

Remote writes carry the current cluster epoch in the existing `RpcHeader`. A data
node rejects stale epochs the same way it rejects stale `TICK_INGEST` and
`WAL_REPLICATE` messages. Auto ids become owner-scoped ids:

```
mem_<node_id>_<epoch>_<counter>
cache_<node_id>_<epoch>_<counter>
```

Caller-supplied ids remain accepted, but updates must route by supplied id and
must reject tenant conflicts globally.

### Read Path

`get_memory(memory_id)` is a point lookup and routes to the id owner.

`search`, `context`, and semantic cache lookup are fan-out operations:

1. Coordinator sends the filtered query to all candidate memory shards. The first
   version can scatter to every live node; a later version can prune by
   tenant/namespace shard map.
2. Each node runs local top-K using the existing bounded heap.
3. Coordinator merges partial top-K results with the same `score` and
   `created_at_ns` tie-breaker.
4. Context assembly deduplicates content and applies the token budget only after
   the global merge, so a large local result from one node cannot starve better
   matches from another node.

Exact prompt cache lookup should not fan out. The normalized prompt hash is part
of the routing key, so the coordinator can route directly to the owner. Semantic
cache lookup fans out after exact miss because similarity is not key-addressable
without an ANN or centroid index.

### Persistence

Sidecar snapshots become shard-local:

```
{agent_memory_dir}/node-{node_id}/shard-{shard_id}/records.bin
{agent_memory_dir}/node-{node_id}/shard-{shard_id}/vectors.bin
```

Every snapshot stores:

- `node_id`
- `shard_id`
- `ring_epoch`
- `next_memory_id`
- `next_cache_id`
- embedding dimension

Startup loads only shards currently owned by the node. During ownership changes,
the destination node must load or receive the shard before the router publishes
the new owner epoch.

### Durability Modes

`--agent-memory-replication-mode` controls the owner ACK policy:

| Mode | Behavior | Use case |
|---|---|---|
| `local` | Current node-local behavior | Dev, cheapest cache |
| `routed` | Single owner, no replica ACK | Low-latency working memory |
| `quorum` | Owner prepares local WAL, WAL-replicates to a majority, commits, then ACKs | Production memory |
| `sync` | Owner prepares local WAL and waits for all configured replicas before commit/ACK | Maximum durability |

The current implementation uses Agent Memory replica append RPCs rather than
tick WAL rows:

```
PREPARE_MEMORY, PREPARE_CACHE, PREPARE_DELETE_MEMORY,
PREPARE_DELETE_CACHE, COMMIT
```

Prepared records without `COMMIT` are intentionally ignored during replay.
Replica records are idempotent for upserts because memory/cache ids are stable.
Explicit deletes and write-triggered automatic TTL/tenant-quota/capacity
evictions are stored as committed tombstones. If a write-triggered tombstone
fails after some earlier tombstones are durable, the owner restores only the
failed and later evicted entries before returning an error. Standalone
background eviction batches, abort markers, and monotonic per-shard sequence
numbers remain future work before this becomes a complete replicated mutation
log.

### Rebalancing And Failover

Agent Memory shard migration follows the cluster partition migration pattern:

1. Mark shard `MIGRATING`.
2. Snapshot source shard to destination.
3. Dual-write mutations to source and destination.
4. Catch up mutation sequence gap.

The current implementation provides two recovery primitives:

- `HttpServer::adopt_agent_memory_owner_shard(source_node_id,
  source_ring_epoch)` validates the source shard manifest, loads its snapshot,
  replays its `wal.log`, merges the records into the replacement node's live
  store, and publishes the replacement node's current shard snapshot.
- `HttpServer::handle_agent_memory_owner_failover(source_node_id,
  source_ring_epoch, new_ring_epoch, live_nodes)` is the automatic orchestration
  hook intended for `FailoverManager::on_failover` callbacks. It removes the
  failed owner from the local Agent Memory ring, advances surviving nodes to the
  new ring epoch, persists the local shard under that epoch, and lets only the
  deterministic successor adopt the failed owner shard. The deterministic
  successor is the first live node id greater than the failed node id, wrapping
  to the lowest live node id.
- `zepto_http_server --failover-enabled` wires `HealthMonitor` to
  `FailoverManager` in non-HA cluster mode. The process tracks the current Agent
  Memory ring epoch, increments it after a node is declared dead, and invokes
  the failover hook with the post-removal live-node list.

Owner-scoped ids from the removed node fall back to current-ring routing for
point lookup when the embedded owner node is no longer present in the Agent
Memory ring.
5. Publish ring epoch with destination owner.
6. Stop dual-write after all nodes observe the new epoch.

On node failure, the minimal recovery path is deterministic successor adoption
from the failed owner's persisted shard. Successful successor replay reports
`replica_promoted=true` in `AgentMemoryOwnerFailoverResult`; when the successor
has no replay source, the failover still completes but reports
`degraded=true`, `replay_source_missing=true`, and a `degraded_reason`. The same
last-failover state is exposed in local `/api/ai/stats`. Full migration
dual-write catch-up remains future work.

### Global Eviction

Capacity limits are two-level:

- Local hard limits protect each process from OOM.
- Optional tenant/namespace quotas protect shared clusters from noisy tenants.

`AgentMemoryEvictionConfig::tenant_quotas` applies local scoped quotas before
global caps. A quota with `tenant_id` and an empty `namespace_id` limits all
entries for that tenant; a quota with both fields set limits only that tenant
namespace. Memory and cache limits are independent. Pinned memory protection
remains shard-local and can allow quota overflow, while explicit TTL expiry
still removes pinned records.

Cluster-wide quota reconciliation remains future work: a controller can compute
tenant usage across shards and send per-shard target reductions.

### Observability

Local Agent Memory stats expose snapshot publish health through
`/api/ai/stats?scope=local` and Prometheus. `save_to_directory()` records the
last snapshot attempt duration as `snapshot_latency_seconds` /
`zepto_agent_memory_snapshot_latency_seconds` and failed snapshot attempts as
`snapshot_failures_total` /
`zepto_agent_memory_snapshot_failures_total`.

`GET /api/ai/stats?scope=cluster` defaults to the same local stats when routing
is not configured. In routed mode, the HTTP server requests
`AGENT_MEMORY_STATS` from each configured remote node, returns aggregate
memory/cache, eviction, snapshot, quota, and ANN counters for successful
responses, and preserves per-node errors under `partial_failures`.

Add cluster-aware metrics:

- `zepto_agent_memory_shards`
- `zepto_agent_memory_owned_shards`
- `zepto_agent_memory_replica_lag`
- `zepto_agent_memory_wal_replay_lag`
- `zepto_agent_memory_remote_write_errors_total`
- `zepto_agent_memory_fanout_partial_failures_total`

### Rollout Plan

1. Done in devlog 122: add `AgentMemoryRouter` and owner-scoped ids while
   keeping default mode `local`. This is a routing foundation only; it does not
   dispatch remote writes yet.
2. Done in devlogs 124 and 126: routed remote write RPCs, point memory lookup,
   and exact prompt-cache lookup.
3. Done in devlog 128: fan-out search/context merge. The first implementation
   uses strict partial-failure behavior and returns an HTTP 502 if a remote
   memory shard cannot answer.
4. Done in devlog 130: shard-local snapshot paths and startup ownership
   validation for the node-local `shard-0` snapshot.
5. Done in devlog 131: local Agent Memory WAL append/replay for `PUT_MEMORY`
   and `STORE_CACHE`.
6. Done in devlog 135: owner WAL replica append RPC plus `quorum`/`sync`
   acknowledgement policies for memory/cache upserts.
7. Done in devlog 136: Agent Memory WAL prepare/commit markers plus live
   rollback on failed persisted writes.
8. Done in devlog 137: explicit memory/cache delete endpoints and committed WAL
   tombstones.
9. Done in devlog 138: deterministic owner failover hook with routed epoch
   advance and successor-only shard adoption.
10. Done in devlog 163: local tenant/namespace memory and cache quotas.
11. Done in devlog 166: cluster-scoped HTTP stats with per-node partial
    failure reporting.
12. Done in devlog 167: write-triggered automatic TTL, tenant-quota, and
    capacity eviction tombstones.
13. Done in devlog 168: successor replay promotion and degraded-state reporting
    when no replay source exists.
14. Done in devlog 169: rollback for write-triggered capacity-eviction side
    effects when primary durability or tombstone persistence fails.
15. Add shard migration and dual-write catch-up.

### Test Plan

- Done in devlog 128: two-node routed write followed by search/context from the
  non-owner HTTP pod.
- Auto-id uniqueness across nodes and epochs.
- Tenant conflict rejection on caller-supplied ids routed through different pods.
- Done in devlog 128: global top-K merge ordering and token-budget context
  assembly.
- Exact cache direct routing and semantic cache fan-out.
- Stale epoch remote write rejection.
- Done in devlog 130: snapshot path isolation for multiple pods using the same
  base directory and wrong-owner manifest rejection.
- Shard migration dual-write catch-up with in-flight writes.
- Done in devlog 138: deterministic owner failover adoption after node removal.
- Done in devlog 166: cluster stats aggregate reachable nodes and report missing
  remote stats clients as partial failures.
- Done in devlog 168: failover replay promotion status and degraded-state
  reporting when replay is impossible.
- Done in devlog 169: failed sync-replication durability restores
  write-triggered memory/cache capacity evictions and replays without the
  failed write or non-durable tombstone.
- Partial fan-out failure in `strict` and `best_effort` modes.

## Next Steps

- Run larger production embedding dumps through the tenant-heavy ANN policy gate
  before choosing a default ANN policy.
- Revisit persisted ANN index sidecars only if rebuild time becomes the
  operational bottleneck after production-dump testing.
- Optional enterprise embedding provider integration.
