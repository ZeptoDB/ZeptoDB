# Action-Outcome Experiment 012: Operational Placement Policy And Telemetry

Date: 2026-06-21
Status: Experimental validation complete
Classification: Experimental runtime path

## Goal

Verify that bounded small-table Action-Outcome JOINs can use explicit
operational table placement instead of relying on accidental
`(stable_table_id, symbol_id=0)` ownership. The experiment must also prove that
operators can observe when the bounded JOIN path is used, accepted, rejected by
row cap, or failed.

## Hypothesis

Action-Outcome operational tables are small enough to place intentionally and
join through the bounded coordinator path. This is commercially useful because
operators need predictable placement for control tables and telemetry that
distinguishes safe bounded replay from unbounded distributed SQL planning.

## Governance Boundary

This experiment validates an experimental runtime path under
`docs/research/EXPERIMENT_GOVERNANCE.md`.

Promoted product support is not claimed yet. The current limitations are:

- Placement policy is admin-only runtime state, not DDL/catalog metadata.
- Placement is not replayed automatically after restart.
- Placement is not a rebalance, failover, or rolling-upgrade policy.
- Bounded small-table JOIN is validated for small operational/control tables
  under the row cap, not arbitrary cross-node JOINs.
- Cluster full-data window materialization is validated for this replay shape,
  but still needs product limits and telemetry for larger tables.

## Procedure

1. Start two `zepto_http_server` nodes with HTTP/RPC cluster routing:
   - node 1 at `http://127.0.0.1:19241/`
   - node 8 at `http://127.0.0.1:19242/`
2. Load the existing Action-Outcome SQL seed.
3. Create the Experiment 010 vendor tables.
4. Apply explicit placement policy through `POST /admin/table-placement`:
   - `action_outcome_vendor_queries_010` pinned to node 8
   - `action_outcome_vendor_recommendations_010` pinned to node 8
   - `action_outcome_vendor_retrieval_010` pinned to node 8
   - `action_outcome_vendor_suppressions_010` pinned to node 1
5. Materialize the vendor rows.
6. Validate full distributed SQL/JOIN/window replay:
   - failed-repeat JOIN
   - context top-action JOIN
   - suppression JOIN across node 1 and node 8
   - misleading retrieval JOIN
   - ROW_NUMBER window
   - LAG window
7. Capture `/stats` and `/metrics` before/after JOIN validation.

## Command

```bash
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

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| Placement API updates | 4/4 pass |
| Seed row-count status | pass |
| Vendor table row-count status | pass |
| Node-local ingest | both nodes receive rows |
| Full SQL/JOIN/window replay | pass |
| Small-table JOIN candidates | at least 4 |
| Accepted small-table JOINs | at least 4 |
| Row-cap rejections | 0 |
| Small-table JOIN errors | 0 |
| Prometheus metric presence | all new metrics present |

## Result

See
`docs/research/results/action_outcome_operational_placement_012.md`.

Summary:

- Overall Experiment 012 status: pass.
- Node-local ingest delta: 161 rows on node 1 and 208 rows on node 8.
- JOIN/window checks: 6/6 pass.
- Small-table JOIN telemetry: 4 candidates, 4 accepted, 0 row-cap rejections,
  0 errors, 327 materialized rows.
- Prometheus telemetry: all seven small-table JOIN metrics present.

## Interpretation

Experiment 012 converts operational-table placement from an implicit hash-ring
side effect into an operator-visible policy. The intentionally cross-node
suppression JOIN remains correct because the bounded small-table JOIN path
materializes both sides under a row cap and delegates JOIN semantics to the
local SQL executor.

The next product step is persistence: runtime placement proves the model, but
production users will expect placement to be declared with the table or stored
in the catalog.
