# Devlog 107 — OPC-UA Sprint 2 Stage 1: quality/StatusCode mapping + live integration test

**Date:** 2026-04-26
**Sprint:** Sprint 2 — Stage 1 (no dependencies)
**Scope:** BACKLOG P9 #2j (quality mapping) + #2k (open62541 integration test)
**Build flag:** `ZEPTO_USE_OPCUA=OFF` (default) unaffected except for one documented TickMessage.volume default change; `ZEPTO_USE_OPCUA=ON` gains StatusCode-aware dispatch and a live end-to-end test.

---

## 1. Why

Sprint 1 (devlogs 105 & 106) wired a real `UA_Client` session and a working
data-change callback, but it ignored `UA_DataValue.status` entirely —
every sample, Good or Bad, landed with `TickMessage.volume = 0`. Industrial
operators routinely write queries like _"only GOOD-quality samples for the
last hour"_; without a quality column those queries are impossible.

A live integration test against open62541's bundled tutorial server was
also still deferred from Sprint 1. With the `UA_Client` flow now stable,
that test is cheap to add and catches any future regression in the
trampoline / subscription / variant-coercion path in CI.

## 2. What changed

### 2j — StatusCode → quality mapping

- `include/zeptodb/feeds/opcua_consumer.h`
  - New enum `OpcUaConfig::QualityHandling { IgnoreBad, AcceptAll,
    AcceptAllGoodAs1 }`.
  - New field `OpcUaConfig::quality_handling = AcceptAllGoodAs1` (default).
  - `on_data_change(...)` gains a trailing `int64_t status = 0` parameter.
    Default preserves the entire pre-existing call graph — every current
    unit test compiles and runs unchanged.

- `src/feeds/opcua_consumer.cpp`
  - `on_data_change` applies the quality switch **before** bumping
    `messages_consumed`, so `IgnoreBad` drops cleanly (decode_errors++
    only, no ghost `messages_consumed` bump).
  - `AcceptAll` stamps the raw 32-bit status into `TickMessage.volume`.
  - `AcceptAllGoodAs1` stamps `1` if GOOD (status == 0), `0` otherwise —
    the default, giving downstream SQL a trivially-filterable quality bit.
  - The file-local `handle_data_change` (static trampoline bridge) now
    reads `value->hasStatus ? value->status : 0` and forwards it as the
    new fourth argument of `on_data_change`. The Sprint-1 reviewer flagged
    the "piggy-back via empty node_id" for unsupported-variant
    `decode_errors++` as subtle; that path is left intact (it is still the
    most minimal way to keep the increment inside the mutex-protected
    member method), but the inline comment above it no longer calls it a
    piggy-back — it now states plainly that the empty-node_id → map-miss
    branch is the explicit decode_error path.

### 2k — live integration test

- `tests/unit/test_opcua_integration.cpp` (new).
  Wraps the entire TU in `#ifdef ZEPTO_OPCUA_AVAILABLE`, matching the
  optional-dep pattern used by `ZEPTO_MQTT_AVAILABLE` /
  `ZEPTO_KAFKA_AVAILABLE` tests — when open62541 is not installed at
  configure time, the file compiles to nothing and the test is silently
  absent.

  The test:
  1. Starts an in-process open62541 server in a background thread that
     exposes a single `ns=1;s=the.answer` Int32 node (value 42), mirroring
     the upstream `tutorial_server_variable` example.
  2. Loads an all-features Enterprise license (bit 8 =
     `Feature::IOT_CONNECTORS` required).
  3. Builds an `OpcUaConsumer`, `set_pipeline`, `start()`.
  4. Polls `stats().messages_consumed` up to 5 s.
  5. Asserts at least one tick arrived and tears down in well under
     10 s wall-clock.

- `tests/CMakeLists.txt`: added `unit/test_opcua_integration.cpp` next to
  `unit/test_opcua.cpp`. The test binary already links `zepto_opcua`,
  which PUBLIC-exports `ZEPTO_OPCUA_AVAILABLE=1` and links
  `${OPEN62541_LIB}` when the dep is found, so no further wiring is
  needed.

### CI dependency for 2k

This test requires `open62541-devel` (RHEL / Amazon Linux) or
`libopen62541-dev` (Debian / Ubuntu) at **compile** time. On this bench
host the package is not installed, so per the `ZEPTO_USE_OPCUA=OFF`
default the file silently compiles to nothing (verified — zero
link-time errors, test count unchanged at 1275).  **CI must install
the dev package for this test to run.**

## 3. Default-build behaviour delta (explicit call-out)

**`ZEPTO_USE_OPCUA=OFF` build (default):** no runtime delta — no OPC-UA
callbacks fire, so the new `QualityHandling` default is inert for every
pre-existing test and every downstream caller.

**`ZEPTO_USE_OPCUA=ON` build:** **one** observable behaviour change — the
new `QualityHandling` default of `AcceptAllGoodAs1` sets
`TickMessage.volume = 1` for every successful OPC-UA dispatch that flows
through `on_data_change`. Previously that field was hard-coded to `0`.
This only affects the OPC-UA path; MQTT and Kafka consumers are
untouched. Nothing in the existing unit-test suite asserts on
`msg.volume` from OPC-UA dispatches, so the change is inert for all
pre-existing tests and all downstream callers.

## 4. Tests added

| Suite | Case | Purpose |
|---|---|---|
| `OpcUaQuality` | `AcceptAllGoodAs1_GoodStatusVolume1` | default profile, GOOD → volume=1, explicit new signature |
| `OpcUaQuality` | `AcceptAllGoodAs1_BadStatusVolume0` | default profile, Bad code → volume=0, still dispatches |
| `OpcUaQuality` | `AcceptAll_RawStatusInVolume` | raw 32-bit status preserved in both Good and Bad cases |
| `OpcUaQuality` | `IgnoreBad_GoodDispatches` | IgnoreBad lets GOOD through |
| `OpcUaQuality` | `IgnoreBad_BadIncrementsDecodeErrors` | IgnoreBad + Bad → decode_errors++ **and** no `messages_consumed` bump (drop is clean) |
| `OpcUaIntegration` | `ConnectsAndReceivesTickFromLiveServer` | `#ifdef ZEPTO_OPCUA_AVAILABLE` only — end-to-end roundtrip against open62541 tutorial server |

## 5. Verification

```
$ cd build && ninja zepto_tests 2>&1 | tail -5
[3/5] Linking CXX static library libzepto_opcua.a
[4/5] Building CXX object tests/CMakeFiles/zepto_tests.dir/unit/test_opcua.cpp.o
[5/5] Linking CXX executable tests/zepto_tests

$ ./tests/zepto_tests --gtest_filter="OpcUa*" 2>&1 | tail -5
[----------] 5 tests from OpcUaQuality (1 ms total)
[==========] 43 tests from 17 test suites ran. (1376 ms total)
[  PASSED  ] 43 tests.

$ ./tests/zepto_tests 2>&1 | tail -5
[==========] 1275 tests from 173 test suites ran. (284667 ms total)
[  PASSED  ] 1275 tests.
```

Baseline after Sprint 1: 1270/1270 total, 38 `OpcUa*`.  After Stage 1:
1275/1275 (+5), 43 `OpcUa*` (+5).  The integration test is compiled out
on this host (no open62541-devel); it will contribute +1 to both counts
in CI once the dep is installed.

## 6. Files touched

| Path | Purpose |
|---|---|
| `include/zeptodb/feeds/opcua_consumer.h` | `QualityHandling` enum + `quality_handling` field + `on_data_change` signature |
| `src/feeds/opcua_consumer.cpp` | quality switch in `on_data_change`; status threaded through `handle_data_change` trampoline |
| `tests/unit/test_opcua.cpp` | 5 new `OpcUaQuality` tests appended at EOF |
| `tests/unit/test_opcua_integration.cpp` | **new** — `#ifdef`-guarded live roundtrip test |
| `tests/CMakeLists.txt` | added the new test file to `zepto_tests` sources |
| `docs/design/opcua_connector.md` | §4 data-model note on QualityHandling modes; §9 test strategy updated |
| `docs/BACKLOG.md` | Tier-2 #2j and #2k marked ✅ with devlog 107 cross-ref |
| `docs/devlog/107_opcua_sprint2_quality_and_integration.md` | this file |

`COMPLETED.md` is **intentionally not updated yet** — per the Sprint 2
plan, that happens in the combined closeout after all stages ship.

## 7. Follow-ups

- 2k is gated on CI having `open62541-devel` installed; track that in the
  deployment checklist when CI is first pointed at this test.
- `AcceptAll` encodes the status as the entire `volume` field, which
  collides with any future real "volume" semantic for OPC-UA. If/when 2d
  (structured variants) ships, promote quality to a dedicated column.
