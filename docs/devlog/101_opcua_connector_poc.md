# Devlog 101 — OPC-UA Connector (PoC)

**Date:** 2026-04-25
**Priority:** P9 (Physical AI / Industry)
**Status:** PoC complete — production work tracked in BACKLOG P9 #1–#11

---

## Scope — files touched

| File | Change |
|---|---|
| `include/zeptodb/feeds/opcua_consumer.h` | **new** — connector header (configs, stats, class) |
| `src/feeds/opcua_consumer.cpp` | **new** — PoC body (no real UA_Client calls) |
| `tests/unit/test_opcua.cpp` | **new** — 22 unit tests |
| `CMakeLists.txt` | `ZEPTO_USE_OPCUA` option + `find_library(open62541)` + `zepto_opcua` library target |
| `tests/CMakeLists.txt` | `test_opcua.cpp` + `zepto_opcua` link |
| `docs/design/opcua_connector.md` | **new** — design doc |
| `docs/BACKLOG.md` | P9 OPC-UA row exploded into 11 production follow-ups |
| `docs/COMPLETED.md` | new "OPC-UA connector (PoC)" entry |
| `KIRO.md` | Design Docs table row for `opcua_connector.md` |

---

## What was built (PoC)

1. **`OpcUaConsumer`** — third connector after Kafka/MQTT, same architecture:
   - `set_pipeline()` / `set_routing()` wiring (single + multi-node).
   - `start()` / `stop()` / `is_running()` lifecycle.
   - `on_data_change(node_id, value, source_ts_ns)` — NodeId lookup →
     TickMessage → dispatch.
   - `ingest_decoded(TickMessage)` — shared dispatch with backpressure
     retry (byte-identical to `MqttConsumer::ingest_decoded`).
   - `OpcUaStats` with the same six counters as `MqttStats`.
   - Static helpers `coerce_variant_to_int64()`, `ua_datetime_to_ns()`,
     `is_valid_security()` — pure, testable.
2. **Optional-dep scaffolding** — `option(ZEPTO_USE_OPCUA ... OFF)`,
   `find_library(OPEN62541_LIB open62541 QUIET)`, sets
   `-DZEPTO_OPCUA_AVAILABLE=1` when found; otherwise silently disables and
   prints the install hint (`sudo dnf install -y open62541-devel`).
3. **NodeId → SymbolId mapping** — built in the ctor from
   `config.nodes`; O(1) `unordered_map<string, {SymbolId, value_scale}>`
   lookup per notification.
4. **1601-epoch → Unix ns conversion** — `ua_datetime_to_ns(int64)` with
   the exact `116 444 736 000 000 000` constant; clamps pre-1970 values
   to 0.
5. **Scalar variant coercion** — Int16/32/64 direct cast, Float/Double
   with `value_scale`, Bool → 0/1, Unsupported → `false`.
6. **Security config sanity check** — `is_valid_security(mode, policy)`
   rejects `Sign` or `SignAndEncrypt` with `Policy::None`. Does NOT
   validate certificate files on disk (production work).
7. **Table-aware ingest** — `config.table_name` → `SchemaRegistry::get_table_id`
   resolution in `set_pipeline()`, same drop-and-count behaviour as
   Mqtt/Kafka on unknown table.

## What was explicitly NOT built (deferred to BACKLOG P9)

| BACKLOG # | Item | Effort |
|---|---|---|
| 1 | Real open62541 client integration (session, subscription, monitored items, reconnect) | S |
| 2 | Basic256Sha256 security (cert loading, trust list) | M |
| 3 | Structured & array variant support | M |
| 4 | String values (blocked on string-column support) | S |
| 5 | `zepto-opcua-browse` CLI for auto-discovery | S |
| 6 | Historical Access (HA) backfill | M |
| 7 | Alarms & Conditions (A&C) | M |
| 8 | Reconnect / failover policy | S |
| 9 | UA StatusCode / quality → TickMessage.volume | S |
| 10 | Integration test against `tutorial_server_variable` | S |
| 11 | Server mode (P10 candidate) | L |

The PoC `start()` validates config, builds the node map, then returns
`false` with a log message both when the dep is missing and when it is
present but the production UA_Client path isn't wired yet. This keeps
the build green, the tests green, and the scaffold ready for item #1 to
drop into.

---

## Why open62541

- **MPL 2.0** — file-level copyleft, compatible with BUSL-1.1.
- **Dominant OSS OPC-UA stack** — used by Siemens, Beckhoff, B&R, Eclipse
  Milo Node; ships in reference industrial stacks.
- **Packaged on RHEL / Amazon Linux / Debian / Ubuntu** — single
  `find_library(open62541)` is enough for distro pickup.
- **Built-in `Basic256Sha256` + certificate store** — no third-party
  security crate needed.
- Commercial stacks (Unified Automation, Prosys) were rejected on
  per-seat licensing grounds.

---

## Reuse from Kafka / MQTT

| Aspect | Reuse source |
|---|---|
| Connector-layer architecture | `KafkaConsumer` / `MqttConsumer` (pipeline vs routing pattern) |
| Backpressure retry helper | `try_with_backpressure` inline lambda (same body) |
| Dispatch path (`ingest_decoded`) | `MqttConsumer::ingest_decoded` (byte-identical) |
| `OpcUaStats` counters | `MqttStats` (same six fields) |
| `table_name` resolution | `MqttConsumer::set_pipeline` (devlog 084) |
| License gate | `Feature::IOT_CONNECTORS` — license_validator.h already named OPC-UA as a future user of this bit |
| Optional-dep pattern | `ZEPTO_USE_MQTT` / `ZEPTO_MQTT_AVAILABLE` (same CMake idiom) |

The retry helper was intentionally duplicated locally rather than refactored
out of `mqtt_consumer.cpp` into a shared header — per PoC minimality
rules, cross-file refactors are deferred until there's a third caller
that actually needs them.

---

## Deliberate deltas vs MQTT / Kafka

1. **No `MessageFormat`.** OPC-UA is a typed wire protocol; every
   Variant carries its own type tag. No JSON/BINARY/JSON_HUMAN branch.
2. **No top-level `symbol_map`.** Each `OpcUaNodeMap` binds NodeId →
   SymbolId + its own `value_scale`. OPC-UA address spaces have
   heterogeneous engineering units per node, so per-node scale is
   denser than a shared map.
3. **Subscription params replace QoS / commit-mode.** `publishing_interval_ms`
   + `sampling_interval_ms` + `queue_size` + `discard_oldest` replace
   MQTT's QoS and Kafka's poll/commit knobs.
4. **MVP simplification: `volume = 0`.** OPC-UA has no trade-volume
   concept. Mapping UA StatusCode / quality into `volume` (or a
   dedicated column) is tracked as BACKLOG P9 #9.

---

## Test coverage summary

22 Google Test cases in `tests/unit/test_opcua.cpp`, all passing with the
default (`ZEPTO_USE_OPCUA=OFF`) build:

| Suite | Cases |
|---|---|
| `OpcUaConfig` | 1 (Defaults) |
| `OpcUaCoerceVariant` | 6 (Int32, Int64Negative, DoubleScale, FloatScale, BoolTrueFalse, UnsupportedReturnsFalse) |
| `OpcUaUaDatetime` | 3 (Epoch1970IsZero, OneSecondAfterEpoch, Before1970Returns0) |
| `OpcUaSecurity` | 3 (NoneAlwaysValid, SignRequiresNonNullPolicy, SignAndEncryptRequiresNonNullPolicy) |
| `OpcUaNodeIdMap` | 2 (Miss_IncrementsDecodeErrors, Hit_BuildsTickAndDispatches) |
| `OpcUaIngestDecoded` | 3 (NoPipelineFails, SingleNode_Dispatches, Backpressure_ExhaustsRetries) |
| `OpcUaLifecycle` | 3 (StartWithoutDep_ReturnsFalse, StartRejectsEmptyNodes, StopIsIdempotent) |
| `OpcUaStats` | 1 (MatchCounters) |

Full regression: **1243 / 1243 tests pass** (was 1221 before this PoC).
Wall 276 s on the dev box.

Integration test against `tutorial_server_variable` is BACKLOG P9 #10 and
will land together with the real `UA_Client` path.

---

## How to enable at build time

```bash
sudo dnf install -y open62541-devel    # RHEL / Amazon Linux
# or
sudo apt install -y libopen62541-dev   # Debian / Ubuntu

cmake -B build -GNinja -DZEPTO_USE_OPCUA=ON ..
ninja -C build zepto_tests
./build/tests/zepto_tests --gtest_filter="OpcUa*"
```

If the library is absent at configure time, CMake prints the install hint
and silently turns the option back off — the default build is unchanged.

---

## Follow-up items

All production work is in BACKLOG P9 items **1–11** (see the P9 block in
`docs/BACKLOG.md`). The critical-path item is #1 (real open62541 client
integration); the rest unblock progressively.

---

## Related devlogs

- 081 — `MqttConsumer` (first of the three connector twins)
- 082 — Table-scoped partitioning (provides `table_id` routing)
- 084 — Table-aware ingest paths (provides `table_name` resolution used here)
