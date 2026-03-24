# ZeptoDB Multi-Node: Industry Use Case Analysis

**Last updated:** 2026-03-22
**Status:** Design / Planning
**Related:** `docs/design/phase_c_distributed.md`, `docs/design/kafka_consumer_design.md`

---

## Overview

ZeptoDB targets multiple industries with significantly different requirements. This document captures the use-case analysis for each vertical and derives design principles that allow a single distributed architecture to serve all of them — with HFT as the primary focus.

---

## 1. Use Case Comparison

| Requirement | HFT | Quant Finance | AdTech | IoT/Sensor |
|---|---|---|---|---|
| Query latency target | < 1ms | < 10s (batch OK) | < 100ms | < 1s |
| Ingestion rate | 1–10M ticks/s | 100K–1M rows/s | 10M events/s | 1K–100K sensors/s |
| Cardinality (symbols/keys) | Low (1K–10K) | Medium (10K–100K) | Very high (millions of campaigns) | Medium-high (device IDs) |
| Time series type | Market ticks | OHLCV, factor data | Impression/click events | Sensor readings |
| Hot path query type | ASOF JOIN, VWAP | Backtest aggregation | COUNT DISTINCT, GROUP BY | Rolling average, anomaly |
| Clock precision required | PTP / ns-level | ms-level OK | ms-level OK | s-level OK |
| Retention | Hours to days (hot) | 10+ years | Days to weeks | Months to years |
| Cross-node JOIN needed | Critical (ASOF) | Yes (multi-factor) | Rare | Rare |
| Replication factor | 2–3 (failover) | 3 (consistency) | 3 (durability) | 2 (edge+cloud) |
| Typical query complexity | Simple + ASOF JOIN | Complex (CTEs, windows) | Medium + high cardinality | Simple aggregations |
| Topology preference | Symbol affinity shard | Hash-based partition | Hash-based partition | Hierarchical edge+cloud |

---

## 2. Industry Deep Dives

### 2.1 High-Frequency Trading (HFT) — Primary Target

**Profile:**
- Ultra-low latency is the only metric that matters
- 1K–10K active symbols, but 80% of volume concentrated in top 100
- Market data arrives in bursts: 10M ticks/s at open/close
- Queries: ASOF JOIN (quote/trade matching), VWAP, real-time P&L

**Key Requirements:**
- **Symbol affinity partitioning**: Each symbol lives on exactly one data node. ASOF JOIN on the same node = 0 network hops.
- **PTP/IEEE 1588 clock sync**: ASOF JOIN correctness requires sub-microsecond clock agreement across nodes. NTP (~1ms) is not sufficient.
- **Co-location awareness**: Data nodes should be physically co-located with exchange feeds (CME, NYSE, NASDAQ). Network latency from the data center to the exchange matters.
- **Hot symbol problem**: AAPL/SPY/QQQ receive 10× the ticks of typical symbols. A naive hash partition overloads one node. Solution: explicit hot-symbol rebalancing or dedicated "hot" nodes.
- **Read-heavy, write-heavy simultaneously**: Ingestion and query must not block each other. Lock-free ring buffers (existing `TickPlant`) solve this.
- **Query routing tier A**: Single-symbol queries must bypass the coordinator entirely and go directly to the data node.

**Topology: Symbol-Affinity Shard**
```
Exchange Feed
     |
[Ingestion Gateway]
     |
  [Router]  -- symbol hash --> [Node A: AAPL, MSFT, ...]
             -- symbol hash --> [Node B: GOOG, AMZN, ...]
             -- symbol hash --> [Node C: TSLA, META, ...]
```

**Open Problems:**
- Cross-node ASOF JOIN (trade on Node A, quote on Node B) requires a coordinator scatter-gather step — adds ~1ms latency. Mitigation: replicate quote data to all nodes (quote tables are smaller than trade tables).
- Rebalancing after node addition without downtime: consistent hashing with virtual nodes.

---

### 2.2 Quantitative Finance (Backtesting / Research)

**Profile:**
- Batch workloads: backtesting a strategy over 10 years of tick data
- Complex queries: CTEs, window functions, multi-factor joins
- Jupyter notebooks as the primary client interface
- Latency tolerance is higher (seconds to minutes), but throughput matters

**Key Requirements:**
- **Large historical storage**: 10+ years of tick data per symbol. Hot tier (last 30 days) in ZeptoDB memory; cold tier in Parquet/S3.
- **Complex SQL**: Full CTE, subquery, window function, UNION support (all implemented in Phase 3).
- **Python/Polars zero-copy**: Results returned as Arrow arrays for seamless Jupyter integration.
- **Parallel backtest execution**: Multiple backtest jobs run concurrently. Node-level parallelism required.
- **Reproducibility**: Queries on historical data must be deterministic. No partial reads during backtest.

**Topology: Hash-Based Partition**
```
[Client: Jupyter / Python SDK]
        |
  [Coordinator Node]
    /    |    \
[Node A][Node B][Node C]
  (each holds 1/N of symbol-time partitions)
        |
  [Cold Tier: DuckDB + Parquet on S3]
```

**Differentiator from HFT:**
- Cross-node joins are expected and acceptable (seconds latency is fine).
- Coordinator-side merge of partial aggregation results is the normal path.
- Schema evolution (adding new factor columns) must be supported.

---

### 2.3 AdTech (Real-Time Bidding Analytics)

**Profile:**
- Extremely high cardinality: millions of campaign IDs, advertiser IDs, creative IDs
- High write throughput: 10M impression/click events per second across the fleet
- Query pattern: GROUP BY campaign_id, COUNT DISTINCT user_id, CTR computation
- Latency target: < 100ms for bid decision support queries

**Key Requirements:**
- **High-cardinality GROUP BY**: Millions of unique keys. Hash-based partitioning by campaign_id is natural.
- **COUNT DISTINCT**: Requires HyperLogLog or exact bitmap (Roaring Bitmap) across nodes. Naive exact COUNT DISTINCT does not scale.
- **Throughput over latency**: 100ms is acceptable. Throughput of 10M events/s per cluster is the hard requirement.
- **Short retention**: Most queries are last-1-hour or last-1-day. Data older than 30 days moves to cold tier automatically.
- **Multi-tenant isolation**: Different advertisers must not see each other's data. Row-level or partition-level access control needed.

**Topology: Hash-Based Partition (same as Quant but optimized for writes)**
```
[Kafka: impression events] --> [Ingestion Fleet]
                                      |
                           [Hash by campaign_id]
                              /    |    \
                          [Node A][Node B][Node C]
                                      |
                           [Coordinator: merge partial agg]
                                      |
                              [BI Dashboard / API]
```

**Key Challenge:**
- COUNT DISTINCT across nodes: Partial sketches (HyperLogLog) must be mergeable. ZeptoDB does not yet implement HLL — this is a future requirement.
- Hot campaign problem mirrors HFT hot symbol problem.

---

### 2.4 IoT / Industrial Sensor Data

**Profile:**
- Sensors at the edge (factory floor, vehicles, infrastructure)
- Data volumes vary enormously: 1 sensor per device up to 10K channels per industrial machine
- Long retention: years of data for predictive maintenance
- Query pattern: rolling average, threshold alerts, anomaly detection
- Often intermittent connectivity (edge sites may go offline)

**Key Requirements:**
- **Hierarchical topology**: Edge node (local ZeptoDB instance) aggregates locally, syncs to cloud ZeptoDB cluster. Queries can target edge (real-time) or cloud (historical).
- **Offline resilience**: Edge nodes must buffer locally when cloud connectivity is unavailable, then replay.
- **Long retention + tiered storage**: 1 year hot in memory, 10 years cold in Parquet. Automatic tier migration.
- **Simple query patterns**: Mostly time-range scans with simple aggregations. Full SQL complexity not needed at edge.
- **Schema flexibility**: Different sensor types have different column sets. Per-device schema or sparse columns.

**Topology: Hierarchical Edge + Cloud**
```
[Sensor A] --> [Edge Node 1 (plant)] --> [Regional Cloud Node]
[Sensor B] -->                                    |
[Sensor C] --> [Edge Node 2 (plant)] --> [Regional Cloud Node]
                                                  |
                                         [Global Coordinator]
                                                  |
                                         [Cold Tier: S3/Parquet]
```

**Key Challenge:**
- Clock sync at edge sites: PTP is not available everywhere. GPS-disciplined NTP is the practical solution. Sub-ms precision achievable but not guaranteed.
- Edge node sizing: Raspberry Pi / industrial PC with 8–64GB RAM. ZeptoDB must run with configurable memory limits.

---

## 3. Conflicting Design Points

Three fundamental conflicts arise when targeting all verticals simultaneously:

### Conflict 1: Partitioning Strategy

| Requirement | Optimal Strategy |
|---|---|
| HFT (ASOF JOIN on same node) | Symbol-affinity shard |
| Quant / AdTech (uniform load, cross-symbol) | Hash-based uniform partition |
| IoT (edge-local, hierarchical) | Geographic/site-based partition |

**Resolution**: Pluggable partition strategy per namespace/table. HFT tables use `symbol_affinity`. General tables use `hash_mod`. IoT tables use `site_id`. The coordinator applies the correct routing strategy based on table metadata.

### Conflict 2: Clock Synchronization Level

| Requirement | Needed Precision |
|---|---|
| HFT (ASOF JOIN correctness) | < 1 μs (PTP required) |
| Quant Finance (ordering) | < 1 ms (NTP sufficient) |
| AdTech (event ordering) | < 10 ms (NTP sufficient) |
| IoT edge (local only) | < 100 ms acceptable |

**Resolution**: PTP sync is a deployment requirement for HFT clusters. The software layer must expose a configurable `clock_source` setting: `ptp`, `ntp`, `local`. ASOF JOIN raises an error if clock source is not `ptp` and strict mode is enabled.

### Conflict 3: Query Routing Tier

| Query Type | Best Routing |
|---|---|
| Single-symbol, real-time (HFT) | Direct to data node (no coordinator) |
| Cross-symbol aggregation (Quant/AdTech) | Scatter-gather via coordinator |
| Historical cold data | Offload to DuckDB/Parquet query engine |

**Resolution**: Three-tier routing (already described in distributed design):
- **Tier A**: Single-symbol, recent time range → direct routing, < 1ms
- **Tier B**: Cross-node, aggregation → coordinator scatter-gather, < 100ms
- **Tier C**: Historical cold data → DuckDB engine, seconds acceptable

---

## 4. Topology Architectures

### 4.1 Symbol-Affinity (HFT)

```
                    +-----------+
  Exchange feeds -> | Ingestion |
                    |  Gateway  |
                    +-----+-----+
                          | route by symbol
          +---------------+---------------+
          |               |               |
    +-----+-----+   +-----+-----+   +-----+-----+
    |  Node A   |   |  Node B   |   |  Node C   |
    | AAPL MSFT |   | GOOG AMZN |   | TSLA META |
    |  (hot)    |   |           |   |           |
    +-----------+   +-----------+   +-----------+
          |               |               |
          +---------------+---------------+
                          |
                   +------+------+
                   | Coordinator |  (only for cross-symbol queries)
                   +-------------+
```

- Coordinator is on the **cold path** only
- Hot symbols can be explicitly placed on dedicated hardware (e.g., Node A = high-memory server)
- Replication: each partition has one replica on a different node

### 4.2 Hash-Based Partition (Quant / AdTech)

```
  Data sources -> [Ingestion Fleet] -> hash(symbol or campaign_id) mod N
                                              |
                              +---------------+---------------+
                              |               |               |
                        [Node A 1/3]    [Node B 1/3]    [Node C 1/3]
                              |               |               |
                              +-------+-------+               |
                                      |                       |
                               [Coordinator: merge partial aggregates]
                                      |
                               [Client response]
```

- Coordinator is on the **hot path** — must be efficient
- Partial aggregation: each node sends `(sum, count)` not full result set
- Coordinator merges and applies ORDER BY / LIMIT

### 4.3 Hierarchical Edge + Cloud (IoT)

```
  [Edge Site 1]           [Edge Site 2]
  +----------+            +----------+
  | Local DB |            | Local DB |  <- 8-64GB RAM, simple queries
  +----+-----+            +----+-----+
       | sync (async)          | sync (async)
       +----------++-----------+
                  |
         +--------+--------+
         | Regional Cloud  |  <- full ZeptoDB cluster
         +--------+--------+
                  |
         +--------+--------+
         | Cold Tier: S3   |  <- DuckDB query engine
         +-----------------+
```

- Edge node runs a **single-node ZeptoDB** with reduced memory limits
- Sync protocol: streaming replication of WAL segments, deduplication on reconnect
- Edge can serve queries independently when cloud is unreachable (degraded mode)

---

## 5. Design Principles for Multi-Usecase Support

1. **HFT-first, others by configuration**: Default deployment is HFT (symbol affinity, PTP clock, direct routing). Other use cases are enabled via config, not code forks.

2. **Pluggable partition strategy**: `PartitionStrategy` interface with `symbol_affinity`, `hash_mod`, `site_id` implementations. Selected per table in metadata.

3. **Partial aggregation is non-negotiable**: Every aggregate function (`SUM`, `AVG`, `VWAP`, `COUNT`, `MIN`, `MAX`) must have a mergeable intermediate representation. This enables coordinator-side merge without shipping raw rows.

4. **Three-tier query routing**: Tier A (direct), Tier B (scatter-gather), Tier C (cold DuckDB). Routing decision made at coordinator based on query type and time range.

5. **Clock source is a deployment contract**: The system does not abstract away clock sync. HFT deployments must provision PTP. Non-HFT deployments use NTP. ASOF JOIN in strict mode rejects queries on NTP clusters.

6. **Edge is first-class**: Edge nodes run the same ZeptoDB binary with a `--mode edge` flag. This disables multi-node coordinator features and enables local-only operation + sync protocol.

---

## 6. Implementation Phases

### Phase 1: HFT MVP (Current Priority)
- [ ] Single-cluster, symbol-affinity partitioning
- [ ] Scatter-gather coordinator (Tier B queries)
- [ ] Direct routing (Tier A queries)
- [ ] Partial aggregation protocol (FlatBuffers serialized intermediate state)
- [ ] Clock sync integration (PTP detection, error on ASOF JOIN if NTP)

### Phase 2: Production HFT
- [ ] Hot-symbol detection and rebalancing
- [ ] Cross-node ASOF JOIN (quote table replication strategy)
- [ ] Consistent hashing for live node add/remove
- [ ] Replication factor 2 with automatic failover

### Phase 3: Multi-Usecase
- [ ] Hash-based partition mode (Quant / AdTech)
- [ ] HyperLogLog for COUNT DISTINCT across nodes (AdTech)
- [ ] Edge node mode + async sync protocol (IoT)
- [ ] Cold tier offload to DuckDB/Parquet
- [ ] Multi-tenant access control at partition level

---

## 7. Open Questions

1. **Cross-node ASOF JOIN**: Replicating quote tables solves HFT, but doubles storage. Is selective replication (top 100 symbols replicated, rest single-copy) acceptable?

2. **HyperLogLog vs exact COUNT DISTINCT**: HLL gives ~1% error. For AdTech billing, exact counts may be legally required. Plan: exact for small cardinality (< 1M unique), HLL for large.

3. **Edge sync conflict resolution**: If the same device sends data to two edge nodes (failover scenario), deduplication by `(device_id, timestamp)` primary key? Or last-write-wins?

4. **Coordinator SPOF**: A single coordinator is a single point of failure. Raft-based coordinator cluster (3 nodes) or active-passive failover?

5. **Schema evolution across nodes**: Adding a column to a table requires coordinated schema migration. Rolling upgrade strategy: new nodes accept both old and new schema, coordinator merges results with NULL fill for missing columns.

---

*Related documents:*
- `docs/design/phase_c_distributed.md` — Distributed cluster technical design
- `docs/design/kafka_consumer_design.md` — Kafka ingestion design
- `docs/design/architecture_design.md` — Overall architecture
