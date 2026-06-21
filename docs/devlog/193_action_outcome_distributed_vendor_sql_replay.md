# 193: Action-Outcome Distributed Vendor SQL Replay

Date: 2026-06-19
Status: Complete

## Context

Experiment 010 made the vendor baseline comparison auditable with native
single-node SQL/JOIN/window queries. The next research step was to run the same
comparison through a two-node ZeptoDB topology and distinguish distributed-safe
surfaces from current distributed planner gaps.

## Changes

- Added `docs/research/tools/action_outcome_distributed_vendor_sql_replay.py`.
  - Reuses the Experiment 010 SQL materialization and validation logic.
  - Reuses the Experiment 008 two-node owner-routing helpers and stats deltas.
  - Loads the Action-Outcome seed and Experiment 010 vendor tables through one
    coordinator endpoint.
  - Classifies each JOIN/window check as `pass`,
    `expected_gap_cross_node_join`, `expected_gap_cluster_window_values`, or
    `fail`.

- Added `docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md`.
  - Records the live two-node replay result.

- Added `docs/research/action_outcome_distributed_vendor_sql_replay_experiment_011.md`.
  - Documents purpose, procedure, result summary, classification, and next
    engineering step.

- Updated backlog, completed-feature docs, distributed design notes, and the
  research process log.

## Verification

```bash
python3 -m py_compile \
  docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  docs/research/tools/action_outcome_vendor_sql_replay.py \
  docs/research/tools/action_outcome_distributed_live_sql_replay.py

ninja -C build -j$(nproc) zepto_http_server

python3 docs/research/tools/action_outcome_distributed_vendor_sql_replay.py \
  --coordinator-url http://127.0.0.1:19241/ \
  --node-a-id 1 \
  --node-b-id 8 \
  --node-a-stats-url http://127.0.0.1:19241/stats \
  --node-b-stats-url http://127.0.0.1:19242/stats \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md \
  --timeout 10

git diff --check
```

Results:

- Seed statements: 203/203 succeeded.
- Seed row counts: pass.
- Vendor table row counts: pass.
- Distributed ingest: pass.
- Node-local deltas: node 1 ingested 161 rows; node 8 ingested 208 rows.
- Co-located JOINs: failed-repeat, context top-action, and misleading
  retrieval JOINs pass.
- Current expected gaps:
  - `suppression_join`: `expected_gap_cross_node_join`.
  - `row_number_window` and `lag_window`:
    `expected_gap_cluster_window_values`.

No full C++ test suite was run in this session because this change adds a
Python research harness and documentation. The HTTP server binary was already
up to date.

## Follow-ups

- Fix cluster-mode window value materialization for declared operational
  tables.
- Add a small-table distributed hash JOIN strategy for operational tables,
  starting with broadcast/replicated dimension-table joins.
