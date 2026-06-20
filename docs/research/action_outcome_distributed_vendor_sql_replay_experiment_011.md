# Action-Outcome Distributed Vendor SQL Replay Experiment 011

Date: 2026-06-20

## Purpose

Experiment 010 proved the vendor baseline comparison on a single ZeptoDB SQL
endpoint. Experiment 011 runs the same vendor replay through a two-node
ZeptoDB HTTP/RPC topology and classifies each SQL/JOIN/window check as either
distributed-safe today or blocked by a current distributed planner boundary.

This experiment is intentionally not a paper exercise: it loads the seed and
vendor tables through one coordinator endpoint, routes rows to physical owners,
and audits the resulting tables through coordinator SQL.

## Procedure

Start node 8:

```bash
build/zepto_http_server \
  --port 19242 \
  --node-id 8 \
  --add-node 1:127.0.0.1:19241 \
  --no-auth \
  --ticks 0 \
  --log-level error
```

Start node 1 as the coordinator endpoint:

```bash
build/zepto_http_server \
  --port 19241 \
  --node-id 1 \
  --add-node 8:127.0.0.1:19242 \
  --no-auth \
  --ticks 0 \
  --log-level error
```

Run the validator:

```bash
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
```

## Results

Result file:
`docs/research/results/action_outcome_distributed_vendor_sql_replay_011.md`

Summary:

| Check | Result |
| --- | --- |
| Seed statements | 203/203 succeeded |
| Seed row counts | pass |
| Vendor table row counts | pass |
| Distributed ingest | pass |
| Boundary classification | pass |
| Full distributed SQL/JOIN/window | partial |

Node-local stats delta:

| Node ID | ticks_ingested | ticks_stored | partitions_created |
| ---: | ---: | ---: | ---: |
| 1 | 161 | 161 | 4 |
| 8 | 208 | 208 | 5 |

## Classification

| Check | Classification | Interpretation |
| --- | --- | --- |
| Failed-repeat JOIN | pass | Recommendations and query controls are co-located on node 8. |
| Context top-action JOIN | pass | Same co-location; context-gated top actions remain safe. |
| Misleading retrieval JOIN | pass | Retrieval evidence and query controls are co-located on node 8. |
| Suppression JOIN | expected_gap_cross_node_join | Suppressions are owned by node 1 while recommendations are owned by node 8. |
| ROW_NUMBER | pass | Cluster-mode full-data materialization preserves declared recommendation-table values. |
| LAG | pass | Same schema-aware materialization path; values match the single-node/vendor baseline. |

## Interpretation

Experiment 011 gives a sharper distributed roadmap than a generic "JOIN/window
does not work" label. The distributed write and row-count surfaces are solid:
both nodes ingest data, and every seed/vendor table returns the expected
cluster-visible row count.

The current gap is narrow:

1. Cross-node hash JOIN needs a small-table strategy, likely broadcast or
   replicated operational tables first.

Cluster-mode ROW_NUMBER/LAG over declared operational tables now pass. The
coordinator fetch-and-compute path materializes the temporary full-data table
through schema-aware typed rows instead of replaying everything through the
legacy tick-shaped `symbol/price/volume/timestamp` path.

## Next Best Step

Implement small-table distributed hash JOIN for the suppression table. With
ROW_NUMBER/LAG passing, the remaining Experiment 011 failure is purely
cross-node JOIN planning.
