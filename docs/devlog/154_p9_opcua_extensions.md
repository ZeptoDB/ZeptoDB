# 154 — P9 OPC-UA Extensions

**Date:** 2026-06-02
**Scope:** Remaining OPC-UA production-profile contracts and factory
competitor benchmark harness.

## What Shipped

### OPC-UA Production Profiles

`OpcUaConsumer` now exposes public, testable hooks for the remaining OPC-UA
profile shapes:

- `on_array_change()` expands array elements to `symbol_id + index *
  array_symbol_stride`.
- `on_string_change()` maps UA String payloads to dictionary/symbol codes.
- `on_structured_change()` dispatches explicit Structured fields with per-field
  scale and engineering-unit metadata at the connector boundary.
- `ingest_history()` replays Historical Access samples through the normal
  NodeId map, quality policy, and routing path.
- `on_alarm_event()` emits Alarms & Conditions events as a dedicated tick
  stream, with active alarms stored as positive severity and cleared alarms as
  negative severity.

The live open62541 data-change bridge now forwards numeric arrays and scalar
strings into those same hooks when `ZEPTO_USE_OPCUA=ON` is available.

### Browse CLI

`zepto-opcua-browse` is now a build target. In default builds it compiles as a
diagnostic stub and exits clearly when open62541 support is unavailable. In
`ZEPTO_USE_OPCUA=ON` builds it can connect to a live server, browse from a root
NodeId, and emit `OpcUaConfig::nodes` JSON or CSV with generated symbol ids.

### Factory 10 KHz Harness

`tools/run-factory-10khz-competitor-bench.sh` standardizes the P9 smart-factory
proof run. It runs ZeptoDB via `bench_ingest_scale`, accepts InfluxDB and
TimescaleDB workload commands through environment variables, records pass /
fail / skipped status in JSONL, and can fail fast with `--require-competitors`
when a sales-proof run is missing an external competitor.

## Verification

```bash
cmake --build build --target zepto_tests zepto-opcua-browse -j$(nproc)
./build/tests/zepto_tests --gtest_filter='OpcUa*'
./build/tests/zepto_tests --gtest_filter='OpcUaArrayVariant.*:OpcUaStructuredVariant.*:OpcUaStringVariant.*:OpcUaHistoricalAccess.*:OpcUaAlarmsConditions.*:OpcUaUnsupportedVariant.*'
./build/zepto-opcua-browse --endpoint opc.tcp://127.0.0.1:4840
tools/run-factory-10khz-competitor-bench.sh --help
```

The targeted OPC-UA suite passed 58/58. The browse CLI default-build diagnostic
returned the expected exit code 2 because open62541 is not installed/enabled in
this environment. The full C++ suite passed with 1420 tests run, 1419 passed,
and one opt-in live S3 upload test skipped.

## P9 Closeout Status

- Live open62541 HistoryRead adapter and ZeptoDB-as-OPC-UA-server mode were
  closed by devlog 155.
- Actual external factory 10 KHz benchmark execution against InfluxDB and
  TimescaleDB deployments was closed by devlog 159.
