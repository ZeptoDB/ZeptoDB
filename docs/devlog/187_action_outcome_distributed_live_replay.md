# 187: Action-Outcome Distributed Live Replay

Date: 2026-06-18
Status: Complete

## Context

Experiment 007 proved the Action-Outcome SQL replay seed on a single live
ZeptoDB endpoint. The next acceptance boundary was a two-node live topology:
DDL replication, schema-aware typed-row INSERT, table-aware owner routing,
remote typed-row RPC ingest, scatter-gather SELECT, and decoded string results
all had to work together.

The first distributed probe exposed two gaps:

- `CoordinatorRoutingAdapter` implemented only tick routing. Declared-schema
  INSERT uses `ClusterNodeBase::ingest_typed_row()`, so cluster-mode HTTP
  generic INSERT could fail closed or never exercise remote owners.
- Distributed concat SELECT over `STRING`/`SYMBOL` columns could return
  node-local dictionary codes because the RPC result payload did not carry
  decoded string cells and concat merge did not rebuild them.

## Changes

- `include/zeptodb/cluster/coordinator_routing_adapter.h`
  - Added `ingest_typed_row()` using the same `(table_id, symbol_id)` owner
    route as tick ingest.
  - Routes typed rows to the local pipeline or matching peer RPC client.

- `tools/zepto_http_server.cpp`
  - Registered a `TcpRpcServer::set_typed_row_ingest_callback()` in non-HA
    cluster mode so peer `TYPED_ROW_INGEST` messages land in the local
    pipeline.

- `include/zeptodb/cluster/rpc_protocol.h`
  - Extended `QueryResultSet` serialization with a backward-compatible optional
    decoded string tail for `SYMBOL` columns.
  - Deserialization preserves that tail in `QueryResultSet::string_rows`.

- `include/zeptodb/cluster/partial_agg.h`
  - `merge_concat_results()` now preserves remote `string_rows`.
  - For local results with a live `symbol_dict`, it reconstructs decoded string
    cells during concat merge.

- Tests:
  - Added CoordinatorRoutingAdapter typed-row local, remote, and unknown-owner
    coverage.
  - Added RPC round-trip coverage for decoded `SYMBOL` strings.
  - Added concat merge coverage for local dictionary strings plus remote string
    tails.

- Research:
  - Added
    `docs/research/tools/action_outcome_distributed_live_sql_replay.py`.
  - Added
    `docs/research/action_outcome_distributed_live_sql_replay_experiment_008.md`.
  - Generated
    `docs/research/results/action_outcome_distributed_live_sql_replay_008.md`.

- Design docs:
  - Updated `docs/design/phase_c_distributed.md` for typed-row routed INSERT
    and decoded string preservation in distributed concat results.

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
  --gtest_filter='RpcProtocol.RoundTripPreservesSymbolStrings:PartialAgg.MergeConcatPreservesSymbolStrings:CoordinatorRoutingAdapter.*Typed*'
```

Result:

- 5/5 focused tests passed.

Distributed live replay:

```bash
python3 docs/research/tools/action_outcome_distributed_live_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --sql-file docs/research/results/action_outcome_sql_replay_006.sql \
  --output docs/research/results/action_outcome_distributed_live_sql_replay_008.md
```

Result:

- Statements attempted: 203.
- Statements succeeded: 203.
- Row-count verification: pass.
- Semantic top-action replay: pass.
- Remote decoded string SELECT: pass.
- Node-local ingest deltas:
  - node 1: 140 rows, 3 partitions.
  - node 8: 58 rows, 2 partitions.

aarch64 verification was not run in this session.

## Follow-ups

- CI-sized C++ integration coverage for typed-row routed INSERT plus decoded
  `STRING` SELECT results landed in devlog 188.
- Add a shard-key policy for symbol-less operational tables so production
  two-node splits do not depend on node-id choice.
- Extend the Action-Outcome replay harness with JOIN/window queries across
  episodes and recommendation tables.
