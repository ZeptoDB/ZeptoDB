# 147: Arrow IPC Ingest Endpoint

Date: 2026-05-31
Status: Complete

## Context

The P4 backlog called for a first-class binary columnar ingest path to remove
the per-row SQL/JSON overhead from HTTP tick ingestion. ZeptoDB already had
Arrow IPC query responses and Python Arrow/DataFrame ingest, but remote HTTP
clients still needed to send SQL `INSERT` strings.

## Changes

- `include/zeptodb/server/arrow_ipc.h` and `src/server/arrow_ipc.cpp` now expose
  `ArrowIpcIngestOptions`, `ArrowIpcIngestResult`, and
  `ingest_arrow_ipc_stream()`.
- `POST /insert/arrow` accepts Arrow IPC RecordBatchStream bytes and maps
  configured columns into `TickMessage` rows:
  - `sym` by default, with `symbol` accepted as the default alias.
  - `price` and `volume` required, converted to int64 after
    `price_scale` / `volume_scale`.
  - `timestamp` optional; Arrow timestamp arrays are converted to ns, and a
    missing timestamp column receives ingest-time ns stamps.
  - `msg_type` optional, defaulting to trade (`0`).
- String symbol columns are interned through `QueryExecutor`, while integer
  symbol columns are accepted as direct symbol ids.
- `QueryExecutor::ingest_tick_batch()` provides the shared direct-ingest path
  for decoded tick batches. It resolves table_id, preserves the existing
  cluster routing hook, drains synchronously for read-after-write behavior,
  and marks destination schemas as having data.
- The HTTP route enforces table ACLs and tenant namespace rules before decode.
  ACL-restricted requests must provide `table=...` so authorization can be
  checked unambiguously.
- Builds without Arrow support return `406 Not Acceptable`, matching the
  Arrow IPC query response behavior.
- Documentation now covers the endpoint in `docs/api/HTTP_REFERENCE.md`, the
  Layer 2 data-flow note, `COMPLETED.md`, and `BACKLOG.md`.

## Verification

- `cmake --build build --target zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='*ArrowIpc*:*HttpArrow*'`
  - 14/14 passed.
  - New coverage includes successful string-symbol ingest, generated
    timestamps when the Arrow table has no timestamp column, empty batch
    no-op, malformed IPC handling, missing required column validation, and
    unknown-table rejection before claiming inserted rows.

## Follow-ups

- Add a symbol-aware batched HTTP client/benchmark that posts Arrow IPC payloads
  instead of SQL INSERT strings.
- Keep MessagePack columnar ingest as the smaller Telegraf/Influx-adjacent
  companion wire format in P4/P5.
