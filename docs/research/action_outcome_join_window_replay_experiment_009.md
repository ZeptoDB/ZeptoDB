# Action-Outcome JOIN/Window Replay Experiment 009

Date: 2026-06-18

## Purpose

Experiment 009 extends the Action-Outcome replay harness beyond load, row
counts, projection, and top-action filtering. It checks whether replay
recommendations can be validated with SQL JOIN and window semantics against a
live ZeptoDB endpoint.

The experiment deliberately separates two questions:

1. Can the current engine validate the replay decision surface with JOIN and
   window queries?
2. Does the native research-shape string-key JOIN already work directly across
   `action_outcome_replay_recommendations.query_id` and
   `action_outcome_episodes.episode_id`?

## Procedure

Start a local ZeptoDB server:

```bash
build/zepto_http_server --port 19341 --ticks 0 --no-auth --log-level error
```

Run the validator:

```bash
python3 docs/research/tools/action_outcome_join_window_replay.py \
  --url http://127.0.0.1:19341/ \
  --sql-file docs/research/results/action_outcome_sql_replay_006.sql \
  --output docs/research/results/action_outcome_join_window_replay_009.md
```

The validator:

- loads the Experiment 006 SQL seed into the live endpoint;
- parses the same seed into an in-memory SQLite control database;
- derives six query-level control rows and eighteen recommendation rows;
- creates numeric acceptance projection tables with explicit `symbol` and
  `timestamp_ns` columns for stable ZeptoDB partitioning;
- runs a native string-window query over the original replay table;
- runs numeric JOIN and ROW_NUMBER/LAG window queries over the projection;
- probes the native string-key JOIN on the original research schema.

## Results

Result file:
`docs/research/results/action_outcome_join_window_replay_009.md`

Summary:

| Check | Result |
| --- | --- |
| SQL statements attempted | 203 |
| SQL statements succeeded | 203 |
| SQL statements failed | 0 |
| Seed row-count verification | pass |
| Projection row-count verification | pass |
| Native string window | pass |
| Numeric JOIN/window acceptance | pass |
| Native string-key JOIN | pass |

The native string-window query validates replay rank order directly on the
original table:

```sql
SELECT query_id,
       recommendation_rank,
       ROW_NUMBER() OVER (
           PARTITION BY query_id
           ORDER BY recommendation_rank
       ) AS rank_check
FROM action_outcome_replay_recommendations
ORDER BY query_id, recommendation_rank;
```

It returns eighteen rows where `rank_check == recommendation_rank`.

The numeric acceptance projection validates JOIN and window semantics with
stable integer keys:

```sql
SELECT r.query_seq,
       r.rank_num,
       r.action_code,
       q.observed_action_code,
       q.expected_top_action_code,
       r.avoid_num
FROM action_outcome_acceptance_recommendations_009 r
JOIN action_outcome_acceptance_queries_009 q
  ON r.query_seq = q.query_seq
ORDER BY r.query_seq, r.rank_num;
```

The top recommendation for each query matches the expected top action code. For
queries whose original episode human outcome is failure, the top action does
not repeat the failed observed action and `avoid_num == 1`.

The numeric window chain also passes:

```sql
SELECT query_seq,
       rank_num,
       action_code,
       score_num,
       ROW_NUMBER() OVER (
           PARTITION BY query_seq
           ORDER BY rank_num
       ) AS rank_check,
       LAG(score_num, 1, 0) OVER (
           PARTITION BY query_seq
           ORDER BY rank_num
       ) AS prev_score
FROM action_outcome_acceptance_recommendations_009
ORDER BY query_seq, rank_num;
```

## Native String-Key JOIN

The native string-key JOIN now produces semantic rows directly from the
research schema:

```sql
SELECT r.query_id,
       r.recommendation_rank,
       r.action_class,
       e.action_class,
       e.human_outcome
FROM action_outcome_replay_recommendations r
JOIN action_outcome_episodes e ON r.query_id = e.episode_id
ORDER BY r.query_id, r.recommendation_rank;
```

Devlog 190 adds the C++ regression and hash JOIN materialization fix that
preserves declared `STRING` key values and joined string result columns.
Experiment 009 now validates native string-key JOIN, while the numeric
projection remains for top-action and outcome-avoidance checks.

## Interpretation

Experiment 009 makes the Action-Outcome replay harness more useful as a
database acceptance surface. It now covers:

- declared-schema load and row counts;
- value-level top-action replay;
- native string partition window functions;
- SQL JOIN over replay recommendations and episode controls;
- rank and previous-score window chains;
- native string-key hash JOIN over the research schema.

This is the right next step for the time-series memory research path: the
engine can now validate action ranking, outcome avoidance, and string-key
schema joins with SQL operators. Experiment 010 SQL replay later closed the
remaining single-node executor gap by adding alias-aware `WHERE` predicate
handling for hash JOIN queries.

## Next Best Step

Port the Experiment 010 SQL/JOIN/window replay into the distributed two-node
live harness so native replay checks can be compared across single-node and
cluster execution paths.
