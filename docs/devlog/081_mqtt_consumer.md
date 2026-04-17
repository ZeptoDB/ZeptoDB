# 081 — MQTT Consumer (IoT / Physical AI Ingestion)

**Date:** 2026-04-17
**Scope:** `include/zeptodb/feeds/mqtt_consumer.h`, `src/feeds/mqtt_consumer.cpp`, `tests/unit/test_mqtt.cpp`, `CMakeLists.txt`

---

## What was built

`MqttConsumer` — a near-drop-in twin of `KafkaConsumer` that subscribes to MQTT
topics (single topic or wildcard such as `sensors/#`, `plant/+/temp`) and routes
ticks through the existing ingestion pipeline, enabling IoT / Physical AI
ingestion (factory sensors, robots, autonomous-vehicle CAN streams, drones).

Like Kafka, it supports two routing modes:

- **Single-node** — `set_pipeline()`: all ticks go to the local `ZeptoPipeline`.
- **Multi-node** — `set_routing()`: the `PartitionRouter` decides per-tick
  whether to dispatch locally or forward to a remote node via
  `TcpRpcClient::ingest_tick()`.

## Config (`MqttConfig`)

| Field | Default | Purpose |
|---|---|---|
| `broker_uri` | `tcp://localhost:1883` | `tcp://`, `ssl://` or `ws://` URI |
| `topic` | — | Topic filter — single or wildcard (`#`, `+`) |
| `client_id` | `zepto-mqtt-consumer` | MQTT client ID |
| `username` / `password` | `""` | Optional credentials |
| `qos` | `1` | 0 = at-most-once, 1 = at-least-once, 2 = exactly-once |
| `keepalive_sec` | `30` | PINGREQ interval |
| `clean_session` | `true` | Fresh session on connect |
| `format` | `MessageFormat::JSON` | Reuses Kafka's enum (JSON / BINARY / JSON_HUMAN) |
| `price_scale` | `10000.0` | Float → int64 fixed-point (JSON_HUMAN) |
| `symbol_map` | `{}` | Name → `SymbolId` (JSON_HUMAN) |
| `backpressure_retries` / `backpressure_sleep_us` | `3` / `100µs` | Ring-buffer-full retry |

## Message formats

Shared with Kafka — the static decoders on `MqttConsumer` are thin wrappers over
`KafkaConsumer::decode_json` / `decode_binary` / `decode_json_human`. Single
source of truth for wire-format parsing, zero duplication.

- `JSON`         — `{"symbol_id":1,"price":15000,"volume":100,"ts":...}`
- `BINARY`       — raw `TickMessage` bytes (64 B, cache-line aligned)
- `JSON_HUMAN`   — `{"symbol":"sensor_a","price":23.75,"volume":1}` with
  `symbol_map` lookup

## QoS handling

`is_valid_qos()` rejects anything outside `{0, 1, 2}`. `start()` fails early on
bad QoS or empty topic — no broker connection attempted.

## Optional-dep pattern

Mirrors the Kafka pattern exactly:

```cmake
option(ZEPTO_USE_MQTT "Enable MQTT consumer (requires paho-mqttpp3)" OFF)
# … find_library(paho-mqttpp3) + find_library(paho-mqtt3a) …
add_library(zepto_mqtt STATIC src/feeds/mqtt_consumer.cpp)
if(ZEPTO_HAS_MQTT)
    target_compile_definitions(zepto_mqtt PUBLIC ZEPTO_MQTT_AVAILABLE=1)
    target_link_libraries(zepto_mqtt PUBLIC ${PAHO_MQTTPP_LIB} ${PAHO_MQTT_LIB})
endif()
```

Without Paho installed:

- `zepto_mqtt` still compiles and links.
- Decode + routing APIs work fully (tests exercise these without a broker).
- `start()` logs a warning and returns `false` cleanly.

With `ZEPTO_USE_MQTT=ON` and Paho available, `start()` connects asynchronously,
subscribes, and receives messages via Paho's internal callback thread — no
separate poll thread is needed (unlike Kafka's `consume()` loop).

## How to enable

```bash
sudo dnf install -y paho-c-devel paho-cpp-devel   # or equivalent
cd build && cmake -DZEPTO_USE_MQTT=ON .. && ninja
```

## Tests

`tests/unit/test_mqtt.cpp` — 18 tests covering:

- `MqttConfig` defaults
- QoS validation (0/1/2 pass, -1/3/99 rejected; `start()` rejects bad QoS + empty topic)
- `decode_json` basic / malformed / empty
- `decode_binary` exact / wrong size
- `decode_json_human` with symbol_map / unknown symbol
- Routing: no pipeline → fail; single-node pipeline → `route_local`
- Routing: multi-node `PartitionRouter` with missing RPC client → `ingest_failures`
- `on_message` stats tracking (success, empty payload, malformed JSON)
- `start()` without the Paho library — returns `false` gracefully

## Results

- Build: clean (`ninja zepto_tests` — 0 errors, 0 warnings).
- New tests: 18 / 18 pass.
- Full regression: 1182 / 1182 pass — no regressions.


## Follow-up fixes (reviewer pass)

### License gate — `Feature::IOT_CONNECTORS` (bit 8)

Added a new license feature flag `Feature::IOT_CONNECTORS` and gated
`MqttConsumer::start()` with it, mirroring the Kafka/Migration/Cluster
pattern established in devlog 069 (feature gates batch 2).

Rationale:

- **Parity with Kafka/Pulsar connector policy** (devlog 069): enterprise-grade
  streaming integrations are Enterprise-only.
- **Leaves room for future IoT connectors** under the same gate — OPC-UA
  (industrial PLCs), ROS2 (robotics), Pulsar, Kinesis — without minting a new
  feature bit per connector.
- Trial keys now set `features=511` (bits 0..8) instead of 255 so evaluators
  can exercise MQTT during the 30-day trial.

Implementation:

- `include/zeptodb/auth/license_validator.h` — new `IOT_CONNECTORS = 1u << 8`.
- `src/feeds/mqtt_consumer.cpp` — `start()` returns `false` with `ZEPTO_WARN`
  when the feature is absent; identical shape to the Kafka gate.
- `src/server/http_server.cpp` — `/api/license` `features[]` array now
  includes `"iot_connectors"` (two occurrences of the feat_map table).
- `src/auth/license_validator.cpp` — `generate_trial_key()` bumped to 511.
- `tests/unit/test_mqtt.cpp` — new `StartRejectedWithoutLicense` test
  (guarded by `#ifdef ZEPTO_MQTT_AVAILABLE`).

### Minor cleanups

- `mqtt_consumer.h` — top-of-file comment now calls out the deliberate
  deltas from `KafkaConsumer`: (1) no poll thread (Paho owns the callback
  thread), (2) no `CommitMode` (MQTT has no consumer offsets — delivery
  semantics live in QoS).
- `mqtt_consumer.h` — removed the unused `#include <thread>`.
- `mqtt_consumer.h` — `MqttConfig::topic` marked `// REQUIRED — must not be
  empty; start() returns false otherwise`.
- `mqtt_consumer.cpp` — `stop()` now has an inline comment on
  `client->disconnect()->wait();` noting that the wait quiesces the Paho
  callback thread before `delete`, preventing a use-after-free race.

### Not done (deferred as non-blocking)

- `spdlog::info/warn/error` → `ZEPTO_INFO/WARN/ERROR` sweep inside
  `mqtt_consumer.cpp`. Reviewer flagged this as follow-up cleanup, not a
  blocker for correctness. Tracked as a general engine-logging task.


## Logging sweep follow-up

Both `src/feeds/kafka_consumer.cpp` and `src/feeds/mqtt_consumer.cpp` have been
converted from direct `spdlog::*` calls to the engine `ZEPTO_*` macros for
engine-code logging consistency (per KIRO.md logging rules). The direct
`#include "spdlog/spdlog.h"` was removed from both files — `ZEPTO_*` macros are
provided via `zeptodb/common/logger.h`. Format strings, log levels, and
messages are identical; no behavioral change.


## Cross-arch CI parity (2026-04-17)

Graviton (ARM64) CI previously ran with all optional connector flags OFF, so
Kafka/MQTT tests were compile-gated out on aarch64 — a silent coverage gap.

Fix: `.github/workflows/graviton-test.yml` now installs `librdkafka-dev` and
`libpaho-mqttpp-dev` and builds with `-DZEPTO_USE_KAFKA=ON -DZEPTO_USE_MQTT=ON`.
Both consumers are now exercised on every push to `main` on both x86_64 and
ARM64. No source code changes were required — the feed consumer code is
arch-neutral C++20.
