# ZeptoDB C++ API Reference

*Last updated: 2026-06-12*

---

## Table of Contents

- [ZeptoPipeline](#zeptopipeline)
- [QueryExecutor (SQL)](#queryexecutor-sql)
- [PartitionManager & Partition](#partitionmanager--partition)
- [TickMessage](#tickmessage)
- [Telegraf Output Helpers](#telegraf-output-helpers)
- [AgentMemoryStore](#agentmemorystore)
- [AgentMemoryRouter](#agentmemoryrouter)
- [Auth — CancellationToken](#auth--cancellationtoken)

---

## Quick Start

### Complete example: ingest ticks, run SQL, read raw columns

```cpp
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/common/types.h"
#include <iostream>

int main() {
    using namespace zeptodb::core;
    using namespace zeptodb::sql;

    // 1. Create and start pipeline (pure in-memory)
    ZeptoPipeline pipeline;
    pipeline.start();

    // 2. Ingest ticks
    for (int i = 0; i < 1000; ++i) {
        TickMessage msg;
        msg.symbol_id = 1;
        msg.price     = 15000 + i * 10;   // 15000, 15010, ..., 24990
        msg.volume    = 100 + i;
        msg.recv_ts   = now_ns();
        pipeline.ingest_tick(msg);
    }
    pipeline.drain_sync();   // flush to column store synchronously

    // 3. Direct query (C++ API)
    auto r = pipeline.query_vwap(1);
    std::cout << "VWAP: " << r.value
              << "  rows_scanned: " << r.rows_scanned << "\n";

    // 4. SQL query
    QueryExecutor exec{pipeline};
    exec.enable_parallel();

    auto result = exec.execute(
        "SELECT count(*), sum(volume), avg(price), vwap(price, volume) "
        "FROM trades WHERE symbol = 1"
    );

    if (!result.ok()) {
        std::cerr << "Error: " << result.error << "\n";
        return 1;
    }
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        std::cout << result.column_names[i] << " = "
                  << result.rows[0][i] << "\n";
    }
    std::cout << "Execution: " << result.execution_time_us << " μs\n";

    // 5. Zero-copy raw column access
    auto& pm    = pipeline.partition_manager();
    auto  parts = pm.get_partitions(1);
    for (auto* part : parts) {
        const int64_t* prices = part->get_column("price");
        size_t n = part->row_count();
        std::cout << "Partition: " << n << " rows, "
                  << "first price = " << prices[0] << "\n";
    }

    pipeline.stop();
    return 0;
}
```

### 5-minute bar aggregation via SQL

```cpp
auto result = exec.execute(R"sql(
    SELECT xbar(timestamp, 300000000000) AS bar,
           first(price) AS open,
           max(price)   AS high,
           min(price)   AS low,
           last(price)  AS close,
           sum(volume)  AS volume
    FROM trades
    WHERE symbol = 1
    GROUP BY xbar(timestamp, 300000000000)
    ORDER BY bar ASC
)sql");

for (const auto& row : result.rows) {
    int64_t bar    = row[0];
    int64_t open   = row[1];
    int64_t high   = row[2];
    int64_t low    = row[3];
    int64_t close  = row[4];
    int64_t volume = row[5];
    std::cout << bar << " O=" << open << " H=" << high
              << " L=" << low << " C=" << close
              << " V=" << volume << "\n";
}
```

### Time-range query with partition pruning

```cpp
int64_t from_ns = now_ns() - 3600LL * 1'000'000'000LL; // last 1 hour
int64_t to_ns   = now_ns();

auto result = exec.execute(
    "SELECT vwap(price, volume), count(*) FROM trades "
    "WHERE symbol = 1 AND timestamp BETWEEN "
    + std::to_string(from_ns) + " AND " + std::to_string(to_ns)
);
```

---

## ZeptoPipeline

`#include "zeptodb/core/pipeline.h"` — Namespace: `zeptodb::core`

The top-level end-to-end pipeline: tick ingestion → column store → query execution.

### Construction

```cpp
#include "zeptodb/core/pipeline.h"
using namespace zeptodb::core;

// Default config (pure in-memory, 32 MB arena per partition)
ZeptoPipeline pipeline;

// Custom config
PipelineConfig cfg;
cfg.arena_size_per_partition = 64ULL * 1024 * 1024; // 64 MB
cfg.drain_batch_size         = 512;
cfg.drain_sleep_us           = 5;
cfg.storage_mode             = StorageMode::TIERED;
cfg.hdb_base_path            = "/data/zepto_hdb";
ZeptoPipeline pipeline{cfg};
```

### Lifecycle

```cpp
pipeline.start();        // start background drain thread
pipeline.stop();         // flush queue + stop drain thread

// Sync drain — useful in tests without background thread
size_t drained = pipeline.drain_sync();
size_t drained = pipeline.drain_sync(/*max_items=*/1000);
```

### PipelineConfig fields

```cpp
struct PipelineConfig {
    size_t   arena_size_per_partition = 32ULL * 1024 * 1024; // 32 MB
    size_t   drain_batch_size         = 256;
    uint32_t drain_sleep_us           = 10;
    StorageMode storage_mode          = StorageMode::PURE_IN_MEMORY;
    std::string hdb_base_path         = "/tmp/zepto_hdb";
    FlushConfig flush_config{};   // tiered mode HDB flush settings
};
```

### StorageMode

```cpp
enum class StorageMode : uint8_t {
    PURE_IN_MEMORY = 0,  // HFT: no HDB, maximum latency
    TIERED         = 1,  // RDB (today) + HDB (historical) hybrid
    PURE_ON_DISK   = 2,  // Backtesting: HDB only
};
```

### Ingest

```cpp
#include "zeptodb/common/types.h"

TickMessage msg;
msg.symbol_id = 1;
msg.price     = 15000;    // scaled integer
msg.volume    = 100;
msg.recv_ts   = now_ns(); // nanosecond timestamp

// Lock-free, thread-safe — returns false if ring buffer is full
bool ok = pipeline.ingest_tick(msg);
```

### Direct queries

```cpp
// VWAP (all time)
QueryResult r = pipeline.query_vwap(symbol_id);
if (r.ok()) double vwap = r.value;

// VWAP (time range)
QueryResult r = pipeline.query_vwap(symbol_id, from_ns, to_ns);

// Row count
QueryResult r = pipeline.query_count(symbol_id);
int64_t count = r.ivalue;

// Filter + sum: sum(col) WHERE col > threshold
QueryResult r = pipeline.query_filter_sum(symbol_id, "volume", 100);

// Total rows stored across all partitions
size_t total = pipeline.total_stored_rows();
```

### QueryResult

```cpp
struct QueryResult {
    enum class Type : uint8_t { VWAP, SUM, COUNT, ERROR };

    Type        type         = Type::ERROR;
    double      value        = 0.0;    // VWAP, AVG
    int64_t     ivalue       = 0;      // COUNT, SUM
    size_t      rows_scanned = 0;
    int64_t     latency_ns   = 0;
    std::string error_msg;

    bool ok() const { return type != Type::ERROR; }
};
```

### Statistics

```cpp
const PipelineStats& s = pipeline.stats();

s.ticks_ingested.load()        // total ticks received (queue push)
s.ticks_stored.load()          // ticks written to column store
s.ticks_dropped.load()         // dropped (ring buffer overflow)
s.queries_executed.load()
s.total_rows_scanned.load()
s.partitions_created.load()
s.last_ingest_latency_ns.load()
```

### Sub-component access

```cpp
PartitionManager& pm  = pipeline.partition_manager();
TickPlant&        tp  = pipeline.tick_plant();

// nullptr in PURE_IN_MEMORY mode
HDBReader*    hdb = pipeline.hdb_reader();
FlushManager* fm  = pipeline.flush_manager();
```

---

## QueryExecutor (SQL)

`#include "zeptodb/sql/executor.h"` — Namespace: `zeptodb::sql`

Parses SQL strings and executes them against `ZeptoPipeline`.

### Construction

```cpp
#include "zeptodb/sql/executor.h"
using namespace zeptodb::sql;

// Default: serial execution, LocalQueryScheduler
QueryExecutor exec{pipeline};

// Custom scheduler injection (testing or distributed)
auto sched = std::make_unique<MyDistributedScheduler>(...);
QueryExecutor exec{pipeline, std::move(sched)};
```

### Parallel execution

```cpp
// Enable parallel (auto = hardware_concurrency threads)
exec.enable_parallel();

// Enable with explicit settings
exec.enable_parallel(
    /*num_threads=*/8,
    /*row_threshold=*/100'000   // use serial for < 100k rows
);

exec.disable_parallel();

// Inspect current settings
const ParallelOptions& opts = exec.parallel_options();
opts.enabled        // bool
opts.num_threads    // size_t (0 = hardware_concurrency)
opts.row_threshold  // size_t
```

### Execute SQL

```cpp
QueryResultSet result = exec.execute(
    "SELECT vwap(price, volume), count(*) "
    "FROM trades WHERE symbol = 1"
);

if (!result.ok()) {
    std::cerr << "Error: " << result.error << "\n";
    return;
}

// Column names
for (const std::string& col : result.column_names) { ... }

// Rows — all values as int64
for (const std::vector<int64_t>& row : result.rows) {
    for (size_t i = 0; i < row.size(); ++i) {
        std::cout << result.column_names[i] << " = " << row[i] << "\n";
    }
}

std::cout << result.execution_time_us << " μs, "
          << result.rows_scanned << " rows scanned\n";
```

### Execute with cancellation token

```cpp
#include "zeptodb/auth/cancellation_token.h"

zeptodb::auth::CancellationToken token;

// Cancel from another thread
std::thread canceller([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    token.cancel();
});

QueryResultSet result = exec.execute(sql, &token);
canceller.join();

if (!result.ok()) {
    // result.error == "Query cancelled"
}
```

### QueryResultSet

```cpp
struct QueryResultSet {
    std::vector<std::string>          column_names;
    std::vector<ColumnType>           column_types;
    std::vector<std::vector<int64_t>> rows;        // all values as int64

    double      execution_time_us = 0.0;
    size_t      rows_scanned      = 0;
    std::string error;                             // empty if ok

    bool ok() const { return error.empty(); }
};
```

---

## PartitionManager & Partition

`#include "zeptodb/storage/partition_manager.h"` — Namespace: `zeptodb::storage`

### PartitionManager

```cpp
PartitionManager& pm = pipeline.partition_manager();

// Get or create partition for (symbol, timestamp)
// Creates a new partition if none exists for this (symbol, date_bucket)
Partition& part = pm.get_or_create(symbol_id, timestamp_ns);

// All partitions for a symbol (ordered by time)
std::vector<Partition*> parts = pm.get_partitions(symbol_id);

// Partitions overlapping [from_ns, to_ns] — O(partitions) with O(1) overlap check
std::vector<Partition*> parts = pm.get_partitions_for_time_range(
    symbol_id, from_ns, to_ns
);

// Total partition count (all symbols)
size_t n = pm.partition_count();
```

### Partition

Direct read-only access to column data — zero copy.

```cpp
// Column data pointers (nullptr if column doesn't exist)
const int64_t* prices     = part.get_column("price");
const int64_t* volumes    = part.get_column("volume");
const int64_t* timestamps = part.get_column("timestamp");
size_t         row_count  = part.row_count();

// Partition key
const PartitionKey& key = part.key();
key.symbol_id    // SymbolId (int64)
key.date         // Date bucket (int64, nanoseconds floored to day)

// Time range binary search — O(log n) on sorted timestamp column
// Returns [begin_row, end_row) half-open range
auto [begin_row, end_row] = part.timestamp_range(from_ns, to_ns);

// O(1) overlap check using first/last row timestamps
bool overlaps = part.overlaps_time_range(from_ns, to_ns);
```

---

## TickMessage

`#include "zeptodb/common/types.h"`

```cpp
using SymbolId  = int64_t;
using Timestamp = int64_t;   // nanoseconds since Unix epoch

struct TickMessage {
    SymbolId  symbol_id = 0;
    int64_t   price     = 0;     // scaled integer (e.g. cents: 150.25 → 15025)
    int64_t   volume    = 0;
    Timestamp recv_ts   = 0;     // nanoseconds since epoch
    int64_t   bid       = 0;     // optional
    int64_t   ask       = 0;     // optional
    int64_t   extra[4]  = {};    // user-defined columns
};
```

### Timestamp utilities

```cpp
#include "zeptodb/common/types.h"

// Current nanosecond timestamp
Timestamp ts = now_ns();

// Convert from epoch seconds
Timestamp ts = 1711000000LL * 1'000'000'000LL;

// Convert from epoch milliseconds
Timestamp ts = 1711000000000LL * 1'000'000LL;

// Nanosecond constants
constexpr int64_t NS_PER_US  = 1'000LL;
constexpr int64_t NS_PER_MS  = 1'000'000LL;
constexpr int64_t NS_PER_S   = 1'000'000'000LL;
constexpr int64_t NS_PER_MIN = 60'000'000'000LL;
constexpr int64_t NS_PER_H   = 3'600'000'000'000LL;
constexpr int64_t NS_PER_DAY = 86'400'000'000'000LL;
```

---

## Telegraf Output Helpers

`#include "zeptodb/feeds/telegraf_output.h"` — Namespace:
`zeptodb::feeds`

The Telegraf helpers parse Influx line protocol emitted by Telegraf
`outputs.execd`, map metrics into ZeptoDB tick columns, and build a safe SQL
INSERT batch. The standalone `zepto-telegraf-output` tool uses this API.

```cpp
using namespace zeptodb::feeds;

auto metric = parse_telegraf_line(
    "cpu,symbol=edge01 value=99.5,volume=1i 1711234567000000000");
if (!metric) throw std::runtime_error("bad line protocol");

TelegrafOutputConfig cfg;
cfg.table_name = "telegraf";
cfg.price_scale = 100.0;

std::string error;
auto row = metric_to_telegraf_sql_row(*metric, cfg, &error);
if (!row) throw std::runtime_error(error);

auto batch = build_telegraf_insert_sql({*row}, cfg);
// INSERT INTO telegraf (symbol, price, volume, timestamp) VALUES ...
```

`TelegrafOutputConfig` fields:

| Field | Default | Meaning |
|---|---|---|
| `table_name` | `telegraf` | Destination SQL table; must be `[A-Za-z_][A-Za-z0-9_]*` |
| `symbol_tag` | `symbol` | Tag mapped to ZeptoDB `symbol` |
| `measurement_as_symbol` | `true` | Use measurement name when `symbol_tag` is absent |
| `price_field` | `value` | Numeric field mapped to `price` |
| `volume_field` | `volume` | Numeric field mapped to `volume` |
| `default_volume` | `1` | Volume when `volume_field` is absent |
| `price_scale` | `1.0` | Multiplier before int64 price storage |
| `volume_scale` | `1.0` | Multiplier before int64 volume storage |
| `timestamp_unit` | `Nanoseconds` | Unit for line-protocol timestamps |

---

## AgentMemoryStore

`#include "zeptodb/ai/agent_memory.h"` — Namespace: `zeptodb::ai`

`AgentMemoryStore` is the in-process engine behind the HTTP and Python AI memory
APIs. It is thread-safe; all public methods take or return snapshots. Embeddings
are client-supplied `std::vector<float>` values and must use a single dimension
per store.

```cpp
#include "zeptodb/ai/agent_memory.h"
using namespace zeptodb::ai;

AgentMemoryStore store;

MemoryRecord memory;
memory.tenant_id = "tenant_a";
memory.namespace_id = "agent";
memory.user_id = "u1";
memory.content = "User prefers concise answers.";
memory.embedding = {1.0f, 0.0f};
memory.token_count = 5;
memory.pinned = true;

auto put = store.put_memory(memory);
if (!put.ok) {
    throw std::runtime_error(put.error);
}

MemoryQuery query;
query.tenant_id = "tenant_a";
query.namespace_id = "agent";
query.user_id = "u1";
query.query_embedding = {1.0f, 0.0f};
query.limit = 5;

auto matches = store.search(query);
```

`search()` updates access counters and `last_accessed_ns` for returned memories
by default. Set `query.update_access = false` for read-only diagnostics or
benchmark comparisons that must not perturb later recency/access-count ranking.

### Context assembly

```cpp
ContextRequest request;
request.tenant_id = "tenant_a";
request.namespace_id = "agent";
request.query_embedding = {1.0f, 0.0f};
request.token_budget = 128;
request.limit = 20;

ContextResult context = store.get_context(request);
```

### Exact and semantic cache

```cpp
CacheEntry entry;
entry.tenant_id = "tenant_a";
entry.namespace_id = "agent";
entry.prompt = "Summarize the latest task";
entry.response = "Short task summary";
entry.embedding = {0.9f, 0.1f};
store.store_cache(entry);

CacheLookup lookup;
lookup.tenant_id = "tenant_a";
lookup.namespace_id = "agent";
lookup.prompt = " summarize   THE latest task ";
lookup.embedding = {0.88f, 0.12f};
lookup.semantic_threshold = 0.92;

CacheLookupResult hit = store.lookup_cache(lookup);
```

### Eviction

```cpp
AgentMemoryEvictionConfig eviction;
eviction.max_memories = 100000;
eviction.max_cache_entries = 10000;
eviction.protect_pinned = true;

AgentMemoryTenantQuota tenant_quota;
tenant_quota.tenant_id = "tenant_a";
tenant_quota.namespace_id = "agent";  // empty means all namespaces for tenant_a
tenant_quota.max_memories = 1000;
tenant_quota.max_cache_entries = 500;
eviction.tenant_quotas.push_back(tenant_quota);

store.set_eviction_config(eviction);
store.evict_expired();
```

Capacity eviction removes expired entries first, then evicts the lowest-retention
memory/cache entries. Memory retention combines importance, recency, access count,
and pinned status; pinned memories are protected by default from capacity eviction
but explicit TTL expiry still removes them. Tenant quotas run before global caps
and evict only entries matching the configured tenant and optional namespace.
`put_memory()` and `store_cache()` return `StoreResult.evictions` with
`AgentMemoryEvictionEvent` tombstone keys for automatic TTL, tenant-quota, and
capacity evictions caused by the write. HTTP owners persist those keys as delete
WAL tombstones. Each eviction event also carries a memory/cache snapshot so
owner durability code can call `restore_evicted_entries()` if persistence fails
before the corresponding tombstone is durable.

### ANN acceleration

ANN is optional and only generates semantic candidates. The default exact path
uses filtered top-K scan and parallelizes large full scans. `AgentMemoryStore`
still applies tenant/session/type filters, TTL checks, and the final recency,
importance, pinned, and access-count ranking.

```cpp
AgentMemoryAnnConfig ann;
ann.mode = AgentMemoryAnnMode::Auto;
ann.min_records = 50000;
ann.oversample = 8;
ann.index.max_candidates = 50000;
ann.index.ivf_centroids = 256;
ann.index.ivf_probe = 8;

store.set_ann_config(ann);
store.rebuild_ann_index();  // optional; explicit synchronous rebuild
```

ANN rebuilds take a memory-vector snapshot under the store mutex, build the next
index outside that mutex, and swap it in only if no newer mutation superseded
the snapshot. Search schedules dirty rebuilds on the store's background ANN
worker and falls back to exact scan until a fresh ANN candidate index is
available. Once an ANN index is clean, append-only memory inserts are added to
the index incrementally. Updates that preserve tenant, namespace, and embedding
do not dirty ANN. Updates that change embedding or partition, explicit deletes,
and compacting row-id remaps are maintained incrementally while the index is
clean; if maintenance cannot be applied, the store marks ANN dirty and uses exact
scan until the background replacement is ready. Eviction, snapshot loads, and ANN
config changes still wait for a fresh background index before using ANN again.

`AgentMemoryAnnMode::SparseProjection` forces the ANN path below
`min_records`. `AgentMemoryAnnMode::Hnsw` enables the optional hnswlib backend
when the build was configured with `ZEPTO_ENABLE_HNSWLIB=ON`; HNSW uses
normalized vectors with L2 distance, which is order-equivalent to cosine
similarity. HNSW tuning fields are `ann.index.hnsw_m`,
`ann.index.hnsw_ef_construction`, and `ann.index.hnsw_ef_search`.
`AgentMemoryAnnMode::Ivf` enables the dependency-free inverted-file baseline;
`ann.index.ivf_centroids` controls the per-partition list count and
`ann.index.ivf_probe` controls how many nearest lists each query scans. `Off`
preserves the exact filtered scan. Set
`MemoryQuery::force_scan = true` when comparing ANN results against exact
retrieval, and set `MemoryQuery::update_access = false` when the comparison
should not affect access-count or recency ranking. The v0 sparse-projection index
and optional HNSW/IVF indexes are experimental derived state and are rebuilt from
live memories after snapshot load.

`AgentMemoryStats` reports ANN and sidecar footprint fields in addition to
counts and rebuild/search counters: `ann_memory_bytes`,
`ann_tombstone_entries`, `snapshot_records_bytes`,
`snapshot_vectors_bytes`, and `snapshot_total_bytes`.

### Owner-scoped ids

```cpp
AgentMemoryIdConfig ids;
ids.owner_scoped = true;
ids.node_id = 7;
ids.ring_epoch = 42;
store.set_id_config(ids);

// Next auto ids are mem_7_42_1 and cache_7_42_1.
```

The default remains the legacy local format: `mem_N` and `cache_N`.

### Main methods

| Method | Purpose |
|---|---|
| `put_memory(MemoryRecord)` | Store or update a memory; validates token count and embedding dimension; returns automatic eviction tombstone keys in `StoreResult.evictions` |
| `get_memory(memory_id, tenant_id)` | Return one memory snapshot, optionally tenant-scoped |
| `get_cache(tenant_id, namespace_id, prompt)` | Return one exact cache snapshot without updating access counters |
| `memory_records_snapshot()` | Return all memory records without updating access counters |
| `cache_entries_snapshot()` | Return all cache entries without updating access counters |
| `remove_memory(memory_id, tenant_id)` | Remove one memory by id, optionally tenant-scoped |
| `search(MemoryQuery)` | Filter, rank, and return top-K memories |
| `get_context(ContextRequest)` | Deduplicate and select memories under a token budget |
| `store_cache(CacheEntry)` | Store exact/semantic cache entry; returns automatic eviction tombstone keys in `StoreResult.evictions` |
| `remove_cache(tenant_id, namespace_id, prompt)` | Remove one exact cache entry |
| `lookup_cache(CacheLookup)` | Exact prompt cache lookup with semantic fallback |
| `set_eviction_config(AgentMemoryEvictionConfig)` | Configure global and tenant-scoped memory/cache capacity limits plus pinned protection |
| `eviction_config()` | Return the current eviction policy |
| `set_ann_config(AgentMemoryAnnConfig)` | Configure optional ANN candidate generation |
| `ann_config()` | Return the current ANN policy |
| `rebuild_ann_index()` | Rebuild the derived ANN index from live memory vectors |
| `set_id_config(AgentMemoryIdConfig)` | Configure legacy or owner-scoped automatic id generation |
| `id_config()` | Return the current automatic id identity |
| `evict_expired(now_ns)` | Remove expired memory/cache entries and return the number removed |
| `restore_evicted_entries(evictions)` | Restore automatic-eviction snapshots after failed durability; bypasses eviction enforcement for rollback |
| `save_to_directory(path)` | Write `records.bin` and `vectors.bin` sidecar snapshot files |
| `load_from_directory(path)` | Load a sidecar snapshot atomically into the store |
| `stats()` | Return memory/cache counts, embedding dimension, eviction counters, and ANN counters |

---

## AgentMemoryRouter

`#include "zeptodb/ai/agent_memory_router.h"` — Namespace: `zeptodb::ai`

`AgentMemoryRouter` is the multi-node Agent Memory ownership helper. It is a
thread-safe consistent hash ring over Agent Memory nodes. It only returns an
owner decision; callers still perform the local store call or remote RPC.

```cpp
AgentMemoryRouterConfig cfg;
cfg.self_node_id = 2;
cfg.ring_epoch = 11;
cfg.mode = AgentMemoryRoutingMode::Routed;

AgentMemoryRouter router(cfg);
router.add_node(1);
router.add_node(2);
router.add_node(3);

auto key = AgentMemoryRouter::memory_key(
    "tenant_a", "agent", "session_1", "agent_1", "user_1", "mem_1");
AgentMemoryOwner owner = router.route(key);
```

Default `Local` mode always returns `self_node_id`, even if nodes were added.
`memory_key()` chooses the logical subject in this order: session, agent, user,
then memory id. `cache_key()` uses the normalized prompt hash as the logical
subject so exact prompt cache lookup can route directly to one owner.

### Routed HTTP operations

`HttpServer::set_agent_memory_routing()` wires routed Agent Memory HTTP
operations. The server uses `AgentMemoryRouter` for owner selection and
`TcpRpcClient::request_binary()` to send opaque Agent Memory payloads to remote
owners. The routing config's `ring_epoch` is copied to those clients so remote
writes carry the existing RPC fencing epoch. It returns `false` if shard-local
persistence validation or load fails for the current node. The receiving pod
registers `TcpRpcServer` callbacks with
`HttpServer::handle_agent_memory_put_rpc()`,
`HttpServer::handle_agent_cache_store_rpc()`,
`HttpServer::handle_agent_memory_get_rpc()`,
`HttpServer::handle_agent_memory_search_rpc()`, and
`HttpServer::handle_agent_cache_lookup_rpc()`. Replicas that participate in
`quorum` or `sync` durability also register
`HttpServer::handle_agent_memory_replica_append_rpc()`.

`HttpServer::set_agent_memory_replication_mode()` accepts
`AgentMemoryReplicationMode::Routed`, `Quorum`, or `Sync`. `Routed` is the
default single-owner ACK policy. `Quorum` waits for a majority of configured
Agent Memory nodes across the prepared WAL record and commit marker. `Sync`
waits for all configured Agent Memory nodes before the owner commit marker is
published.

Current routed operations are memory put/delete, cache store/delete, point
memory lookup, exact prompt cache lookup, semantic cache fallback, search, and
context. Search and context fan out through local top-K search on each Agent
Memory node, followed by a coordinator-side global merge. Semantic cache fallback
fans out after exact prompt lookup misses and returns the highest-score hit. The
wire message types are `AGENT_MEMORY_PUT`, `AGENT_MEMORY_DELETE`,
`AGENT_CACHE_STORE`, `AGENT_CACHE_DELETE`, `AGENT_MEMORY_GET`,
`AGENT_MEMORY_SEARCH`, `AGENT_CACHE_LOOKUP_EXACT`, and
`AGENT_MEMORY_REPLICA_APPEND`, with matching result/ack messages.

When Agent Memory persistence is enabled, standalone mode stores snapshots in the
configured directory. Routed mode stores this node's local shard under
`node-{node_id}/shard-0/` and validates that shard's `manifest.json` before
loading it. The HTTP server appends local owner mutations to `wal.log` as
prepared records plus commit markers, replays only committed prepared records
after snapshot load, and truncates the log after a successful snapshot publish.
Explicit memory/cache deletes are persisted as committed tombstones. Failed
persisted writes roll back the live owner store before returning an error,
including write-triggered automatic evictions whose tombstones were not durable.

`HttpServer::adopt_agent_memory_owner_shard(source_node_id, source_ring_epoch)`
is the explicit failover replay primitive. It validates the failed owner's shard
manifest, loads that shard's snapshot, replays its WAL, merges the memory/cache
records into the replacement node's live store, and publishes the replacement
node's current shard snapshot. Owner-scoped ids from a removed node fall back to
current-ring routing for point lookup when the embedded owner id is no longer a
live Agent Memory node.

`HttpServer::handle_agent_memory_owner_failover(source_node_id,
source_ring_epoch, new_ring_epoch, live_nodes)` is the automatic orchestration
hook for failover callbacks. It requires routed mode and `live_nodes` must
include the current server's node id. The method advances the local Agent Memory
ring to `new_ring_epoch`, removes the failed source node, persists the local
shard under the new epoch, and returns `AgentMemoryOwnerFailoverResult`. Only
the deterministic successor adopts the failed owner's shard; other live nodes
return `ok=true` with `adopted=false`. The deterministic successor is the first
live node id greater than `source_node_id`, wrapping to the lowest live node id.
Successful successor replay sets `adopted=true` and `replica_promoted=true`.
When the successor has no replay source, the result still returns `ok=true` but
sets `degraded=true`, `replay_source_missing=true`, and `degraded_reason` so
operators can distinguish data-loss risk from a clean empty failover. The result
also carries `source_node_id`, `replacement_node_id`, `source_ring_epoch`, and
`new_ring_epoch`.

---

## Auth — CancellationToken

`#include "zeptodb/auth/cancellation_token.h"` — Namespace: `zeptodb::auth`

Used to cancel long-running queries from another thread.

```cpp
#include "zeptodb/auth/cancellation_token.h"

zeptodb::auth::CancellationToken token;

// In another thread:
token.cancel();          // signals cancellation
token.is_cancelled();    // bool — check from executor hot loop

// Reset for reuse
token.reset();
```

---

## Quick Build Reference

```bash
mkdir -p build && cd build
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 \
  -DCMAKE_CXX_COMPILER=clang++-19 \
  -DAPEX_USE_PARQUET=OFF \
  -DAPEX_USE_S3=OFF \
  -DAPEX_BUILD_PYTHON=OFF
ninja -j$(nproc)

# Run tests
./tests/zepto_tests
```

---

*See also: [SQL Reference](SQL_REFERENCE.md) · [Python Reference](PYTHON_REFERENCE.md) · [HTTP Reference](HTTP_REFERENCE.md)*

## Table-aware ingest (Stage B — devlog 084)

All ingress entry points that produce a `TickMessage` can now stamp a
destination `table_id`. Resolve the name via the pipeline's `SchemaRegistry`
once, then stamp every message before calling `ingest_tick()`:

```cpp
#include "zeptodb/core/pipeline.h"
#include "zeptodb/ingestion/tick_plant.h"
#include <stdexcept>

zeptodb::core::ZeptoPipeline pipeline;
// ... CREATE TABLE first (via QueryExecutor) ...
const uint16_t tid = pipeline.schema_registry().get_table_id("trades");
if (tid == 0) throw std::invalid_argument("Unknown table: trades");

zeptodb::ingestion::TickMessage msg{};
msg.symbol_id = 1;
msg.price     = 15000;
msg.volume    = 100;
msg.recv_ts   = now_ns();
msg.table_id  = tid;              // Stamp before ingest
pipeline.ingest_tick(msg);
```

### Schema-aware typed row ingest

Connectors that already decoded data into a table schema can bypass
`TickMessage::price`/`volume` and write typed wide rows directly:

```cpp
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"

zeptodb::core::ZeptoPipeline pipeline;
zeptodb::sql::QueryExecutor exec{pipeline};

auto ddl = exec.execute(
    "CREATE TABLE ros_imu ("
    "timestamp TIMESTAMP_NS, recv_ts TIMESTAMP_NS, "
    "robot_id SYMBOL, frame_id SYMBOL, quality INT32, "
    "orientation_w FLOAT64)"
);
if (!ddl.ok()) throw std::runtime_error(ddl.error);

const uint16_t tid = pipeline.schema_registry().get_table_id("ros_imu");
const uint32_t robot = pipeline.symbol_dict().intern("arm-01");
const uint32_t frame = pipeline.symbol_dict().intern("base_link");

zeptodb::core::TypedRowMessage row;
row.table_id = tid;
row.symbol_id = 808;      // table-scoped partition key
row.timestamp = 13'000'000'500LL;
row.columns = {
    zeptodb::core::TypedColumnValue::timestamp("timestamp", 13'000'000'500LL),
    zeptodb::core::TypedColumnValue::timestamp("recv_ts", 13'000'001'000LL),
    zeptodb::core::TypedColumnValue::symbol("robot_id", robot),
    zeptodb::core::TypedColumnValue::symbol("frame_id", frame),
    zeptodb::core::TypedColumnValue::int32("quality", 1),
    zeptodb::core::TypedColumnValue::float64("orientation_w", 0.5),
};

if (!pipeline.ingest_typed_row(std::move(row))) {
    throw std::runtime_error("typed ingest failed");
}
```

`ingest_typed_row()` requires `table_id != 0`, `symbol_id != 0`, and at least
one column. The target table must exist in `SchemaRegistry`; row columns must
exist in that schema with matching types. All declared table columns are
materialized in the partition, and columns omitted from an individual row are
default-filled. `SchemaRegistry::get(uint16_t table_id)` returns a schema copy
by stable table id for connector code that resolves names once.

### Feed handlers

`KafkaConfig::table_name` / `MqttConfig::table_name` /
`KinesisConfig::table_name` resolve the id once inside `set_pipeline()` and
stamp it on every decoded tick automatically:

```cpp
zeptodb::feeds::KafkaConfig cfg;
cfg.topic      = "market_data";
cfg.table_name = "trades";        // empty = legacy path (table_id = 0)
zeptodb::feeds::KafkaConsumer consumer(cfg);
consumer.set_pipeline(&pipeline); // resolves table_name → table_id here
```

Kinesis uses the same decode formats as Kafka and MQTT. Default builds keep the
pure decode/routing path testable; live AWS polling requires
`-DZEPTO_USE_KINESIS=ON` and AWS SDK C++ Kinesis:

```cpp
#include "zeptodb/feeds/kinesis_consumer.h"
#include <cstring>

zeptodb::feeds::KinesisConfig kcfg;
kcfg.region = "us-east-1";
kcfg.stream_name = "market-data";
kcfg.shard_id = "shardId-000000000000";
kcfg.table_name = "trades";
kcfg.format = zeptodb::feeds::MessageFormat::JSON_HUMAN;
kcfg.symbol_map = {{"AAPL", 1}};

zeptodb::feeds::KinesisConsumer kinesis(kcfg);
kinesis.set_pipeline(&pipeline);

// Testable without AWS:
const char* payload = R"({"symbol":"AAPL","price":150.25,"volume":100})";
kinesis.on_record(payload, std::strlen(payload));
```

FIX / ITCH / Binance parser classes expose `set_table_id(tid)` and
`set_table_name(name)` setters that return the configured id to callers
that forward the parser's `Tick` into a `TickMessage`.

### ROS 2 connector

`Ros2Consumer` provides the C++ surface for the ROS 2 / Physical AI bridge.
Config validation, ROS timestamp conversion, scalar sample mapping,
table-aware routing, stats, and Prometheus formatting work without `rclcpp`.
When configured with `-DZEPTO_USE_ROS2=ON` and both `rclcpp` and `std_msgs`
are found, `start()` opens live scalar subscriptions for
`std_msgs/msg/{Float64,Float32,Int64,Int32,UInt64,UInt32}` messages using a
single `data` field. When `sensor_msgs`, `nav_msgs`, `tf2_msgs`, and
`geometry_msgs` are also found, `Ros2IngestMode::StandardProfile` supports
Physical AI standard profiles for `sensor_msgs/msg/Imu`,
`sensor_msgs/msg/JointState`, `nav_msgs/msg/Odometry`,
`tf2_msgs/msg/TFMessage`, and `sensor_msgs/msg/LaserScan`. The same message
set is also supported by `Ros2IngestMode::TypedProfile` for schema-aware wide
tables.

`TypedProfile` rows participate in table-scoped cluster routing. When
`Ros2Consumer::set_routing()` is configured and `PartitionRouter` selects a
remote owner, the row is sent through `RpcClientBase::ingest_typed_row()` /
`TcpRpcClient::ingest_typed_row()` instead of being dropped locally.

Standard profiles flatten configured numeric field paths into the same
`Ros2ScalarSample` / `TickMessage` path as scalar topics. IMU and Odometry
emit one row per configured field. JointState and TF expand arrays by adding
the element index to each field's base `symbol_id`. LaserScan emits configured
metadata and finite numeric summaries; it does not expand raw range arrays in
the hot path.

When `rosbag2_cpp` and `rosbag2_storage` are also available, the same consumer
can import or replay scalar, standard-profile, and typed-profile rosbag2 data
without publishing anything back into the ROS graph. Bag imports use configured
subscriptions as the default topic allowlist, preserve rosbag send timestamps
as source time, and write through the same table-aware `ZeptoPipeline` ingest
route as live subscriptions.

```cpp
#include "zeptodb/feeds/ros2_consumer.h"

zeptodb::feeds::Ros2Config cfg;
zeptodb::feeds::Ros2SubscriptionConfig sub;
sub.topic = "/robot/joint_effort";
sub.message_type = "std_msgs/msg/Float64";
sub.table_name = "ros_joint";
sub.fields.push_back({"data", /*symbol_id=*/1, /*value_scale=*/1000.0});
cfg.subscriptions.push_back(sub);

zeptodb::feeds::Ros2Consumer consumer(cfg);
consumer.set_pipeline(&pipeline);

zeptodb::feeds::Ros2ScalarSample sample;
sample.topic = "/robot/joint_effort";
sample.symbol_id = 1;
sample.value = 42000;
sample.source_ts_ns = 1717000000000000000LL;
sample.recv_ts_ns = 1717000000000000100LL;
consumer.on_scalar_sample(sample);
```

```cpp
zeptodb::feeds::Ros2SubscriptionConfig imu;
imu.topic = "/imu/data";
imu.message_type = "sensor_msgs/msg/Imu";
imu.mode = zeptodb::feeds::Ros2IngestMode::StandardProfile;
imu.table_name = "ros_imu";
imu.fields.push_back({"orientation.w", 100, 1000.0});
imu.fields.push_back({"angular_velocity.z", 101, 1000.0});
imu.fields.push_back({"linear_acceleration.x", 102, 1000.0});

zeptodb::feeds::Ros2SubscriptionConfig scan;
scan.topic = "/scan";
scan.message_type = "sensor_msgs/msg/LaserScan";
scan.mode = zeptodb::feeds::Ros2IngestMode::StandardProfile;
scan.fields.push_back({"ranges.count", 200, 1.0});
scan.fields.push_back({"ranges.mean", 201, 1000.0});
scan.fields.push_back({"range_max", 202, 1000.0});
```

Typed profile subscriptions do not use `fields`. Instead, create a table whose
required columns match `Ros2Consumer::typed_profile_schema(profile)` and set
`typed_partition_symbol_id` to the table-scoped partition key:

```cpp
zeptodb::feeds::Ros2SubscriptionConfig imu_typed;
imu_typed.topic = "/imu/data";
imu_typed.message_type = "sensor_msgs/msg/Imu";
imu_typed.mode = zeptodb::feeds::Ros2IngestMode::TypedProfile;
imu_typed.table_name = "ros_imu_wide";
imu_typed.typed_partition_symbol_id = 808;

auto required = zeptodb::feeds::Ros2Consumer::typed_profile_schema(
    zeptodb::feeds::Ros2StandardProfile::Imu);
```

Every typed profile includes `timestamp TIMESTAMP_NS`, `recv_ts TIMESTAMP_NS`,
`robot_id SYMBOL`, `session_id SYMBOL`, `topic SYMBOL`, `frame_id SYMBOL`, and
`quality INT32`. Profile-specific columns are:

| Profile | Additional columns |
|---|---|
| IMU | `orientation_{x,y,z,w}`, `angular_velocity_{x,y,z}`, `linear_acceleration_{x,y,z}` as `FLOAT64` |
| JointState | `joint_id SYMBOL`, `position FLOAT64`, `velocity FLOAT64`, `effort FLOAT64`; one row per joint |
| Odometry | `child_frame_id SYMBOL`, pose position/orientation columns, twist linear/angular columns |
| TFMessage | `child_frame_id SYMBOL`, `translation_{x,y,z}`, `rotation_{x,y,z,w}`; one row per transform |
| LaserScan | scan metadata, `ranges_count`, `ranges_{min,max,mean}`, `intensities_count`, `intensities_{min,max,mean}` |

Typed rows use the same table-scoped owner routing as scalar rows. If a
configured `PartitionRouter` routes a typed row to a remote owner, the bridge
forwards the `TypedRowMessage` over `TcpRpcClient::ingest_typed_row()` and the
remote node applies it through its local `ZeptoPipeline::ingest_typed_row()`.

Supported standard profile field paths:

| Message type | Field paths |
|---|---|
| `sensor_msgs/msg/Imu` | `orientation.{x,y,z,w}`, `angular_velocity.{x,y,z}`, `linear_acceleration.{x,y,z}` |
| `sensor_msgs/msg/JointState` | `position`, `velocity`, `effort` |
| `nav_msgs/msg/Odometry` | `pose.position.{x,y,z}`, `pose.orientation.{x,y,z,w}`, `twist.linear.{x,y,z}`, `twist.angular.{x,y,z}` |
| `tf2_msgs/msg/TFMessage` | `translation.{x,y,z}`, `rotation.{x,y,z,w}` |
| `sensor_msgs/msg/LaserScan` | `angle_min`, `angle_max`, `angle_increment`, `time_increment`, `scan_time`, `range_min`, `range_max`, `ranges.count`, `ranges.min`, `ranges.max`, `ranges.mean`, `intensities.count`, `intensities.min`, `intensities.max`, `intensities.mean` |

```cpp
zeptodb::feeds::Ros2BagConfig bag;
bag.uri = "/data/robot_run_001";
bag.topics = {"/robot/joint_effort"}; // empty = configured subscriptions
bag.max_messages = 0;                 // 0 = full bag
bag.fail_on_unknown_topic = true;
bag.replay_speed = 4.0;               // replay_bag() only

zeptodb::feeds::Ros2BagStats imported = consumer.import_bag(bag);
if (!imported.completed) {
    throw std::runtime_error(imported.error);
}

zeptodb::feeds::Ros2BagStats replayed = consumer.replay_bag(bag);
```

`Ros2BagStats` reports `messages_read`, `messages_consumed`,
`messages_skipped`, `decode_errors`, `rows_ingested`,
`first_source_ts_ns`, and `last_source_ts_ns`. Without rosbag2 support compiled
in, import/replay fails closed and returns `completed == false` with a
diagnostic `error`.

### OPC-UA Consumer

`OpcUaConsumer` provides the C++ surface for industrial OPC-UA ingestion. The
default build keeps open62541 optional: pure decode/routing helpers compile and
test without a live server, while `-DZEPTO_USE_OPCUA=ON` enables real client
connectivity and the `zepto-opcua-browse` discovery CLI.

```cpp
#include "zeptodb/feeds/opcua_consumer.h"

zeptodb::feeds::OpcUaConfig cfg;
cfg.endpoint = "opc.tcp://plc-gateway:4840";
cfg.nodes.push_back({"ns=2;s=Temperature", 100, 100.0});
cfg.nodes.push_back({"ns=2;s=MotorCurrentArray", 200, 1000.0, 1});

zeptodb::feeds::OpcUaConsumer consumer(cfg);
consumer.set_pipeline(&pipeline);
```

Production-profile hooks share the same quality policy, backpressure retry,
and local/remote routing counters as scalar `on_data_change()`:

```cpp
zeptodb::feeds::OpcUaConsumer::Variant v;
v.type = zeptodb::feeds::OpcUaConsumer::VariantType::Double;
v.f64 = 12.34;

consumer.on_array_change("ns=2;s=MotorCurrentArray", {v}, source_ts_ns);
consumer.on_string_change("ns=2;s=MachineState", "RUNNING", source_ts_ns);

zeptodb::feeds::OpcUaConsumer::StructuredField pressure;
pressure.field_name = "pressure";
pressure.symbol_id = 301;
pressure.value = v;
pressure.value_scale = 100.0;
pressure.engineering_unit = "bar";
consumer.on_structured_change({pressure}, source_ts_ns);
```

Historical backfills and Alarms & Conditions use explicit replay/event hooks:

```cpp
zeptodb::feeds::OpcUaConsumer::HistoricalSample sample;
sample.node_id = "ns=2;s=Temperature";
sample.value = v;
sample.source_ts_ns = source_ts_ns;
consumer.ingest_history({sample});

zeptodb::feeds::OpcUaConsumer::AlarmEvent alarm;
alarm.symbol_id = 9001;
alarm.severity = 750;
alarm.active = true;
alarm.source_ts_ns = source_ts_ns;
consumer.on_alarm_event(alarm);
```

Live Historical Access reads use the same routing path when open62541 was built
with historizing support:

```cpp
zeptodb::feeds::OpcUaConsumer::HistoryReadOptions history;
history.start_ts_ns = start_ns;
history.end_ts_ns = end_ns;
history.values_per_node = 1000;  // 0 lets the server choose
history.return_bounds = true;

size_t routed = consumer.read_history(history);
```

For browse-based config discovery:

```bash
./build/zepto-opcua-browse \
  --endpoint opc.tcp://plc-gateway:4840 \
  --root ns=0;i=85 \
  --max-depth 3 \
  --symbol-base 1000 \
  --config
```

`OpcUaServer` exposes configured ZeptoDB symbols as OPC-UA Int64 variable nodes
for PLC/SCADA clients. Default builds keep the snapshot/publish contract
available for tests but `start()` returns false until open62541 is enabled.

```cpp
zeptodb::feeds::OpcUaServerConfig scfg;
scfg.endpoint = "opc.tcp://0.0.0.0:4840";
scfg.nodes.push_back({"ns=1;s=temperature", 100, "temperature", 0});

zeptodb::feeds::OpcUaServer server(scfg);
server.start();
server.publish_value(100, 2375, source_ts_ns);
server.stop();
```

### Migration tools

All four migrator configs (`ClickHouseMigrator::Config`,
`DuckDBIntegrator::Config`, `TimescaleDBMigrator::Config`) expose a
`dest_table` field. `HDBLoader::set_dest_table(name)` sets the destination
on the loader. The `zepto-migrate` CLI wires them via `--dest-table <name>`.

---

## Cluster routing (Stage C — devlog 085)

`ClusterNode::ingest_tick(TickMessage msg)` now honors `msg.table_id` when
selecting the owning node — it calls `route(msg.table_id, msg.symbol_id)`
instead of `route(msg.symbol_id)`. Two tables that both use `symbol_id = 1`
can therefore live on different nodes. The migration dual-write path
(`PartitionRouter::migration_target(symbol_id)`) keeps its single-symbol
semantics for now.

Typed rows use the same owner selection. `TcpRpcClient::ingest_typed_row()`
serializes `zeptodb::core::TypedRowMessage` over `TYPED_ROW_INGEST` and the
remote server applies it through the local pipeline's
`ZeptoPipeline::ingest_typed_row()`.

A table-aware routing accessor is available for callers that build their
own ingest path:

```cpp
zeptodb::cluster::ClusterNode<T> node(cfg);
const uint16_t tid = node.pipeline().schema_registry().get_table_id("trades");

// Resolve the owner for (table, symbol)
NodeId owner = node.route(tid, /*symbol_id=*/1);

// Backward compat: route(sym) is equivalent to route(0, sym)
NodeId legacy_owner = node.route(1);  // == node.route(0, 1)
```

Each data node resolves `FROM <table>` in scattered SQL via its own
`SchemaRegistry`, which Stage A made durable at `{hdb_base}/_schema.json`.
If every node loads the same file (e.g. a shared mount or an operator-driven
seed), the scatter-gather SELECT path is automatically table-aware — no
additional RPC fields required.

*See also: [Devlog 084 — Stage B](../devlog/084_stage_b_ingest_paths.md), [Devlog 085 — Stage C](../devlog/085_stage_c_cluster_and_sql_strict.md)*
