# Action-Outcome SQL-Backed Replay Experiment 006

Date: 2026-06-18

## Purpose

Experiments 001-005 used Python fixtures directly. That proved the research
logic but did not yet connect the Action-Outcome Memory Engine to ZeptoDB's core
claim: time-series data should be stored, filtered, replayed, and audited through
the database.

Experiment 006 is the first SQL-backed replay slice. It maps the JSON fixture
into SQL tables, reads the episodes back from those tables, and reruns the
Experiment 005 context-gated replay from the SQL-backed episode store.

## Scope

This experiment intentionally stays small:

- Create a ZeptoDB-compatible SQL schema for action-outcome episodes.
- Convert base episodes, distractor episodes, and retrieval quality labels into
  SQL rows.
- Use a local SQL execution harness to validate the schema and data flow.
- Reconstruct replay episodes from SQL rows.
- Compare SQL-backed replay output with the prior JSON-backed replay.

The local harness uses SQLite only as a deterministic SQL executor for research
validation. The generated DDL and INSERT statements use ZeptoDB-compatible types
(`STRING`, `INT64`, `DOUBLE`) so the same seed can become a ZeptoDB ingest input
in the next step.

## Tables

### `action_outcome_episodes`

One row per operational episode. This table stores the indexed fields needed for
candidate filtering, action ranking, audit, and replay reconstruction:

- episode and incident ids
- service, environment, incident family
- time windows and action timestamp
- action class, target, actor, tool
- policy decision and risk tier
- outcome labels
- topology and change context
- compact JSON/text payloads for alerts, logs, traces, tags, and reflections

### `action_outcome_episode_metrics`

One row per metric in an episode's pre-action symptom window.

This is the first time-series-friendly table in the research schema. It lets
future replay queries filter or aggregate by metric discriminators such as
`cpu_pct`, `db_conn_used_pct`, `consumer_error_pct`, and `freshness_lag_s`.

### `action_outcome_retrieval_quality_labels`

One row per query-candidate quality label.

Labels remain research data, not product ground truth. They are used to evaluate
whether top-3 retrieval evidence is useful, superficial, or misleading.

### `action_outcome_replay_recommendations`

One row per replay query/action recommendation produced by the SQL-backed run.

### `action_outcome_gate_suppressions`

One row per historical outcome downweighted by the context gate.

This table is important for operator trust because it records why an apparently
successful historical action was not reused.

## Evaluation

Run:

```bash
python3 docs/research/tools/action_outcome_sql_replay.py \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_sql_replay_006.md \
  --sql-output docs/research/results/action_outcome_sql_replay_006.sql
```

Success criteria:

- SQL row counts match fixture counts.
- SQL-backed `context_gated` top actions match JSON-backed Experiment 005.
- SQL-backed `context_gated` keeps top-3 hit rate at 1.00.
- SQL-backed `context_gated` keeps failed-action avoidance at 1.00.
- Gate suppression rows are materialized for audit.

## Next Best Step

After this experiment, run the generated SQL against a live ZeptoDB HTTP server
and replace the local SQL harness with live `POST /` query execution.
