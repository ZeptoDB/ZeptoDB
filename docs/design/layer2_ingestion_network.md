# Layer 2: Ingestion & Network Layer (Tick Plant)

This document is the detailed design specification for the **ultra-low latency routing backbone (Tick Plant)** layer, where network packets from external exchanges are received and distributed to the DB system.

## 1. Architecture Diagram

```mermaid
flowchart LR
    Exchange["Market Feed\n(FIX / Pcap)"] -- "Kernel Bypass NIC\n(DPDK / Solarflare)" --> TP["Ticker Plant\n(Zero-copy Message Router)"]

    subgraph MPMC["Lock-free MPMC Queue (Ring Buffer)"]
        TP -- "Pub" --> RingBuffer((Fast Ring Buffer))
    end

    RingBuffer -- "Sub" --> RDB["RDB\nIn-Memory Storage"]
    RingBuffer -- "Sub" --> CEP["Complex Event Processor\n(real-time fraud detection)"]
    TP -- "Sequential Write" --> WAL["Write-Ahead Log\n(replay log for failure recovery)"]
```

## 2. Tech Stack
- **Network packet processing:** DPDK, **UCX (Unified Communication X)** framework (open-source communication standard supporting RoCE v2, InfiniBand, AWS SRD without cloud vendor lock-in).
- **Queue and lock management:** C++20 Atomics (`std::memory_order_relaxed`), Rust (for memory safety in packet parsing).
- **Routing algorithm:** Multi-Producer Multi-Consumer (MPMC) Ring Buffer, Disruptor architecture-based event loop.

## 3. Layer Requirements
1. **Network OS Stack Bypass:** Discard the Linux TCP/IP stack and fetch data from the NIC (Network Interface Card) directly to software user-space buffers via polling, eliminating interrupt latency.
2. **Multi-Subscriber Broadcasting:** With a single packet reception, push data to multiple subscribers — storage (RDB), logging (WAL), pattern analysis (CEP) — in a zero-copy manner.
3. **Ordering Guarantee:** Every financial tick must be guaranteed strict microsecond-precision FIFO ordering and global timestamp stamping upon receipt.

## 4. Detailed Design
- **Ring Buffer-based Lock-Free Queue design:** Producer threads (Ticker Plant receive side) pre-claim the next write position on the Ring buffer via Atomic Fetch-and-Add. To fundamentally block Thread Context-switch and Mutex Lock, threads are fully pinned to a single CPU core (CPU Pinning) with spinlock or infinite polling loop.
- **Async separation of storage and parsing:** When data arrives, the Ticker Plant immediately writes to WAL in the most primitive form to ensure durability. It then immediately throws it into the RDB queue for async columnar format normalization and format assignment.

## 5. Apache Kafka Consumer

**Status:** Implemented (2026-03-23) — `include/zeptodb/feeds/kafka_consumer.h`, `src/feeds/kafka_consumer.cpp`

### Purpose

Connects enterprise Kafka data pipelines to ZeptoDB ingestion. Kafka is the dominant message bus in fintech, adtech, and e-commerce real-time systems. This integration allows any Kafka producer (market data feeds, event buses, IoT gateways) to stream ticks directly into ZeptoDB without an intermediate adapter.

### Architecture

```
Kafka Topic (librdkafka poll)
        ↓
KafkaConsumer::on_message(data, len)
        ↓
decode_json / decode_binary / decode_json_human
        ↓
ingest_decoded(TickMessage)
        ↓
  ┌─ router_ set?  ──→  PartitionRouter::route(symbol_id)
  │                        ├─ local  → ZeptoPipeline::ingest_tick()
  │                        └─ remote → TcpRpcClient::ingest_tick()
  └─ no router     ──→  ZeptoPipeline::ingest_tick()  (single-node)
```

### Wire Formats

| Format | Description | Use case |
|--------|-------------|----------|
| `JSON` | `{"symbol_id":1,"price":15000,"volume":100,"ts":...}` | Dev / testing |
| `BINARY` | Raw `TickMessage` bytes (64 bytes, little-endian) | HFT production path |
| `JSON_HUMAN` | `{"symbol":"AAPL","price":150.25,"volume":100}` | General data engineering |

### Routing

- **Single-node:** `set_pipeline(pipeline*)` — all ticks dispatched locally.
- **Multi-node:** `set_routing(local_id, router, remotes)` — uses the same `PartitionRouter` consistent-hash ring as the TCP RPC cluster. Remote ticks forwarded via existing `TcpRpcClient::ingest_tick()`.

### Compile-time Toggle

```cmake
# Enable (requires librdkafka-devel)
cmake -DAPEX_USE_KAFKA=ON ..

# Disabled (default) — decode/routing logic still compiled, start() returns false
cmake -DAPEX_USE_KAFKA=OFF ..
```

Install dependency: `sudo dnf install -y librdkafka-devel`

### Key Classes

| Symbol | File |
|--------|------|
| `zeptodb::feeds::KafkaConfig` | `include/zeptodb/feeds/kafka_consumer.h` |
| `zeptodb::feeds::KafkaConsumer` | `include/zeptodb/feeds/kafka_consumer.h` |
| `zeptodb::feeds::KafkaStats` | `include/zeptodb/feeds/kafka_consumer.h` |

### Prometheus Metrics Exposure

`KafkaStats` is exposed to Prometheus via two cooperating APIs:

```cpp
// 1. Format KafkaStats as Prometheus/OpenMetrics text
std::string metrics_text =
    zeptodb::feeds::KafkaConsumer::format_prometheus("market-data", consumer.stats());
// → zepto_kafka_messages_consumed_total{consumer="market-data"} 1234
//   zepto_kafka_bytes_consumed_total{consumer="market-data"} 79016
//   zepto_kafka_decode_errors_total{consumer="market-data"} 0
//   zepto_kafka_route_local_total{consumer="market-data"} 1000
//   zepto_kafka_route_remote_total{consumer="market-data"} 234
//   zepto_kafka_ingest_failures_total{consumer="market-data"} 0

// 2. Register with HttpServer to extend GET /metrics
server.add_metrics_provider([&consumer]() {
    return zeptodb::feeds::KafkaConsumer::format_prometheus("market-data", consumer.stats());
});
// Multiple consumers: call add_metrics_provider() once per consumer with distinct names.
```

`HttpServer::add_metrics_provider()` is a generic extension point — it accepts any `std::function<std::string()>` and appends its output to the existing pipeline stats in `/metrics`. There is no compile-time dependency between `zepto_server` and `zepto_kafka`.

**Metric names and types:**

| Metric | Type | Description |
|--------|------|-------------|
| `zepto_kafka_messages_consumed_total` | counter | Messages successfully decoded and dispatched |
| `zepto_kafka_bytes_consumed_total` | counter | Total payload bytes received |
| `zepto_kafka_decode_errors_total` | counter | Messages that failed to decode |
| `zepto_kafka_route_local_total` | counter | Ticks sent to local pipeline |
| `zepto_kafka_route_remote_total` | counter | Ticks forwarded via RPC |
| `zepto_kafka_ingest_failures_total` | counter | Drops after all backpressure retries |

### Tests

26 unit tests in `tests/unit/test_kafka.cpp` — all pass without a live Kafka broker:
- Config defaults
- `decode_json` (basic, ts-optional, missing fields, invalid, null)
- `decode_binary` (exact, too-short, null)
- `decode_json_human` (known symbol, unknown symbol, missing price)
- `ingest_decoded` (no pipeline, single-node)
- `on_message` stats tracking (valid message, decode error)
- `format_prometheus` (counters, HELP/TYPE lines, zero stats, label values)
- `start()` graceful return when library not compiled in

4 integration tests in `tests/unit/test_features.cpp` (`MetricsProviderTest`):
- `DefaultMetricsContainApexCounters` — baseline /metrics output
- `RegisteredProviderAppearsInOutput` — custom provider text in /metrics
- `MultipleProvidersAllAppear` — two providers both present
- `KafkaStatsProviderIntegration` — live KafkaConsumer stats via format_prometheus

### Redpanda / WarpStream Compatibility

`KafkaConsumer` uses the standard Kafka consumer API (group ID, `subscribe()`, `consume()`). Any Kafka-API-compatible broker (Redpanda, WarpStream, MSK) works without code changes.

Last updated: 2026-03-23 (Kafka Prometheus metrics exposure added)

## Table-aware ingest (Stage B — devlog 084)

Every ingress surface now accepts an optional destination table name. The
table_id is resolved once (at `set_pipeline()` or on the first call) via
`SchemaRegistry::get_table_id(name)` and stamped onto each produced
`TickMessage::table_id`.

| Surface | Config / call | Resolution | Unknown name |
|---|---|---|---|
| Python `Pipeline.ingest_batch` | `table_name=...` kwarg | per-call lookup | `ValueError` |
| Python `Pipeline.ingest_float_batch` | `table_name=...` kwarg | per-call lookup | `ValueError` |
| `zepto_py.from_pandas / from_polars / from_arrow` | `table_name=...` kwarg | threaded through to C++ | `ValueError` |
| `KafkaConfig::table_name` | set at construction | cached at `set_pipeline()` | log ERROR, increment `ingest_failures`, drop message |
| `MqttConfig::table_name` | set at construction | cached at `set_pipeline()` | same as Kafka |
| `FIXParser::set_table_id/name` | manual | write-only setter | caller responsibility |
| `NASDAQITCHParser::set_table_id/name` | manual | write-only setter | caller responsibility |
| `BinanceFeedHandler::set_table_id/name` | manual | write-only setter | caller responsibility |

Default `table_name = ""` (or `None` in Python) preserves the legacy path
(`msg.table_id = 0`). WAL serialization is unchanged — `TickMessage` is
64 bytes and already includes `table_id`; pre-devlog-082 WAL files replay
with `table_id = 0`.

### `TickMessage` wire contract and WAL forward-compat

`TickMessage` is a cache-aligned 64-byte struct. Since devlog 082 the
second byte pair of the struct holds `uint16_t table_id`; the same slot
was zero-padding in pre-082 builds. Both of these facts matter for
**rolling upgrades** and **WAL replay**:

* **Cluster wire format.** `remote_ingest` / dual-write / replica forwarding
  send the full 64-byte `TickMessage` verbatim, so `table_id` is carried
  end-to-end without any RPC version bump. Stage C's
  `ClusterNode::ingest_tick` uses `route(msg.table_id, msg.symbol_id)` so
  the destination node is picked from the table-scoped hash.
* **WAL replay.** A v1 WAL file written by a pre-082 node replays cleanly
  on an 082+ node: the `table_id` slot is zero in the on-disk bytes, so
  every replayed tick lands in the legacy `table_id = 0` pool — exactly
  the "unassigned table" bucket operators expect during a rolling upgrade.
  No format bump, no shim, no reader fork. New WAL records written by an
  082+ node carry the real `table_id` and replay into the matching table
  on restart.

### `Quote` / `Order` structs — future `table_id` path (devlog 088 note)

Only `feeds::Tick` carries a `table_id` field today. The sibling `Quote` and
`Order` structs (see `include/zeptodb/feeds/tick.h`) do **not** — they are
not yet routable into ZeptoDB tables: the ingress pipeline consumes Ticks
only, while Quotes/Orders stay inside the feed handler for client
distribution. When they become routable (see the order-book and quote
distribution items in `BACKLOG.md`, tentative tags P9 / P10), the exact
same pattern as `Tick.table_id` must be applied: add `uint16_t table_id = 0`
to the struct, stamp it from `parser_.table_id()` immediately before the
corresponding `quote_callback_` / `order_callback_` dispatch, and extend
the handler setters to cascade the resolved id. No other surface changes
are expected (the structs are not on any current wire protocol or WAL
format).

Last updated: 2026-04-18 (Stage C — devlog 085)
