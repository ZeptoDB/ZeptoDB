# Action-Outcome Live SQL Replay Experiment 007

Date: 2026-06-18

## Purpose

Experiment 006 proved the Action-Outcome replay contract with a local SQL
executor. Experiment 007 runs the generated SQL seed through a live ZeptoDB HTTP
SQL endpoint to identify the first real engine integration boundary.

## Scope

This experiment checks:

- whether the generated DDL and INSERT statements are accepted by `POST /`;
- whether live table row counts match the SQL-backed control;
- whether value-level replay queries can recover top actions and guardrail
  audit rows from live ZeptoDB tables.

## Procedure

Start a local ZeptoDB server:

```bash
build/zepto_http_server --port 19125 --no-auth --log-level error
```

Run the live replay validator:

```bash
python3 docs/research/tools/action_outcome_live_sql_replay.py \
  --url http://127.0.0.1:19125/ \
  --sql-file docs/research/results/action_outcome_sql_replay_006.sql \
  --output docs/research/results/action_outcome_live_sql_replay_007.md
```

The validator counts expected rows from the generated INSERT statements, loads
the full seed into the live endpoint, queries row counts back from ZeptoDB, and
then attempts value-level top-action replay checks.

## Results

Live parser, ingest, row-count, projection, and WHERE compatibility passed:

| Check | Result |
| --- | --- |
| SQL statements attempted | 203 |
| SQL statements succeeded | 203 |
| SQL statements failed | 0 |
| Row-count verification | pass |
| Semantic top-action replay | pass |
| Failed-action avoidance WHERE query | pass |

Live row counts matched the Experiment 006 control:

| Table | Live Rows |
| --- | ---: |
| `action_outcome_episodes` | 32 |
| `action_outcome_episode_metrics` | 101 |
| `action_outcome_retrieval_quality_labels` | 26 |
| `action_outcome_replay_recommendations` | 18 |
| `action_outcome_gate_suppressions` | 21 |

The semantic query recovered the expected top actions:

```sql
SELECT query_id, action_class
FROM action_outcome_replay_recommendations
WHERE top_action = 1
ORDER BY query_id;
```

| Query | Expected Top Action | Live Top Action |
| --- | --- | --- |
| `aoe_checkout_002` | `rollback` | `rollback` |
| `aoe_payment_002` | `traffic_drain` | `traffic_drain` |
| `aoe_inventory_002` | `config_revert` | `config_revert` |
| `aoe_cache_002` | `cache_purge` | `cache_purge` |
| `aoe_queue_003` | `scale_out` | `scale_out` |
| `aoe_search_003` | `rollback` | `rollback` |

The live guardrail query also returned six rows where top actions avoided
repeating failed prior actions:

```sql
SELECT count(*)
FROM action_outcome_replay_recommendations
WHERE top_action = 1 AND avoids_failed_repeat = 1;
```

## Interpretation

Experiment 007 now validates the Action-Outcome SQL seed as a live ZeptoDB
acceptance harness. Generic table INSERT materialization lets declared
`STRING`, `DOUBLE`/`FLOAT64`, and integer columns remain queryable through the
HTTP SQL endpoint, so the research schema no longer needs a tick-shaped
projection just to support replay queries.

This closes the Experiment 007 integration boundary found before devlog 186:
row counts, projection, and WHERE semantics now agree with the local SQL-backed
control from Experiment 006.

## Regression Test

Devlog 185 added the active red C++ regression test:
`ActionOutcomeSQLReplay.GenericInsertProjectionMaterializesDeclaredColumns`.
Devlog 186 implements the generic INSERT path and the test now passes. The test
captures this minimal arbitrary schema INSERT projection case:

```sql
CREATE TABLE action_outcome_projection (
    query_id STRING,
    action_class STRING,
    top_action INT64,
    score_micros INT64,
    timestamp_ns INT64
);

INSERT INTO action_outcome_projection
    (query_id, action_class, top_action, score_micros, timestamp_ns)
VALUES
    ('aoe_payment_002', 'traffic_drain', 1, 670031, 1781748536000000000);

SELECT query_id, action_class
FROM action_outcome_projection
WHERE top_action = 1;
```

Expected and current product behavior: one row with `score_micros = 670031`.

## Next Best Step

Add a distributed two-node replay run that exercises
`ClusterNodeBase::ingest_typed_row()` routing with symbol-less Action-Outcome
tables. After that, extend the acceptance contract with JOIN/window queries
over replay recommendations and episode metadata.
