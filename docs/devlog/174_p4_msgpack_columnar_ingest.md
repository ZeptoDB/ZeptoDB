# 174: P4 MessagePack Columnar Ingest

Date: 2026-06-11
Status: Complete

## Context

P4 Tool Integration still needed a smaller binary ingest surface than Arrow IPC
for Telegraf-style and embedded clients. The goal was a dependency-light batch
format that avoids per-row SQL while preserving ZeptoDB's table-aware ingest
semantics.

## Changes

- Added `POST /insert/msgpack` to the HTTP server.
- Added a dependency-free MessagePack columnar decoder for maps of column arrays.
- Supports configurable `sym`, `price`, `volume`, `timestamp`, and `msg_type`
  columns plus price and volume scales.
- Reuses `QueryExecutor::ingest_tick_batch()` so table-id resolution, cluster
  routing, synchronous drain, and schema `has_data` updates match Arrow IPC
  ingest.
- Enforces the same table ACL and tenant namespace checks as the Arrow ingest
  endpoint.
- Documented the endpoint in the HTTP API reference and layer 2 ingestion
  design doc.

## Verification

- `cmake --build build -j$(nproc) --target zepto_tests` — PASS.
- `./build/tests/zepto_tests --gtest_filter='HttpMsgpackIngestTest.*'` —
  PASS, 10 tests.
- `./build/tests/zepto_tests --gtest_filter='HttpArrowIpcTest.*:HttpArrowIpcEncoder.*:HttpArrowIpcIngest.*'`
  — PASS, 14 tests.
- Added focused HTTP tests for string symbols, default symbol aliases, missing
  timestamps, empty batches, missing columns, column length mismatches, malformed
  payloads, oversized declared arrays, unknown tables, table ACL denial, and
  tenant namespace denial.
- aarch64 / Graviton verification not run for this endpoint change.

## Follow-ups

- P4 next priority is the ClickHouse wire protocol.
- Telegraf output can migrate from SQL-over-HTTP to `POST /insert/msgpack` in a
  future P5 transport update.
