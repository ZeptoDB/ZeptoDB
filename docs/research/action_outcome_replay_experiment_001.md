# ActionOutcomeReplay Experiment 001

Date: 2026-06-13
Branch: `codex/aiops-time-series-memory-research`

## Purpose

Define the first replay experiment for the Action-Outcome Memory Engine.

This experiment does not execute actions. It replays known incidents from the
fixture and compares whether action-outcome memory retrieves better prior
episodes and safer action recommendations than simpler baselines.

## Input Data

Fixture:

- `docs/research/fixtures/action_outcome_episodes.json`

Schema:

- `docs/research/action_outcome_episode_schema.md`

Fixture shape:

- 6 incident types.
- 4 episodes per incident type.
- 24 total synthetic AIOps episodes.
- Incident families:
  - `checkout_latency_after_deploy`
  - `payment_dependency_timeout`
  - `inventory_db_connection_pool`
  - `recommendation_cache_stale`
  - `order_queue_backlog`
  - `search_index_memory_leak`

Note: the original minimum target was 5 families and 20 episodes. The fixture
uses 6 families and 24 episodes because memory-leak behavior is a useful
contrast case for restart, rollback, traffic drain, and scale-out.

## Query Episodes

Use leave-one-out evaluation:

1. Select one episode as the query.
2. Hide its outcome and reflection.
3. Search all other episodes.
4. Recommend top actions.
5. Compare recommendation against the hidden outcome and human label.

Initial query set:

| Query Episode | Reason |
| --- | --- |
| `aoe_checkout_002` | Tests whether restart failure retrieves rollback/config-revert successes for checkout deploy regression. |
| `aoe_payment_002` | Tests whether scale-out failure retrieves traffic-drain/rollback success for dependency timeout. |
| `aoe_inventory_002` | Tests whether scale-out rollback case retrieves config/traffic-drain alternatives. |
| `aoe_cache_002` | Tests whether restart failure retrieves cache purge success. |
| `aoe_queue_003` | Tests whether restart failure retrieves scale-out success for capacity backlog. |
| `aoe_search_003` | Tests whether scale-out failure retrieves rollback/restart/traffic-drain distinctions for memory leak. |

## Baselines

### Baseline A: Keyword Retrieval

Score based on overlap across:

- `service`
- `incident_type`
- alert names
- action class
- tags

Expected weakness:

- May overfit service or incident type.
- Does not understand recovery outcome.

### Baseline B: Text-Only Memory

Score based on textual similarity over:

- `reflection`
- human outcome notes
- log signatures
- candidate root-cause text

Expected weakness:

- May retrieve semantically similar explanations but miss topology/change/action
  differences.

### Baseline C: Time-Series Evidence Pack

Score based on:

- symptom similarity,
- metric shape similarity,
- topology similarity,
- change-context similarity.

Expected weakness:

- Can find similar incidents but may not know which actions worked.

### Variant D: Action-Outcome Memory

Score based on:

- symptom similarity,
- temporal motif similarity,
- topology similarity,
- change-context similarity,
- action similarity,
- outcome similarity,
- policy/risk similarity,
- negative-outcome penalty.

Expected strength:

- Should retrieve cases where the same action failed and cases where another
  action succeeded under similar conditions.

## First Scoring Formula

Use the simple scoring formula from
`docs/research/action_outcome_memory_engine_plan.md`:

```text
score =
  0.20 * symptom_similarity +
  0.20 * temporal_motif_similarity +
  0.15 * topology_similarity +
  0.15 * change_similarity +
  0.15 * action_outcome_similarity +
  0.10 * postmortem_text_similarity +
  0.05 * recency_score -
  risk_penalty
```

For Experiment 001, implement the components as deterministic fixture-level
heuristics before using embeddings:

| Component | Initial Heuristic |
| --- | --- |
| `symptom_similarity` | Jaccard overlap over alerts, logs, trace strings, and metric keys. |
| `temporal_motif_similarity` | Compare direction and severity bucket of primary metrics. |
| `topology_similarity` | Jaccard overlap over service, upstream/downstream, and entity refs. |
| `change_similarity` | Match change type and deploy/config/flag presence. |
| `action_outcome_similarity` | Prefer episodes with known successful outcomes for candidate actions; penalize same-action failures. |
| `postmortem_text_similarity` | Token overlap over human notes and reflection. |
| `recency_score` | Use event timestamp ordering in the fixture. |
| `risk_penalty` | Penalize high/critical risk actions unless prior success is strong. |

## Metrics

| Metric | Definition |
| --- | --- |
| `top_1_useful_episode` | Whether the top retrieved episode gives a useful action lesson. |
| `top_3_successful_action_hit` | Whether top 3 retrieval includes an action that succeeded for similar conditions. |
| `same_failed_action_penalty` | Whether the system avoids repeating an action that failed in similar conditions. |
| `recommendation_quality` | Manual label: good, partial, bad, unsafe. |
| `evidence_completeness` | Whether returned evidence includes symptom, action, outcome, and recovery curve. |

## Expected Results

Expected high-level outcomes:

- For cache staleness, action-outcome memory should prefer `cache_purge` over
  `restart` or `scale_out`.
- For dependency timeouts, it should distinguish external-route `traffic_drain`
  from useless `scale_out`.
- For db pool saturation, it should penalize app scale-out because it worsens
  database pressure.
- For queue backlog, it should distinguish capacity backlog from poison-message
  retry loops.
- For search memory leak, it should distinguish temporary restart mitigation
  from durable rollback.

## Research Questions For This Experiment

1. Does outcome memory retrieve negative examples that prevent repeated mistakes?
2. Does adding action-outcome data change the recommended action compared with
   time-series evidence alone?
3. Which fields dominate retrieval: service, incident type, change context,
   action class, or recovery curve?
4. Does the fixture need richer metric shape data before the experiment is
   meaningful?

## Next Implementation Step

Build a small replay evaluator that:

1. Loads `action_outcome_episodes.json`.
2. Runs leave-one-out retrieval.
3. Computes deterministic baseline scores.
4. Emits a comparison table.
5. Writes results to `docs/research/results/action_outcome_replay_001.md`.

Keep this evaluator outside the core C++ engine until the research signal is
clear.
