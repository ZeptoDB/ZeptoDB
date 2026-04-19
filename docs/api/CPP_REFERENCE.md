# ZeptoDB C++ API Reference

*Last updated: 2026-03-22*

---

## Table of Contents

- [ZeptoPipeline](#zeptopipeline)
- [QueryExecutor (SQL)](#queryexecutor-sql)
- [PartitionManager & Partition](#partitionmanager--partition)
- [TickMessage](#tickmessage)
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

### Feed handlers

`KafkaConfig::table_name` / `MqttConfig::table_name` resolve the id once
inside `set_pipeline()` and stamp it on every decoded tick automatically:

```cpp
zeptodb::feeds::KafkaConfig cfg;
cfg.topic      = "market_data";
cfg.table_name = "trades";        // empty = legacy path (table_id = 0)
zeptodb::feeds::KafkaConsumer consumer(cfg);
consumer.set_pipeline(&pipeline); // resolves table_name → table_id here
```

FIX / ITCH / Binance parser classes expose `set_table_id(tid)` and
`set_table_name(name)` setters that return the configured id to callers
that forward the parser's `Tick` into a `TickMessage`.

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
