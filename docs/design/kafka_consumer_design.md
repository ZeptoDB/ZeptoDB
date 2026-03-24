# ZeptoDB Kafka Consumer — Design Document

*Status: Design (not yet implemented)*
*Last updated: 2026-03-22*

---

## 1. Overview

The Kafka Consumer connects ZeptoDB to the enterprise data streaming ecosystem.
It bridges Apache Kafka topics to the existing TickPlant ingestion pipeline,
enabling real-time analytics on any data source that publishes to Kafka.

**Target use cases:**
- Fintech: FIX/market data → Kafka → ZeptoDB real-time analytics
- AdTech: bid-stream events → real-time aggregation
- IoT/sensor: time-series telemetry → ZeptoDB columnar storage
- Log analytics: structured event streams → SQL query interface

---

## 2. Data Flow

```
Kafka Broker (topic: "trades-AAPL", "trades-GOOG", ...)
        │
        │  librdkafka poll (batch, zero-copy payload reference)
        ▼
┌─────────────────────────────────────────┐
│            KafkaConsumer                │
│                                         │
│  consume_loop() — dedicated thread      │
│                                         │
│  ① Backpressure check                  │  queue_depth() > threshold → sleep 1ms
│  ② Batch poll                          │  rd_kafka_consume_batch (1000 msg/poll)
│  ③ MessageDecoder::decode()            │  raw bytes → TickMessage
│  ④ KafkaSymbolMapper::resolve()        │  topic/key → internal SymbolId
│  ⑤ ZeptoPipeline::ingest_tick()         │  normalized tick → Ring Buffer
│  ⑥ Offset commit (manual)             │  after batch drain (at-least-once)
└─────────────────────────────────────────┘
        │
        ▼
TickPlant (MPMCRingBuffer, 65536 slots)
        │
        ▼
drain_sync() → ColumnStore (Partition per symbol/hour)
        │
        ▼
SQL QueryExecutor — real-time analytics
```

---

## 3. Component Design

### 3.1 KafkaConsumerConfig

```cpp
struct KafkaConsumerConfig {
    // Connection
    std::vector<std::string> brokers;           // bootstrap.servers
    std::string              group_id;           // consumer group ID
    std::vector<std::string> topics;             // topic subscription list

    // Message format
    KafkaMessageFormat format = KafkaMessageFormat::JSON;

    // JSON field name mapping (customizable per schema)
    std::string json_symbol_field    = "symbol";
    std::string json_price_field     = "price";
    std::string json_volume_field    = "volume";
    std::string json_ts_field        = "timestamp";
    int64_t     price_scale          = 1;        // float → int: 150.25 * 100 = 15025

    // Performance
    int32_t  batch_size              = 1000;     // messages per poll
    int32_t  poll_timeout_ms         = 1;        // low-latency polling
    size_t   backpressure_threshold  = 50000;    // TickPlant queue depth limit

    // Reliability
    bool        auto_commit          = false;    // manual commit (at-least-once)
    bool        skip_on_error        = true;     // skip malformed messages
    int32_t     max_retry            = 3;        // transient error retries
    std::string auto_offset_reset    = "latest"; // "earliest" | "latest"

    // TLS / SASL (Confluent Cloud, MSK, etc.)
    std::string security_protocol;   // "SASL_SSL", "SSL", "PLAINTEXT"
    std::string sasl_mechanism;      // "PLAIN", "SCRAM-SHA-256", "OAUTHBEARER"
    std::string sasl_username;
    std::string sasl_password;
    std::string ssl_ca_location;     // /path/to/ca.pem
};
```

### 3.2 Message Formats

```cpp
enum class KafkaMessageFormat {
    JSON,         // {"symbol":"AAPL","price":15000,"volume":100,"timestamp":...}
    AVRO,         // Confluent Schema Registry Avro (Phase 2)
    PROTOBUF,     // Google Protobuf (Phase 2)
    BINARY,       // Custom fixed layout: [sym_id:4][price:8][vol:8][ts:8]
    APEX_NATIVE,  // TickMessage raw bytes — zero-copy memcpy (internal use)
};
```

**Priority order for implementation:**
1. `JSON` — universal, works with any producer
2. `BINARY` — zero-allocation fast path for internal pipelines
3. `APEX_NATIVE` — micro-latency internal cluster transport
4. `AVRO` / `PROTOBUF` — enterprise schema registry integration (Phase 2)

### 3.3 MessageDecoder Interface

```cpp
class MessageDecoder {
public:
    virtual ~MessageDecoder() = default;

    // Decode a single Kafka message into a normalized TickMessage.
    // Returns false on parse error; caller decides skip vs abort.
    virtual bool decode(const void*        data,
                        size_t             len,
                        const std::string& topic,
                        const std::string& key,
                        zeptodb::ingestion::TickMessage& out) = 0;

    virtual std::string name() const = 0;
};

// Built-in decoders (all header-only or single .cpp)
class JsonDecoder;    // simdjson SIMD zero-copy parse
class BinaryDecoder;  // fixed-layout binary
class NativeDecoder;  // TickMessage memcpy (zero decode cost)
```

Users can inject a custom decoder for proprietary formats:

```cpp
class MyFIXDecoder : public MessageDecoder {
    bool decode(const void* data, size_t len, ..., TickMessage& out) override {
        // custom FIX-over-Kafka deserialization
        return true;
    }
};
```

### 3.4 KafkaSymbolMapper

Bridges string-based Kafka identities (topic name, message key) to ZeptoDB's
integer `SymbolId`.

```cpp
class KafkaSymbolMapper {
public:
    // Static mapping: topic "trades-AAPL" → symbol_id 1
    void add_topic(const std::string& topic, zeptodb::SymbolId id);

    // Key mapping: message key "AAPL" → symbol_id 1
    void add_key(const std::string& key, zeptodb::SymbolId id);

    // Dynamic mode: auto-assign incrementing IDs for unknown keys
    void set_dynamic(bool enabled);

    // Resolve: topic + key → SymbolId (0 = unknown/drop)
    zeptodb::SymbolId resolve(const std::string& topic,
                           const std::string& key) const;
};
```

**Resolution priority:** explicit key match > explicit topic match > dynamic auto-assign > 0 (drop)

### 3.5 KafkaConsumer

```cpp
class KafkaConsumer {
public:
    KafkaConsumer(const KafkaConsumerConfig&          cfg,
                  zeptodb::core::ZeptoPipeline&           pipeline,
                  std::unique_ptr<MessageDecoder>     decoder,
                  std::unique_ptr<KafkaSymbolMapper>  mapper);
    ~KafkaConsumer();

    bool start();    // launch consume_loop() thread
    void stop();     // graceful shutdown: drain in-flight, commit, join thread
    void pause();    // external backpressure (e.g. from Admin API)
    void resume();
    bool is_running() const;

    // Observability (exposed via /metrics Prometheus endpoint)
    int64_t messages_consumed() const;
    int64_t messages_errored()  const;
    int64_t consumer_lag()      const;   // sum of all partition lags
    double  throughput_mps()    const;   // messages/sec (60s EMA)

private:
    void consume_loop();
    bool process_message(rd_kafka_message_t* msg);
    void commit_offsets();
    void update_lag();

    KafkaConsumerConfig                cfg_;
    zeptodb::core::ZeptoPipeline&          pipeline_;
    std::unique_ptr<MessageDecoder>    decoder_;
    std::unique_ptr<KafkaSymbolMapper> mapper_;

    rd_kafka_t*       rk_    = nullptr;
    rd_kafka_queue_t* queue_ = nullptr;

    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<bool>     paused_{false};

    std::atomic<int64_t>  consumed_{0};
    std::atomic<int64_t>  errored_{0};
    std::atomic<int64_t>  lag_{0};
};
```

---

## 4. Core Algorithms

### 4.1 consume_loop() — Main Thread

```
while (running_) {
    // ① Backpressure: protect TickPlant Ring Buffer from overflow
    if (pipeline_.tick_plant().queue_depth() > backpressure_threshold) {
        sleep(1ms); continue;
    }
    if (paused_) { sleep(1ms); continue; }

    // ② Batch poll (zero-copy: rd_kafka_message_t holds payload pointer)
    msgs = rd_kafka_consume_batch_queue(queue_, poll_timeout_ms, batch_size);

    // ③ Decode + ingest each message
    for msg in msgs:
        if msg.err == RD_KAFKA_RESP_ERR__PARTITION_EOF: continue
        if msg.err != 0: handle_error(msg); continue
        tick = TickMessage{}
        if decoder_.decode(msg.payload, msg.len, topic, key, tick):
            pipeline_.ingest_tick(tick)
            consumed_++
        else:
            errored_++
            if !skip_on_error: stop(); return

    // ④ Commit offsets AFTER all ingests (at-least-once guarantee)
    //    Crash between ingest and commit → replay on restart (idempotent)
    rd_kafka_commit_queue(rk_, queue_, nullptr, nullptr, nullptr)

    // ⑤ Free message memory
    for msg in msgs: rd_kafka_message_destroy(msg)
}
```

### 4.2 Offset Commit Strategy

```
Timeline:
  poll() ──► decode ──► ingest_tick() ──► commit_offset()
                                   ▲
                         crash here = data safe (not committed)
                         restart replays same messages (at-least-once)

  poll() ──► [auto_commit here] ──► ingest_tick()
                         ▲
                crash here = DATA LOSS (committed but not ingested)
                → Why auto_commit = false is the default
```

### 4.3 JSON Decoder (hot path)

Uses `simdjson` for SIMD-accelerated zero-copy parsing:

```cpp
bool JsonDecoder::decode(const void* data, size_t len, ..., TickMessage& out) {
    // simdjson on-demand parser: no allocation, SIMD scan
    auto doc    = parser_.iterate(data, len, len + simdjson::SIMDJSON_PADDING);
    out.symbol_id = mapper_->resolve(topic, key);
    out.price     = int64_t(doc[price_field_].get_int64()) * price_scale_;
    out.volume    = doc[volume_field_].get_int64();
    out.recv_ts   = doc[ts_field_].get_int64();   // or NOW() if missing
    out.msg_type  = 0;  // Trade
    return out.symbol_id != 0;
}
```

---

## 5. File Structure

```
include/zeptodb/feeds/
    kafka_consumer.h           — public interface (all types + KafkaConsumer)

src/feeds/
    kafka_consumer.cpp         — KafkaConsumer + KafkaSymbolMapper impl
    kafka_json_decoder.cpp     — JsonDecoder (simdjson)
    kafka_binary_decoder.cpp   — BinaryDecoder, NativeDecoder

tests/feeds/
    test_kafka_consumer.cpp    — unit tests
    mock_kafka.h               — librdkafka mock for testing without broker

docs/feeds/
    KAFKA_CONSUMER_GUIDE.md    — usage guide with examples
```

---

## 6. CMake Integration

```cmake
# Optional: detect librdkafka
find_library(RDKAFKA_LIB rdkafka)
find_path(RDKAFKA_INCLUDE librdkafka/rdkafka.h)

if(RDKAFKA_LIB AND RDKAFKA_INCLUDE)
    option(APEX_USE_KAFKA "Enable Kafka consumer" ON)
else()
    option(APEX_USE_KAFKA "Enable Kafka consumer" OFF)
    message(STATUS "librdkafka not found — Kafka consumer disabled")
endif()

if(APEX_USE_KAFKA)
    add_library(zepto_kafka STATIC
        src/feeds/kafka_consumer.cpp
        src/feeds/kafka_json_decoder.cpp
        src/feeds/kafka_binary_decoder.cpp)
    target_include_directories(zepto_kafka PUBLIC include ${RDKAFKA_INCLUDE})
    target_link_libraries(zepto_kafka PRIVATE ${RDKAFKA_LIB})
    target_compile_definitions(zepto_kafka PUBLIC APEX_KAFKA_ENABLED)
    # Optional: simdjson for JsonDecoder
    if(TARGET simdjson)
        target_link_libraries(zepto_kafka PRIVATE simdjson)
        target_compile_definitions(zepto_kafka PRIVATE APEX_KAFKA_SIMDJSON)
    endif()
endif()
```

---

## 7. Usage Examples

### Basic JSON consumer

```cpp
KafkaConsumerConfig cfg;
cfg.brokers   = {"kafka-broker:9092"};
cfg.group_id  = "zepto-analytics";
cfg.topics    = {"trades"};
cfg.format    = KafkaMessageFormat::JSON;
cfg.price_scale = 100;  // "$150.25" stored as 15025

auto mapper = std::make_unique<KafkaSymbolMapper>();
mapper->set_dynamic(true);  // auto-assign symbol IDs from message keys

auto consumer = std::make_unique<KafkaConsumer>(
    cfg, pipeline,
    std::make_unique<JsonDecoder>(cfg, mapper.get()),
    std::move(mapper));

consumer->start();
// ... run analytics ...
consumer->stop();
```

### Confluent Cloud (SASL_SSL)

```cpp
KafkaConsumerConfig cfg;
cfg.brokers            = {"pkc-xxxxx.us-east-1.aws.confluent.cloud:9092"};
cfg.group_id           = "zepto-prod";
cfg.topics             = {"market-data"};
cfg.security_protocol  = "SASL_SSL";
cfg.sasl_mechanism     = "PLAIN";
cfg.sasl_username      = secrets->get("KAFKA_API_KEY");
cfg.sasl_password      = secrets->get("KAFKA_API_SECRET");
```

### Custom decoder (Avro / proprietary format)

```cpp
class MyAvroDecoder : public MessageDecoder {
    bool decode(const void* data, size_t len,
                const std::string& topic, const std::string& key,
                zeptodb::ingestion::TickMessage& out) override {
        auto record = avro_deserialize(data, len, schema_registry_url_);
        out.symbol_id = mapper_->resolve(topic, record["symbol"].as_string());
        out.price     = record["price"].as_int64();
        out.volume    = record["volume"].as_int64();
        out.recv_ts   = record["timestamp"].as_int64();
        return out.symbol_id != 0;
    }
    std::string name() const override { return "avro"; }
};
```

### Admin API integration

```cpp
// Pause/resume via DELETE /admin/kafka/pause
server.set_kafka_consumer(std::move(consumer));

// GET /admin/kafka/stats
// → {"consumed": 1234567, "errored": 0, "lag": 42, "throughput_mps": 98432.1}
```

---

## 8. Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| Throughput | 1M msg/sec | Same as TickPlant capacity |
| End-to-end latency | < 500μs p99 | Kafka → ColumnStore |
| Memory (decode) | Zero-alloc on hot path | simdjson on-demand, no heap |
| Offset commit overhead | < 1% of throughput | Batch commit every 1000 msgs |
| Backpressure activation | < 1ms | queue_depth poll every iteration |

---

## 9. Reliability Design

### At-least-once delivery

- `auto_commit = false` by default
- Offsets committed AFTER successful `ingest_tick()` + drain
- Crash between ingest and commit → replay on restart → idempotent (same tick inserted twice, same partition/hour → overwrite)

### Error handling

| Error type | Default behavior | Config override |
|-----------|-----------------|-----------------|
| Parse failure | Skip + increment errored_ | `skip_on_error=false` → stop |
| Broker disconnect | librdkafka auto-reconnect | `max_retry=3` |
| Partition EOF | Ignore (normal end of partition) | — |
| TickPlant full | Wait 1ms (backpressure) | `backpressure_threshold` |
| Unknown symbol | Drop message (symbol_id=0) | `mapper->set_dynamic(true)` |

### Exactly-once (Phase 2)

Requires Kafka transactions + idempotent producer on the source side.
Implementation deferred — at-least-once with idempotent ticks is sufficient
for financial analytics (duplicate ticks in same partition are overwritten).

---

## 10. Open Questions (Resolve Before Implementation)

| # | Question | Options | Recommendation |
|---|----------|---------|----------------|
| 1 | JSON parser library | simdjson vs nlohmann vs custom | **simdjson** (SIMD, zero-alloc) |
| 2 | Avro priority | Implement now vs Phase 2 | **Phase 2** (JSON+Binary covers 80%) |
| 3 | Schema Registry | Confluent SR vs inline schema | **Confluent SR** for Avro (if/when needed) |
| 4 | Multi-consumer threads | 1 thread vs N threads (1/partition) | **1 thread + batch poll** initially |
| 5 | Admin API pause/resume | HTTP endpoint vs programmatic only | **Both** (Admin REST + programmatic) |
| 6 | Exactly-once | at-least-once vs exactly-once | **at-least-once** (simpler, sufficient) |

---

## 11. Implementation Phases

### Phase 1 (Core — implement first)
- `KafkaConsumerConfig`, `KafkaSymbolMapper`
- `MessageDecoder` interface + `JsonDecoder` + `BinaryDecoder` + `NativeDecoder`
- `KafkaConsumer` with consume_loop, backpressure, manual offset commit
- CMake optional build (`APEX_USE_KAFKA`)
- Unit tests with mock Kafka (no broker needed)
- `KAFKA_CONSUMER_GUIDE.md`

### Phase 2 (Enterprise)
- `AvroDecoder` + Confluent Schema Registry client
- `ProtobufDecoder`
- Admin REST API: `GET /admin/kafka/stats`, `POST /admin/kafka/pause`
- Prometheus metrics: `zepto_kafka_consumed_total`, `zepto_kafka_lag`
- Multi-topic fan-out (one consumer, N symbol groups)

### Phase 3 (Advanced)
- Kafka Connect Sink plugin (Java bridge)
- Per-partition consumer threads (N-thread scaling)
- Exactly-once with Kafka transactions

---

*Related docs:*
- *`docs/design/layer2_ingestion_network.md` — TickPlant, Ring Buffer*
- *`docs/feeds/FEED_HANDLER_GUIDE.md` — FIX, ITCH feed handlers*
- *`docs/design/layer5_security_auth.md` — SecretsProvider for Kafka credentials*
