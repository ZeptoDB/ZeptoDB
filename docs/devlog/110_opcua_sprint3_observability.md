# Devlog 110 — OPC-UA Sprint 3 (Tier-3 observability closeout)

**Date:** 2026-04-27
**Scope:** Tighten the OPC-UA connector's observability surface so it can
back SLA commitments. Ships BACKLOG P9 Tier-3 items **2r**, **2s**,
**2t**, plus three Sprint-2 polish items the reviewer flagged as
follow-ups. All items S-sized. `ZEPTO_USE_OPCUA=OFF` (default build):
**zero** runtime delta.

**Build flag:** `ZEPTO_USE_OPCUA=OFF` (default) unaffected;
`ZEPTO_USE_OPCUA=ON` gains one touched path (unsupported-variant
`decode_errors` is now bumped explicitly instead of via an
empty-node_id piggy-back).

---

## 1. Why

Sprint 2 closed out first-commercial-ready features (2c security, 2k
integration test, 2i reconnect, 2j quality mapping — devlogs 107-109).
Before signing SLAs (99.95 % ingest availability, latency percentiles)
the operator-facing surface has to be watertight:

* `stats()` snapshots must be self-consistent under contention (Grafana
  scrapes at 1 Hz; a torn snapshot produces misleading dashboards).
* Every stats counter must be unit-testable — the `route_remote` path
  was blocked behind a real TCP listener.
* Cross-connector microbench baselines must be apples-to-apples so the
  "OPC-UA is 2-3× faster than Kafka" claim in the sales deck is
  reproducible.

---

## 2. What changed

### 2r — Atomic stats snapshot (audit, no behaviour change)

Audited every multi-field stats transition in
`src/feeds/opcua_consumer.cpp`:

| Path | Transition | Locking |
|---|---|---|
| `ingest_decoded` local-no-pipeline | `ingest_failures++` | single field, one `lock_guard` |
| `ingest_decoded` remote-missing | `ingest_failures++` | single field, one `lock_guard` |
| `ingest_decoded` local success/fail | `route_local++` XOR `ingest_failures++` | single field, one `lock_guard` |
| `ingest_decoded` remote success/fail | `route_remote++` XOR `ingest_failures++` | single field, one `lock_guard` |
| `on_data_change` miss | `decode_errors++` | single field, one `lock_guard` |
| `on_data_change` IgnoreBad-non-good | `decode_errors++` | single field, one `lock_guard` |
| `on_data_change` pre-dispatch | `messages_consumed++; bytes_consumed++` | **two fields, one `lock_guard`** ✓ |
| `on_unsupported_variant` (new) | `decode_errors++` | single field, one `lock_guard` |
| `run_iterate_loop` reconnect | `reconnects++` | single field, one `lock_guard` |

**Finding:** no torn-read window exists. Every outcome path updates
exactly one field under one lock, so a reader's `stats()` snapshot is
always self-consistent. The apparent split between `messages_consumed++`
(pre-dispatch) and `route_{local,remote}++ XOR ingest_failures++`
(post-dispatch) is semantic — "attempts started" vs "attempts finished"
— not a torn transition. The existing Sprint-1 test
`OpcUaEdgeConcurrency.StatsUnderThreadedWrites` already asserts
`messages_consumed == route_local + ingest_failures` at quiescence,
which would flag any real torn window.

2r is therefore a **documentation-only** item. Added an audit comment
block above `ingest_decoded` in `src/feeds/opcua_consumer.cpp`
explicitly stating the invariant. No new test — the existing Sprint-1
concurrency test already covers the post-quiescence invariant, and
tightening it to assert mid-flight reader snapshots would replicate the
producer-consumer semantics we already document.

### 2s — `RpcClientBase` extraction (unit-testable remote dispatch)

Mirrors the Sprint-1 `ClusterNodeBase` pattern (devlog 103).

**New:** `include/zeptodb/cluster/rpc_client_base.h` — 24-line abstract
base declaring `bool ingest_tick(const TickMessage&)`. No dependencies
beyond `tick_plant.h`.

**Changed:**

* `TcpRpcClient` now inherits `RpcClientBase`; `ingest_tick` marked
  `override`. Signature unchanged (`const TickMessage&`).
* `remotes_` map in all three consumers (`OpcUaConsumer`,
  `MqttConsumer`, `KafkaConsumer`) now typed
  `shared_ptr<RpcClientBase>` instead of `shared_ptr<TcpRpcClient>`.
  Purely a type-widening change — existing call sites
  (`it->second->ingest_tick(msg)`) work unchanged via virtual dispatch.
* Every consumer header swaps `#include
  "zeptodb/cluster/tcp_rpc.h"` for
  `#include "zeptodb/cluster/rpc_client_base.h"` — lighter include
  graph for downstream consumers of these headers.

**Tests added in `tests/unit/test_opcua.cpp`:**

* `OpcUaRouting.RouteRemote_IncrementsOnSuccessfulDispatch` — stub
  `CountingRpcClient` returns `true`; 200 ticks routed through a
  two-node ring (one remote); asserts `route_remote > 0`,
  `route_remote == stub calls`, and `ingest_failures == 0`.
* `OpcUaRouting.RouteRemote_DoesNotIncrementOnFailedDispatch` — stub
  returns `false`; asserts `route_remote == 0`, `ingest_failures > 0`,
  and `stub->calls > 0` (so the stub was genuinely invoked, not
  bypassed).

Kafka / MQTT equivalents are **deliberately deferred** — the 80 %
value of 2s is the base-class extraction, which also unblocks the
other two consumers; the extra test pairs can be added later without
any architecture change.

### 2t — Cross-connector microbench parity

Added `DISABLED_` perf harnesses to `test_kafka.cpp` and
`test_mqtt.cpp`, copying the exact shape of
`OpcUaPerf.DISABLED_SingleThreadHotPath`:

1. Real `ZeptoPipeline` (PURE_IN_MEMORY defaults)
2. `set_pipeline()` only (no routing)
3. Pass 1: 1 M `on_message()` calls with 500-symbol cycled payload pool
   → wall-clock throughput
4. Pass 2: 100 K per-call chrono samples → p50 / p99 nanosecond latency
5. Pass 3: fresh pipeline, 50 K calls (below TickPlant 65 K capacity)
   → pure cheap-path throughput (no store_tick fallback)

**Results on this host (x86_64, single-thread hot path):**

| Connector | pass1 throughput | pass2 p50 | pass2 p99 | pass3 (cheap) |
|-----------|-----------------:|----------:|----------:|--------------:|
| Kafka     |    183 K ticks/s |   1605 ns |   2286 ns |  2.69 M ticks/s |
| MQTT      |    182 K ticks/s |   1633 ns |   2502 ns |  2.48 M ticks/s |
| OPC-UA    |    192 K ticks/s |   1230 ns |   2084 ns |  6.69 M ticks/s |

Pass 1 is dominated by TickPlant queue saturation (all three drop ~934
K of the 1 M ticks once the 65 K ring fills) — it measures the
end-to-end hot path including backpressure-exhaustion handling.
Pass 3 is the apples-to-apples decode+dispatch comparison:

* OPC-UA is ~2.5× faster than Kafka/MQTT on the cheap path because it
  skips JSON parsing — the NodeId → SymbolId map is a direct
  `unordered_map` lookup, whereas the JSON_HUMAN path through
  `KafkaConsumer::decode_json_human` tokenises and allocates per call.
* Kafka and MQTT are within noise of each other (same underlying
  decoder).

These numbers are now directly comparable with any future
OPC-UA-vs-Kafka deck claim.

### Polish 1 — devlog-107 §3 wording

Split into explicit `ZEPTO_USE_OPCUA=OFF` (no runtime delta) and
`ZEPTO_USE_OPCUA=ON` (volume=1 for GOOD samples) subsections. Previous
phrasing incorrectly implied OFF builds had a behaviour change.

### Polish 2 — Explicit `decode_errors` on unsupported variant

`src/feeds/opcua_consumer.cpp::handle_data_change` previously bumped
`decode_errors` for an unsupported UA variant type by calling
`self->on_data_change(std::string{}, 0, 0, 0)` — piggy-backing on the
empty-node_id map-miss branch. Two different error semantics sharing
one code path made the counter ambiguous in traces.

**New:** public `OpcUaConsumer::on_unsupported_variant()` — one lock,
one field, no dispatch. `handle_data_change` now calls it directly.
Also directly testable without the open62541 dep.

**Test added:** `OpcUaUnsupportedVariant.ExplicitDecodeErrorsIncrement`
— calls `on_unsupported_variant()`; asserts `decode_errors == 1`,
`messages_consumed == 0`, `route_local == 0`, `route_remote == 0`.

### Polish 3 — Reconnect test comment

Added a 3-line clarifying NOTE to
`tests/unit/test_opcua.cpp::OpcUaReconnect.BackoffCappedAtConfigMultiple`
— this test covers only the saturation invariant (clamp at 32× base);
live-disconnect sequence coverage is deferred to a future sprint.

---

## 3. Files changed

* **New:** `include/zeptodb/cluster/rpc_client_base.h`
* Edited: `include/zeptodb/cluster/tcp_rpc.h` — inherit `RpcClientBase`,
  `override` on `ingest_tick`
* Edited: `include/zeptodb/feeds/{opcua,mqtt,kafka}_consumer.h` —
  `remotes_` map widened to `shared_ptr<RpcClientBase>`; include swap
* Edited: `src/feeds/{opcua,mqtt,kafka}_consumer.cpp` —
  `set_routing` signature (parameter type only)
* Edited: `src/feeds/opcua_consumer.cpp` — atomicity audit comment;
  new `on_unsupported_variant()`; `handle_data_change` calls it
* Edited: `include/zeptodb/feeds/opcua_consumer.h` — declare
  `on_unsupported_variant()`
* Edited: `tests/unit/test_opcua.cpp` — 3 new tests (2 routing, 1
  unsupported variant); polish-3 comment
* Edited: `tests/unit/test_kafka.cpp` — `KafkaPerf.DISABLED_SingleThreadHotPath`
* Edited: `tests/unit/test_mqtt.cpp` — `MqttPerf.DISABLED_SingleThreadHotPath`
* Edited: `docs/devlog/107_opcua_sprint2_quality_and_integration.md` —
  polish 1
* Edited: `docs/BACKLOG.md` — Tier 3 marked shipped; P9 count 18→15
* Edited: `docs/COMPLETED.md` — Sprint 3 entry

---

## 4. Verification

```
$ cd build && ninja zepto_tests 2>&1 | tail -5
[1/3] Building CXX object tests/CMakeFiles/zepto_tests.dir/unit/test_kafka.cpp.o
[2/3] Building CXX object tests/CMakeFiles/zepto_tests.dir/unit/test_mqtt.cpp.o
[3/3] Linking CXX executable tests/zepto_tests

$ ./tests/zepto_tests --gtest_filter="OpcUa*:KafkaPerf*:MqttPerf*" 2>&1 | tail -3
[==========] 52 tests from 20 test suites ran.
[  PASSED  ] 52 tests.
  YOU HAVE 3 DISABLED TESTS

$ ./tests/zepto_tests 2>&1 | tail -3
[==========] 1284 tests from 176 test suites ran.
[  PASSED  ] 1284 tests.
  YOU HAVE 3 DISABLED TESTS
```

* Baseline 1281 + 2 (2s routing) + 1 (polish 2 unsupported-variant) =
  **1284 tests** ✓
* OPC-UA test count: 49 + 3 = **52** ✓
* **2 new disabled perf tests** (Kafka, MQTT) visible but not counted
  in PASSED ✓
* Full suite: **1284 / 1284 passed**, no regressions.

```
$ ./tests/zepto_tests --gtest_also_run_disabled_tests --gtest_filter="*Perf.DISABLED_SingleThreadHotPath*" 2>&1 | grep -E "\[.*Perf\]"
[KafkaPerf] pass1 wall=5470686 us  throughput=182792 ticks/s  ok=65536 failures=934464  | pass2 p50=1605 ns  p99=2286 ns
[KafkaPerf] pass3 (cheap-path only) wall=18614 us  throughput=2686150 ticks/s  ok=50000
[MqttPerf]  pass1 wall=5492158 us  throughput=182078 ticks/s  ok=65536 failures=934464  | pass2 p50=1633 ns  p99=2502 ns
[MqttPerf]  pass3 (cheap-path only) wall=20189 us  throughput=2476596 ticks/s  ok=50000
[OpcUaPerf] pass1 wall=5209756 us  throughput=191948 ticks/s  ok=65536 failures=934464  | pass2 p50=1230 ns  p99=2084 ns
[OpcUaPerf] pass3 (cheap-path only) wall=7473 us  throughput=6690753 ticks/s  ok=50000
```

---

## 5. Status

* OPC-UA Tier 3 (2r / 2s / 2t) — all shipped.
* Sprint-2 polish follow-ups (1 / 2 / 3) — all closed.
* Connector is now SLA-grade: unit-testable route_remote path, audited
  stats atomicity, cross-connector benchmark parity, clean
  `decode_errors` semantics. Remaining OPC-UA items (2d structured
  variants, 2e strings, 2f browse CLI, 2g HA, 2h A&C, 2l server mode)
  are Tier-4+ DX/advanced-data work unblocking future sectors, not SLA
  blockers.

**Next available devlog number:** `111_*.md`.
