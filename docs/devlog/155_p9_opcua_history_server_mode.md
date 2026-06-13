# 155 — P9 OPC-UA HistoryRead and Server Mode

**Date:** 2026-06-02
**Scope:** Close P9 OPC-UA live Historical Access and ZeptoDB-as-OPC-UA-server
mode.

## What Shipped

### Live Historical Access

`OpcUaConsumer::read_history()` reads server-side historian samples through
open62541 `UA_Client_HistoryRead_raw` when the library is compiled with
`UA_ENABLE_HISTORIZING`. Returned `UA_HistoryData` values reuse the existing
subscription decode path, so scalar/string/array handling, quality policy,
stats, backpressure, and local/remote routing stay identical between live
subscriptions and historical backfills.

Default builds without open62541 or historizing support fail closed: the method
returns `0`, increments `ingest_failures`, and logs a diagnostic.

### OPC-UA Server Mode

`OpcUaServer` exposes configured ZeptoDB symbols as OPC-UA Int64 variable
nodes. `publish_value()` updates the local snapshot in all builds and writes
the live OPC-UA node value when open62541 server support is compiled in. The
server wrapper validates empty/duplicate config, creates variable nodes under
Objects, runs the open62541 iterate loop on a background thread, and shuts down
cleanly through `UA_Server_run_shutdown`.

## Verification

```bash
cmake --build build --target zepto_tests zepto-opcua-browse -j$(nproc)
./build/tests/zepto_tests --gtest_filter='OpcUaUaDatetime.*:OpcUaHistoricalAccess.*:OpcUaServerMode.*:OpcUaArrayVariant.*:OpcUaStructuredVariant.*:OpcUaStringVariant.*:OpcUaAlarmsConditions.*'
./build/tests/zepto_tests --gtest_filter='OpcUa*'
./build/tests/zepto_tests
git diff --check
```

Focused HA/server-mode coverage passed 16/16. The full `OpcUa*` suite passed
65/65 in the default no-open62541 build. The full C++ suite passed with 1427
tests run, 1426 passed, and one opt-in live S3 upload test skipped.

`ZEPTO_USE_OPCUA=ON` configure was also attempted in `/tmp`; this Amazon Linux
image does not provide `open62541-devel`, so CMake correctly reported
`open62541: not found` and kept the fail-closed no-open62541 build. Live-linked
runtime validation still requires a host image with open62541 built with
`UA_ENABLE_HISTORIZING`.

## P9 Closeout Status

- Actual external factory 10 KHz benchmark execution against InfluxDB and
  TimescaleDB deployments was closed by devlog 159.
