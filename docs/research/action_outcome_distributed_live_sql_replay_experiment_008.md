# Action-Outcome Distributed Live SQL Replay Experiment 008

Date: 2026-06-18

## Purpose

Experiment 007 validated Action-Outcome replay on a single live ZeptoDB HTTP
endpoint. Experiment 008 validates the same seed through a real two-node
HTTP/RPC topology so schema-aware typed-row INSERT, DDL replication, routed
storage, scatter-gather SELECT, and decoded string results are exercised
together.

## Procedure

Build the server and test binary:

```bash
cd build
ninja -j$(nproc) zepto_tests
ninja -j$(nproc) zepto_http_server
```

Start node 8 first:

```bash
./build/zepto_http_server \
  --port 19242 \
  --node-id 8 \
  --add-node 1:127.0.0.1:19241 \
  --no-auth \
  --ticks 0 \
  --log-level error
```

Start node 1 as the coordinator endpoint used by the validator:

```bash
./build/zepto_http_server \
  --port 19241 \
  --node-id 1 \
  --add-node 8:127.0.0.1:19242 \
  --no-auth \
  --ticks 0 \
  --log-level error
```

Run the distributed validator:

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

Node ids `1` and `8` are intentional: the Action-Outcome seed is symbol-less,
so rows route by `(stable_table_id, symbol_id=0)`. The `1/8` ring splits the
five Action-Outcome tables across both nodes; the `1/2` ring happens to place
all five on node 1.

## Results

Result file:
`docs/research/results/action_outcome_distributed_live_sql_replay_008.md`

Summary:

| Check | Result |
| --- | --- |
| SQL statements attempted | 203 |
| SQL statements succeeded | 203 |
| SQL statements failed | 0 |
| Row-count verification | pass |
| Semantic top-action replay | pass |
| Distributed ingest | pass |
| Remote decoded string SELECT | pass |

Node-local stats delta:

| Node ID | ticks_ingested | ticks_stored | partitions_created |
| ---: | ---: | ---: | ---: |
| 1 | 140 | 140 | 3 |
| 8 | 58 | 58 | 2 |

Owner split:

| Table | Owner node | Inserts |
| --- | ---: | ---: |
| `action_outcome_episode_metrics` | 1 | 101 |
| `action_outcome_episodes` | 8 | 32 |
| `action_outcome_gate_suppressions` | 1 | 21 |
| `action_outcome_replay_recommendations` | 1 | 18 |
| `action_outcome_retrieval_quality_labels` | 8 | 26 |

The semantic top-action query returned the six expected replay
recommendations, and the guardrail query returned six rows where top actions
avoid repeating failed prior actions.

The remote string query against `action_outcome_episodes`, which is owned by
node 8 in the 1/8 ring, returned decoded `episode_id` and `action_class`
strings rather than node-local dictionary codes.

## Fixes Captured During The Run

The first distributed run exposed two product gaps:

1. `CoordinatorRoutingAdapter` did not override `ingest_typed_row()`, so
   declared-schema INSERT could not route through the non-template HTTP server
   adapter.
2. Distributed concat SELECT over `STRING`/`SYMBOL` columns returned node-local
   dictionary codes instead of decoded strings because RPC serialization and
   merge did not preserve string cells.

The routing gap is fixed in devlog 187. The follow-up C++ integration
regression and typed-row `STRING` payload hardening are recorded in devlog 188.

## Interpretation

Experiment 008 is the first end-to-end distributed Action-Outcome replay
acceptance run. It proves that the SQL-backed Action-Outcome seed can be loaded
through one HTTP node, stored across two physical owners, queried back through
cluster SELECT, and still produce the same replay decision surface as the
single-node harness.

## Next Best Step

The CI-sized C++ regression now covers schema-aware typed-row routing and
decoded remote `STRING` SELECT results. Next, extend the research harness with
JOIN/window replay queries across `action_outcome_episodes` and
`action_outcome_replay_recommendations`, then define a shard-key policy for
symbol-less operational tables.
