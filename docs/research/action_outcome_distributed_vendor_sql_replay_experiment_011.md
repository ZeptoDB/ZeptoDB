# Action-Outcome Distributed Vendor SQL Replay Experiment 011

Date: 2026-06-20

## Purpose

Experiment 010 proved the vendor baseline comparison on a single ZeptoDB SQL
endpoint. Experiment 011 runs the same vendor replay through a two-node
ZeptoDB HTTP/RPC topology and verifies that co-located JOINs, bounded
cross-node small-table hash JOINs, and cluster-mode window replay all match
the single-node/vendor baseline.

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
  --timeout 10 \
  --strict-full-sql
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
| Full distributed SQL/JOIN/window | pass |

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
| Suppression JOIN | pass | Suppressions are owned by node 1 while recommendations are owned by node 8; the bounded small-table hash JOIN path gathers both sides and executes the original JOIN locally. |
| ROW_NUMBER | pass | Cluster-mode full-data materialization preserves declared recommendation-table values. |
| LAG | pass | Same schema-aware materialization path; values match the single-node/vendor baseline. |

## Interpretation

Experiment 011 now closes the research harness' last SQL replay boundary. The
distributed write and row-count surfaces are solid: both nodes ingest data,
and every seed/vendor table returns the expected cluster-visible row count.
Co-located vendor JOINs pass, the node 1 to node 8 suppression JOIN passes
through the bounded small-table hash JOIN path, and cluster-mode ROW_NUMBER/LAG
over declared operational tables pass through schema-aware typed temporary
materialization.

This is still intentionally narrower than a fully general distributed SQL
optimizer. The completed path is for bounded operational control tables; large
cross-node hash JOINs should move to a cost-based distributed planner.

## Next Best Step

Define symbol-less operational table placement policy and add row-cap
observability for the small-table JOIN path before promoting it beyond the
research harness.
