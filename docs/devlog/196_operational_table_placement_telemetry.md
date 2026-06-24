# 196: Operational Table Placement Policy And Telemetry

Date: 2026-06-21
Status: Experimental validation complete
Classification: Experimental runtime path

## Context

Experiment 011 proved that bounded small-table Action-Outcome JOINs can pass
across node owners without a full distributed SQL optimizer. The next risk was
operational: symbol-less control tables still depended on stable table-id plus
`symbol_id=0` routing. That made placement predictable but implicit, and it
left operators without telemetry for small-table JOIN row-cap behavior.

## Changes

- `include/zeptodb/cluster/partition_router.h`
  - Added `TablePlacementPolicy` and `TablePlacement`.
  - Added table-level placement overrides for default table+symbol hashing,
    `hash_by_table`, and `pinned_node`.
  - Made table placement participate in table-scoped `route(table_id, symbol)`.
- `include/zeptodb/cluster/query_coordinator.h`
  - Added public placement APIs and `SmallTableJoinTelemetrySnapshot`.
  - Added reset/snapshot helpers for focused tests and replay harnesses.
- `src/cluster/query_coordinator.cpp`
  - Resolves placement updates through the local schema snapshot.
  - Routes declared symbol-less INSERTs by `(table_id, 0)` so placement policy
    applies when the coordinator is used directly.
  - Records bounded small-table JOIN candidates, accepted joins, row-cap
    rejections, non-cap errors, materialized rows, and last-side row counts.
- `src/server/http_server.cpp`
  - Added `POST /admin/table-placement`.
  - Added `small_table_join` to `/stats` when a coordinator is wired.
  - Added Prometheus metrics for the small-table JOIN telemetry counters and
    gauges.
- `tests/unit/test_distributed_insert.cpp`
  - Added `OperationalTablePlacementPolicyPinsSymbollessTable`.
  - Extended the small-table JOIN regressions with telemetry assertions.
- `docs/research/tools/action_outcome_operational_placement_experiment.py`
  - Added Experiment 012: creates vendor tables, applies explicit placement,
    materializes Action-Outcome data, verifies strict distributed SQL/JOIN/window
    replay, and records `/stats` plus Prometheus telemetry.

## Experimental Boundary

This change validates a narrow runtime path for small operational/control
tables. It is not yet a promoted product placement feature.

Intended scope:

- Declared operational tables whose rows often use `symbol_id=0`.
- Small Action-Outcome/control tables that fit inside the bounded small-table
  JOIN row cap.
- Admin-driven placement validation and replay harnesses.

Non-goals:

- No general distributed SQL optimizer.
- No large cross-node hash JOIN guarantee.
- No persistent catalog/DDL placement metadata.
- No rebalance, failover, or rolling-upgrade semantics for placement overrides.

Guardrails and observability:

- Bounded small-table JOIN enforces the coordinator row cap before temporary
  materialization.
- `/stats` and Prometheus expose candidates, accepted joins, row-cap
  rejections, errors, materialized rows, and last-side row counts.
- Placement updates require the admin HTTP path or explicit C++ coordinator
  calls.

Product promotion requires:

- Persisted placement metadata in DDL, catalog state, or documented config.
- Explicit restart, rebalance, and node replacement semantics.
- A decision on whether bounded small-table JOIN remains automatic, becomes a
  feature flag, or moves behind an optimizer/cost rule.
- Operations guidance for row-cap alerts and failed/rejected bounded JOINs.

## Verification

```bash
ninja -C build -j$(nproc) zepto_tests zepto_http_server

./build/tests/zepto_tests \
  --gtest_filter='DistributedInsert.OperationalTablePlacementPolicyPinsSymbollessTable:DistributedInsert.SmallTableBroadcastJoinMaterializesCrossNodeOperationalTables:DistributedInsert.SmallTableBroadcastJoinRejectsRowsOverLimit'

python3 -m py_compile \
  docs/research/tools/action_outcome_vendor_sql_replay.py \
  docs/research/tools/action_outcome_operational_placement_experiment.py \
  docs/research/tools/action_outcome_distributed_vendor_sql_replay.py

python3 docs/research/tools/action_outcome_operational_placement_experiment.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --metrics-url http://127.0.0.1:19241/metrics \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_operational_placement_012.md \
  --timeout 10
```

Result summary:

| Check | Status |
| --- | --- |
| Focused C++ regression set | 3/3 pass |
| Explicit placement policy | pass |
| Seed row counts | pass |
| Vendor table counts | pass |
| Distributed ingest | pass |
| Full SQL/JOIN/window replay | pass |
| Small-table JOIN telemetry | pass |
| Prometheus telemetry | pass |
| Experiment 012 overall | pass |

Experiment 012 observed node-local ingest deltas of 161 rows on node 1 and 208
rows on node 8. Small-table JOIN telemetry recorded 4 candidates, 4 accepted
joins, 0 row-cap rejections, 0 errors, and 327 materialized rows.

## Follow-ups

- Promote runtime placement overrides to a persisted catalog or DDL table
  option.
- Add an operations example or alert for
  `zepto_small_table_join_row_cap_rejections_total`.
- Keep large cross-node hash JOINs on the cost-based distributed planner
  roadmap.
