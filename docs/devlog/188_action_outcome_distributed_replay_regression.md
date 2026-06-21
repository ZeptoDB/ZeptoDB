# 188: Action-Outcome Distributed Replay Regression

Date: 2026-06-18
Status: Complete

## Context

Experiment 008 proved the Action-Outcome SQL seed across a live two-node
HTTP/RPC topology. The remaining risk was that this acceptance path lived in a
Python/live-process harness only. We needed a CI-sized C++ regression that
exercises the same brittle boundary: schema-aware `INSERT`, table-aware
owner routing, real `TYPED_ROW_INGEST` TCP RPC, remote owner storage, and a
coordinator `SELECT` that returns decoded `STRING` values.

The first C++ test attempt exposed a deeper issue. Local HTTP JSON could decode
remote `STRING` codes when the coordinator still had the sender dictionary, but
the remote owner itself was storing only dictionary codes from the forwarding
node. After restart, failover, or a coordinator dictionary miss, those codes
could lose their semantic text.

## Changes

- `include/zeptodb/core/pipeline.h`
  - Added optional `TypedColumnValue::has_string_value` and `string_value` for
    `SYMBOL`/`STRING` values crossing RPC boundaries.

- `include/zeptodb/storage/string_dictionary.h`
  - Added `ensure_code()` so replicated typed-row ingest can bind a sender
    dictionary code to the original string text and reject inconsistent code/text
    mappings.

- `src/sql/executor.cpp`
  - SQL `INSERT` materialization now preserves original text for
    `SYMBOL`/`STRING` values in the `TypedRowMessage`.

- `src/core/pipeline.cpp`
  - `ZeptoPipeline::ingest_typed_row()` binds replicated string codes before
    appending values to owner partitions.

- `include/zeptodb/cluster/rpc_protocol.h`
  - `TYPED_ROW_INGEST` serialization now includes a backwards-compatible
    optional string payload tail by column index.
  - `QueryResultSet` serialization now decodes both `SYMBOL` and `STRING`
    columns into the optional remote string tail.

- `include/zeptodb/cluster/partial_agg.h`
  - Distributed concat merge now preserves and reconstructs decoded
    `SYMBOL`/`STRING` cells.

- `src/server/http_server.cpp`
  - JSON response construction consumes `string_rows` for both `SYMBOL` and
    `STRING` columns.

- Tests:
  - Added
    `DistributedInsert.CoordinatorAdapterRoutesTypedRowsOverRpcAndDecodesRemoteStrings`.
  - Added `RpcProtocol.RoundTripPreservesTypedRowStringPayload`.
  - Updated the distributed INSERT stub to account for the typed-row path.

- Docs:
  - Updated `docs/design/phase_c_distributed.md`.
  - Updated `docs/api/CPP_REFERENCE.md`.
  - Updated research and backlog/completed tracking.

## Verification

Build:

```bash
cd build
ninja -j$(nproc) zepto_tests
ninja -j$(nproc) zepto_http_server
```

Focused tests:

```bash
cd build
./tests/zepto_tests \
  --gtest_filter='RpcProtocol.RoundTripPreservesTypedRowStringPayload:DistributedInsert.CoordinatorAdapterRoutesTypedRowsOverRpcAndDecodesRemoteStrings'

./tests/zepto_tests \
  --gtest_filter='RpcProtocol.RoundTripPreservesSymbolStrings:RpcProtocol.RoundTripPreservesTypedRowStringPayload:PartialAgg.MergeConcatPreservesSymbolStrings:DistributedInsert.*'
```

Results:

- 2/2 new focused tests passed.
- 8/8 related protocol/partial-agg/distributed-insert tests passed.
- Full local C++ suite passed: 1508 passed, 1 skipped live S3 opt-in,
  3 disabled.

aarch64 verification was not run in this session.

## Follow-ups

- Add JOIN/window replay acceptance over `action_outcome_episodes` and
  `action_outcome_replay_recommendations`.
- Define a shard-key policy for symbol-less operational/Action-Outcome tables
  before promoting distributed replay beyond research.
