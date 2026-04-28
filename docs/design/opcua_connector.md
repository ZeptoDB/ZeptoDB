# OPC-UA Connector Design

**Status:** Tier-1 shipped (PoC devlog 101, Tier-1 pre-work devlog 105,
real `UA_Client` integration devlog 106). Tier-2 shipped in Sprint 2:
quality mapping + integration test (devlog 107), Basic256Sha256
security (devlog 108), reconnect / failover policy (devlog 109).
Connector is now ready for first-commercial industrial deployments.
**Library:** [open62541](https://www.open62541.org/) (MPL 2.0)
**Source:** `include/zeptodb/feeds/opcua_consumer.h` · `src/feeds/opcua_consumer.cpp`
**Tests:** `tests/unit/test_opcua.cpp`

---

## 1. Goal & Scope

Ingest data from OPC-UA servers (PLCs, DCS, SCADA gateways, industrial
historians) into ZeptoDB through the same single contract every other
connector uses:

```
OPC-UA server ──► OpcUaConsumer ──ingest_tick(TickMessage)──► ZeptoPipeline
```

This unlocks **Sector B (smart-factory / industrial)** use-cases — Samsung,
SK, TSMC, Siemens — where OPC-UA is the dominant fieldbus-to-IT protocol.

### PoC scope (MVP — this devlog)

- **Client mode only.** OPC-UA server mode (exposing ZeptoDB queries) is
  backlog.
- **Scalar variants only.** Int16 / Int32 / Int64 / Float / Double /
  Boolean. Float/Double are coerced to `int64` via a per-node
  `value_scale` — identical pattern to Kafka/MQTT JSON_HUMAN decode.
- **SourceTimestamp preferred, ServerTimestamp fallback.**
- **Single subscription, one or more monitored items.**
- Config validation, NodeId→SymbolId mapping, routing (single/multi-node),
  backpressure retry, table-aware ingest, stats.
- **Real `UA_Client` integration** (devlog 106): `start()` opens a
  session, creates one subscription, registers one MonitoredItem per
  configured node, and drives async callbacks via
  `UA_Client_run_iterate()` on a single background thread. Honours the
  `connect_timeout_ms` / `session_timeout_ms` knobs. Without the
  open62541 dep, `start()` still returns `false` — the default build is
  byte-identical to the PoC.

### Deferred to BACKLOG P9 (production)

1. ~~Real `UA_Client` session setup, endpoint discovery, subscription,
   monitored items, data-change callback wiring~~ — **shipped in devlog
   106 (BACKLOG P9 #2b)**. Reconnect loop shipped in devlog 109 (P9 #2i).
2. ~~`Basic256Sha256` security (certificate loading, trust list, server
   cert validation).~~ — **shipped in devlog 108 (BACKLOG P9 #2c)**. MVP
   limits: single-cert trust list, no revocation list.
3. Structured & array variants, engineering units.
4. String variants (blocked on string-column column support).
5. `zepto-opcua-browse` CLI for address-space auto-discovery.
6. Historical Access (HA) for backfill.
7. Alarms & Conditions (A&C) as a separate tick stream.
8. ~~Reconnect / failover policy knobs.~~ — **shipped in devlog 109
   (BACKLOG P9 #2i)**: exponential backoff up to 16× base, ±25% jitter,
   automatic subscription rebuild, `OpcUaStats::reconnects` counter.
9. UA StatusCode / quality mapping to `volume` or a dedicated column.
10. Integration test against open62541's `tutorial_server_variable`.
11. OPC-UA **server mode** (P10 candidate).

---

## 2. Why open62541

| Criterion | open62541 |
|-----------|-----------|
| License | MPL 2.0 — compatible with ZeptoDB BUSL-1.1 (file-level copyleft only) |
| Maturity | Used by Siemens, Beckhoff, B&R, Eclipse Milo Node, and shipped inside reference stacks |
| Footprint | Single C library, `find_library(open62541)`, `~1 MB` static |
| Security | Built-in `Basic256Sha256`, certificate store, UA 1.04 compliant |
| Distro coverage | `dnf install open62541-devel` on RHEL/Amazon Linux; `apt install libopen62541-dev` on Debian/Ubuntu |

Commercial stacks (Unified Automation, Prosys) were rejected: per-seat
licensing conflicts with the OSS edition's "no runtime fees" promise.

---

## 3. Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      connector layer                        │
│                                                             │
│   KafkaConsumer     MqttConsumer     OpcUaConsumer          │
│   ──────────▲─────────────▲─────────────▲──────             │
│              \             │             /                  │
│               \            │            /                   │
│                ── ingest_tick(TickMessage) ──               │
│                            │                                │
└────────────────────────────┼────────────────────────────────┘
                             ▼
                    ┌────────────────┐
                    │  ZeptoPipeline │    (single engine contract)
                    └────────────────┘
                             │
                             ▼
              PartitionRouter → local | remote (TcpRpcClient)
```

All three connectors are **structurally identical**: their only interface
with the engine is `ZeptoPipeline::ingest_tick(TickMessage)`. This
guarantees that:

- The engine does not learn about new protocols; adding OPC-UA is a pure
  connector-layer change.
- Multi-node routing (`PartitionRouter` + `TcpRpcClient::ingest_tick`)
  works for OPC-UA the moment it works for anything else.
- Backpressure, stats, and the table-aware ingest flow (devlog 084) are
  free-by-construction.

---

## 4. Data Model

### 4.1 Node → TickMessage

| OPC-UA field | TickMessage field | Notes |
|---|---|---|
| NodeId (string form) | — | looked up in `node_map_` |
| → SymbolId | `symbol_id` | from `OpcUaNodeMap.symbol_id` |
| Variant (Int16/32/64) | `price` (int64) | direct cast |
| Variant (Float/Double) | `price` (int64) | `value * value_scale` |
| Variant (Boolean) | `price` (int64) | 0 or 1 |
| SourceTimestamp (UA DateTime) | `recv_ts` | → ns since Unix epoch |
| ServerTimestamp (UA DateTime) | `recv_ts` | fallback when source is absent |
| — | `volume` | **0 (MVP)** — UA has no trade-volume concept |
| — | `table_id` | resolved once from `OpcUaConfig.table_name` |

### 4.2 UA DateTime → Unix ns

OPC-UA `DateTime` is 100-ns ticks since **1601-01-01 UTC**. Unix epoch
(1970-01-01 UTC) is 11 644 473 600 s = `116 444 736 000 000 000` ticks
later. Conversion:

```cpp
ns = (ua_datetime_100ns_since_1601 - 116444736000000000LL) * 100;
// values before 1970 clamp to 0
```

Implemented as `OpcUaConsumer::ua_datetime_to_ns()` (pure, unit-tested).

### 4.3 Variant coercion

`OpcUaConsumer::coerce_variant_to_int64(variant, scale, out)` handles the
six MVP scalar types. Array and Structured variants are BACKLOG P9 #3.

### 4.4 Quality handling (`UA_DataValue.status` → `TickMessage.volume`)

Each DataChange notification carries a 32-bit `UA_StatusCode`
(`0 = UA_STATUSCODE_GOOD`; anything else = uncertain/bad).
`OpcUaConfig::quality_handling` controls how it lands in the tick
stream (devlog 107, BACKLOG P9 #2j):

| Mode | Behaviour |
|---|---|
| `AcceptAllGoodAs1` (**default**) | Forward every sample; `TickMessage.volume = 1` if GOOD, else `0`. Gives queries a trivially-filterable boolean quality bit without dropping data. |
| `AcceptAll` | Forward every sample; raw 32-bit status is preserved in `TickMessage.volume`. Use when downstream analytics need the precise subcode. |
| `IgnoreBad` | Drop non-GOOD samples at decode time; `decode_errors++` per drop. Use when aggregates (SUM, AVG, anomaly detection) cannot tolerate uncertain values. |

The policy branch runs inside `on_data_change()` — so a single flip of
`config.quality_handling` before `start()` changes fleet-wide behaviour
with no code change in any downstream operator. The default flip
(`volume = 1` on GOOD) is the **only** OFF-build behaviour delta vs
Sprint 1; see devlog 107 §3.

### 4.5 Security (Basic256Sha256)

Transport security wires `OpcUaConfig::{security_mode, security_policy,
client_cert_path, client_key_path, server_cert_path}` to open62541's
`UA_ClientConfig_setDefaultEncryption()` (devlog 108, BACKLOG P9 #2c).

Supported combos:

| `security_mode` | `security_policy` | Behaviour |
|---|---|---|
| `None` | `None` | `UA_ClientConfig_setDefault` — plain TCP, dev-only |
| `Sign` | `Basic256Sha256` | `setDefaultEncryption` + `UA_MESSAGESECURITYMODE_SIGN` |
| `SignAndEncrypt` | `Basic256Sha256` | `setDefaultEncryption` + `UA_MESSAGESECURITYMODE_SIGNANDENCRYPT` |

`start()`'s validation pipeline runs, in order:

1. `is_valid_security(mode, policy)` — rejects `Sign` / `SignAndEncrypt`
   paired with `Policy::None` (Sprint 1).
2. **New:** `Sign` / `SignAndEncrypt` requires both `client_cert_path`
   and `client_key_path` to be non-empty.  Emptiness is rejected here
   with `ZEPTO_ERROR` before the license gate, so misconfigurations
   surface their real cause instead of a misleading licensing error.
3. License gate (`Feature::IOT_CONNECTORS`).
4. Under `#ifdef ZEPTO_OPCUA_AVAILABLE`: cert + key files are read into
   `UA_ByteString`s, `UA_ClientConfig_setDefaultEncryption` is called,
   and `cc->securityMode` is set from the enum.  `server_cert_path`
   populates a single-entry trust list.

MVP limits (Sprint 3 follow-ups):

- No revocation list — `setDefaultEncryption`'s revoke args are `nullptr, 0`.
- Trust list is at most **one** server cert; full CA chains require a
  trust-directory iterator.
- Live Basic256Sha256 round-trip integration test (ephemeral
  `openssl req` keypair against a Sign-mode open62541 server) is
  deferred; the None/None integration path (devlog 107) is the only
  live round-trip in CI today.

---

### 4.6 Reconnect / failover

The background publish-loop thread drives `UA_Client_run_iterate()` and
inspects its `UA_StatusCode` return. When the code matches any of
`UA_STATUSCODE_BADCONNECTIONCLOSED`, `UA_STATUSCODE_BADSERVERNOTCONNECTED`
or `UA_STATUSCODE_BADSECURECHANNELCLOSED`, it sleeps a jittered backoff
and calls `UA_Client_connect()` again on the same `UA_Client` — which
reuses the installed `ClientConfig` (including any Basic256Sha256
setup from §4.5) without re-priming encryption. On success it calls
`setup_subscription()` to rebuild the subscription + MonitoredItems
(the server-side objects died with the session) and increments
`OpcUaStats::reconnects`. On failure the backoff doubles up to a
ceiling of `config_.reconnect_interval_ms * 16` (≈ 32 s with the
default 2 s base). See devlog 109 for the full state-machine and
formula.

```
backoff_ms     = reconnect_interval_ms          // on reset / success
max_backoff_ms = reconnect_interval_ms * 16
on connect failure:  backoff_ms = min(backoff_ms * 2, max_backoff_ms)
sleep = backoff_ms ± 25%  (uniform jitter, 50 ms-slice interruptible)
```

**Observability.** `OpcUaStats::reconnects` counts successful reconnects
and is exposed through the existing `stats()` snapshot; wire into
Prometheus via the shared feeds metrics exporter.

**Not yet tested (Sprint 3 follow-up).** Live disconnect simulation
(kill + restart tutorial server, assert `reconnects >= 1`). The unit
tests in Sprint 2 cover the counter and the doubling-clamp math only;
the reconnect path's compile-time correctness is covered by the
`ZEPTO_USE_OPCUA=ON` build.

---

## 5. `OpcUaConfig` Schema

```cpp
struct OpcUaConfig {
    std::string endpoint         = "opc.tcp://localhost:4840";
    std::string client_name      = "zepto-opcua-client";

    enum class SecurityMode   { None, Sign, SignAndEncrypt };
    enum class SecurityPolicy { None, Basic256Sha256 };
    SecurityMode   security_mode   = SecurityMode::None;
    SecurityPolicy security_policy = SecurityPolicy::None;

    std::string username, password;
    std::string client_cert_path, client_key_path, server_cert_path;

    // Subscription parameters
    double   publishing_interval_ms = 100.0;
    double   sampling_interval_ms   = 50.0;
    uint32_t queue_size             = 10;
    bool     discard_oldest         = true;

    std::vector<OpcUaNodeMap> nodes;     // NodeId → SymbolId + scale
    std::string table_name;              // optional SchemaRegistry lookup

    int backpressure_retries   = 3;
    int backpressure_sleep_us  = 100;
};

struct OpcUaNodeMap {
    std::string node_id;          // e.g. "ns=2;s=Temperature"
    SymbolId    symbol_id;
    double      value_scale = 10000.0;
};
```

---

## 6. API Surface

Mirrors `MqttConsumer` exactly — the same method set, same semantics:

| Method | Purpose |
|---|---|
| `set_pipeline(ZeptoPipeline*)` | single-node mode; resolves `table_name` |
| `set_routing(local_id, router, remotes)` | multi-node mode |
| `start()` / `stop()` / `is_running()` | lifecycle |
| `stats()` | snapshot `OpcUaStats` |
| `on_data_change(node_id, value, src_ts_ns)` | pure, testable |
| `ingest_decoded(TickMessage)` | pure, testable dispatch |
| static `coerce_variant_to_int64(v, scale, out)` | pure |
| static `ua_datetime_to_ns(ua_dt)` | pure |
| static `is_valid_security(mode, policy)` | pure |

`OpcUaStats` has the same six counters as `MqttStats`:
`messages_consumed`, `bytes_consumed`, `decode_errors`, `route_local`,
`route_remote`, `ingest_failures`.

---

## 7. Deliberate Deltas vs Kafka / MQTT

1. **No `MessageFormat`.** OPC-UA is a typed wire protocol — a Variant
   has a concrete type tag. There is no JSON / binary / human-JSON
   branching on the decode path.
2. **No top-level `symbol_map`.** Each `OpcUaNodeMap` entry binds one
   NodeId to one SymbolId with its own `value_scale`. This is denser for
   typical OPC-UA address spaces (hundreds of nodes, each with different
   engineering units) than Kafka's single map.
3. **Subscription params replace QoS / commit-mode.** OPC-UA subscribes
   at broker-chosen intervals (`publishing_interval_ms`) and samples each
   node at its own rate (`sampling_interval_ms`); backpressure is
   expressed via `queue_size` + `discard_oldest` at the server side, not
   the client side.
4. **No decoder thread.** Like MQTT's Paho callback thread, open62541
   owns its own `UA_Client_run_iterate()` loop; the consumer will hook
   into it rather than spinning its own poll thread (production-phase
   work).

---

## 8. Reuse from Kafka / MQTT

- **Backpressure retry helper** — local `try_with_backpressure()` lambda,
  byte-identical to `MqttConsumer`'s. Kept local rather than refactored
  out to stay minimal for the PoC.
- **Dispatch path** (`ingest_decoded`) — identical single-vs-multi-node
  branching, identical stats bookkeeping, identical `route(table_id,
  symbol_id)` resolution.
- **Table-aware ingest** (devlog 084) — same `config.table_name` →
  `SchemaRegistry::get_table_id()` resolution in `set_pipeline()`, same
  drop-with-`ingest_failures`-bump behaviour on unknown name.
- **License gate** — same `Feature::IOT_CONNECTORS` bit (the license
  validator comment already listed OPC-UA as a future user of this bit,
  so no license-bitmask change is required).
- **Optional-dep pattern** — `ZEPTO_USE_OPCUA` CMake option → `find_library`
  → sets `ZEPTO_OPCUA_AVAILABLE` preprocessor symbol. Without the dep
  `opcua_consumer.cpp` short-circuits `start()` to `false`; with the dep,
  the real `UA_Client` integration (devlog 106) runs.

---

## 9. Test Strategy

### PoC (this devlog — 22 tests in `tests/unit/test_opcua.cpp`)

- Variant coercion for all six MVP types (Int32, Int64 negative, Double
  with scale, Float with scale, Bool true/false, Unsupported).
- UA DateTime conversion (epoch boundary, 1 s after epoch, before-1970
  clamping).
- Security combo validation (None-always-valid, Sign-requires-policy,
  SignAndEncrypt-requires-policy).
- NodeId map hit / miss bookkeeping.
- `ingest_decoded` single-node dispatch, no-pipeline failure, multi-node
  backpressure exhaustion with counters.
- `start()` refuses without dep / with empty nodes.
- `stop()` is idempotent.
- `OpcUaStats` counters consistent across hit+miss scenarios.
- Config defaults.

### Integration (devlog 107, BACKLOG P9 #2k — shipped)

`tests/unit/test_opcua_integration.cpp` spins up open62541's bundled
tutorial-style server in-process, exposes a single `ns=1;s=the.answer`
Int32 node (value 42), then connects an `OpcUaConsumer` and asserts at
least one tick reaches the pipeline within 5 s (teardown under 10 s
wall-clock).

The entire file is wrapped in `#ifdef ZEPTO_OPCUA_AVAILABLE` — matching
the `ZEPTO_MQTT_AVAILABLE` / `ZEPTO_KAFKA_AVAILABLE` silent-optional-dep
pattern. **CI dependency:** install `open62541-devel` (RHEL / Amazon
Linux) or `libopen62541-dev` (Debian / Ubuntu) at configure time for
this test to run.  On hosts without the dep the TU compiles to nothing
and the test is silently absent.

---

## 10. How to Enable at Build Time

```bash
sudo dnf install -y open62541-devel    # RHEL / Amazon Linux
# or
sudo apt install -y libopen62541-dev   # Debian / Ubuntu

cmake -B build -GNinja -DZEPTO_USE_OPCUA=ON ..
ninja -C build zepto_tests
```

If the library is not installed at configure time, CMake emits:

```
-- open62541: not found — OPC-UA consumer disabled
--   Install: sudo dnf install -y open62541-devel
```

…and `ZEPTO_USE_OPCUA` is silently turned off — the PoC builds and tests
still pass.

---

## 11. Cross-References

- BACKLOG P9 entries 1–11 (production follow-ups)
- Devlog 081 (`MqttConsumer`) — identical pattern, two-of-three connector twins
- Devlog 082 (table-scoped partitioning) — provides `table_id` addressable routing
- Devlog 084 (table-aware ingest) — provides `table_name` → `table_id` resolution
- `docs/design/physical_ai_market.md` — business context for P9

---

## 12. Deployment Sizing (from PoC microbench, devlog 101)

Measured on the PoC hot path (single-thread, 500-node pool, `ZeptoPipeline`
defaults: batch=256, arena=32 MB):

- **Throughput**: 6.63 M ticks/s single-core (no routing, no UA_Client).
- **Latency**: p50 ≈ 1.08 µs, p99 ≈ 2.03 µs per `on_data_change()`.
- **Node map build**: 100 000 nodes → 18 ms (well under 100 ms budget).

Conservative real-world planning number with open62541 + SSL + OPC-UA
binary decode overhead: **~2.2 M ticks/s per connector core**.

| Sector | Rate | Topology | `queue_size` | `publishing_interval_ms` | `sampling_interval_ms` | `backpressure_retries` |
|---|---|---|---|---|---|---|
| Auto factory (Hyundai / Bosch) | 200 K/s | 1 consumer, 1 core (11× headroom) | 100 | 10 | 1 | 10 |
| Steel mill (POSCO) | 250 K/s | 1 consumer, 1 core (8.8× headroom) | 100 | 10 | 1 | 10 |
| Semiconductor fab (Samsung / SK / TSMC) | 5 M/s | 3–4 consumers sharded via `set_routing()`; or client-side batching in the UA_Client callback (BACKLOG P9 #2b) | 1000 | 10 | 0.1 | 20 |

Default `queue_size=10` in the PoC is tuned for a demo, not a fab: at
10 KHz sampling with a 100 ms publishing interval, 1 000 samples queue
per node per publish cycle — a 10-slot queue with `discard_oldest=true`
silently drops 99 % of spikes. Sector-aware profiles are tracked as
BACKLOG P9 #2q.

---

## 13. Known Limitations (PoC)

The edge-case sweep in devlog 101 documents latent bugs left unfixed so
they surface as flip-target assertions for the production work. The
2026-04 pre-2b pass (devlog 105) closed the two blocker rows below:

| Test | Issue | BACKLOG | Impact |
|---|---|---|---|
| ~~`OpcUaEdgeCoerce.DoubleScaleOverflow`~~ | ~~`static_cast<int64_t>(double)` is UB on out-of-range values~~ | **P9 #2n — fixed (devlog 105)**, saturating clamp to `INT64_MIN`/`INT64_MAX` | — |
| ~~`OpcUaEdgeCoerce.DoubleNaNAndInf`~~ | ~~`NaN` / `+Inf` pass through as garbage~~ | **P9 #2n — fixed (devlog 105)**, `coerce_variant_to_int64` now returns `false` → caller bumps `decode_errors` | — |
| `OpcUaEdgeDatetime.FarFutureOverflow` | Signed multiply overflow in `ua_datetime_to_ns` for values approaching `INT64_MAX` | P9 #2m follow-up | Low impact — only affects dates > year 2287 |
| ~~`OpcUaEdgeConfig.DuplicateNodeIdFirstWinsSilently`~~ → `DuplicateNodeIdRejected` | ~~`emplace` on duplicate `node_id` silently drops the second entry~~ | **P9 #2p — fixed (devlog 105)**, `start()` rejects duplicates up-front | — |
| ~~`OpcUaEdgeConfig.EmptyNodeIdAccepted`~~ → `EmptyNodeIdRejected` | ~~Empty `node_id` string was accepted into the map~~ | **P9 #2p — fixed (devlog 105)**, `start()` rejects empty `node_id` | — |
| `OpcUaEdgeConcurrency.StatsUnderThreadedWrites` | `stats()` snapshot can tear across fields under 8-writer contention | P9 #2r | Non-blocker — acceptable for PoC dashboards, tighten for SLA reporting |

The real `UA_Client` integration (2b) **landed in devlog 106**. It
consumes the `connect_timeout_ms` / `session_timeout_ms` knobs added in
devlog 105 (#2o), and the sector profiles
(`OpcUaConfig::apply_profile` — #2q) are now exposed to pilot customers.
The reconnect loop (#2i) still consumes `reconnect_interval_ms` and is
Sprint-2 work.
