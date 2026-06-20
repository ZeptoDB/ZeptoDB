# Action-Outcome Vendor SQL Replay Experiment 010

Date: 2026-06-18

## Purpose

The first Experiment 010 report compared similar-incident retrieval,
runbook/action-prior recommendation, reflection-only memory, and
context-gated Action-Outcome Memory in Python. This replay verifies that the
same comparison can be audited through native ZeptoDB SQL tables, hash JOINs,
and window functions.

The practical question is whether our differentiator is inspectable inside the
database:

- which top recommendations repeat historically failed actions;
- which context-gated recommendations avoid those failed actions;
- which retrieved records were misleading;
- which candidate outcomes were suppressed by the context gate;
- whether recommendation ordering is stable under `ROW_NUMBER` and `LAG`.

## Inputs

- SQL seed: `docs/research/results/action_outcome_sql_replay_006.sql`
- Base fixture: `docs/research/fixtures/action_outcome_episodes.json`
- Noisy distractor fixture:
  `docs/research/fixtures/action_outcome_distractor_episodes.json`
- Retrieval quality labels:
  `docs/research/fixtures/action_outcome_retrieval_quality_labels.json`

## Command

```bash
build/zepto_http_server --port 19343 --ticks 0 --no-auth --log-level error

python3 docs/research/tools/action_outcome_vendor_sql_replay.py \
  --url http://127.0.0.1:19343/ \
  --extra-fixture docs/research/fixtures/action_outcome_distractor_episodes.json \
  --quality-labels docs/research/fixtures/action_outcome_retrieval_quality_labels.json \
  --output docs/research/results/action_outcome_vendor_sql_replay_010.md \
  --timeout 10
```

## Materialized Tables

| Table | Rows | Purpose |
| --- | ---: | --- |
| `action_outcome_vendor_queries_010` | 6 | Query controls and historical failed actions. |
| `action_outcome_vendor_recommendations_010` | 72 | Four variants x six queries x top three actions. |
| `action_outcome_vendor_retrieval_010` | 72 | Retrieval evidence quality for each variant/query pair. |
| `action_outcome_vendor_suppressions_010` | 21 | Context-gated suppressions that block unsafe outcome reuse. |

## Acceptance Checks

| Check | Status | What it proves |
| --- | --- | --- |
| Seed row counts | pass | The base Action-Outcome SQL seed still loads fully. |
| Vendor table row counts | pass | The Experiment 010 comparison is fully materialized in ZeptoDB. |
| Failed-repeat JOIN | pass | Baseline variants still expose top actions that match historical failures. |
| Context top-action JOIN | pass | Context-gated top actions avoid every historical failed action. |
| Suppression JOIN | pass | The 21 context suppressions are queryable against recommendation rows. |
| Misleading retrieval JOIN | pass | Misleading evidence is visible instead of hidden by the harness. |
| `ROW_NUMBER` | pass | Recommendation rank ordering is stable under window replay. |
| `LAG` | pass | Per-variant/query score deltas are stable under window replay. |

## Result Summary

The live result report is
`docs/research/results/action_outcome_vendor_sql_replay_010.md`.

The key product result remains intact after SQL materialization:

| Query | Context-Gated Top Action | Historical Failed Action |
| --- | --- | --- |
| `aoe_cache_002` | `cache_purge` | `restart` |
| `aoe_checkout_002` | `rollback` | `restart` |
| `aoe_inventory_002` | `config_revert` | `scale_out` |
| `aoe_payment_002` | `traffic_drain` | `scale_out` |
| `aoe_queue_003` | `scale_out` | `restart` |
| `aoe_search_003` | `rollback` | `scale_out` |

## Interpretation

This turns the vendor baseline comparison from a Python-only research fixture
into a ZeptoDB-native audit path. The important difference is not only that the
context gate chooses safer actions. The evidence trail can now be queried:
recommendations, suppressions, retrieval quality, and historical outcomes are
all visible through SQL/JOIN/window checks.

That is the commercial angle: an SRE can inspect why the memory engine avoided
a past action, not merely trust a black-box ranking.

## Next Best Step

Experiment 011 now ports this replay into the two-node live harness and records
which checks are already distributed-safe versus current planner gaps. The next
engineering step is to fix cluster-mode window value materialization for
declared operational tables, then add a narrow small-table distributed hash JOIN
path for the suppression JOIN.
