# Action-Outcome Context Gate Experiment 005

Date: 2026-06-14

## Purpose

Experiment 004 showed a critical failure mode: successful historical actions can
be harmful evidence when the success occurred under a different causal context.
The noisy distractor fixture caused the current `full_guarded` strategy to repeat
failed actions for cache and queue incidents because superficially similar
`restart` episodes had succeeded in single-pod contexts.

Experiment 005 adds a context-conditioned outcome gate. The goal is to reuse
action outcomes only when the candidate episode's causal context is close enough
to the query episode.

## Research Claim

Action-Outcome Memory should not mean "prefer actions that worked before." It
should mean:

1. Retrieve similar time-series episodes.
2. Check whether the causal context is compatible.
3. Reuse positive or negative outcomes only after the context check.
4. Downweight historical outcomes that came from incompatible contexts.

This is the key distinction between an action log and a commercial AIOps memory
engine.

## Context Gate Signals

The first transparent gate uses only fixture-level structured signals:

- Blast-radius compatibility.
- Single-entity versus service-wide topology compatibility.
- Change-type compatibility.
- Metric discriminator compatibility:
  - CPU saturation mismatch.
  - DB connection saturation mismatch.
  - Consumer error-rate mismatch.
  - Pod skew mismatch.
  - Feature-flag versus deploy mismatch.
- Existing time-series, topology, and change similarity scores.

The gate intentionally stays heuristic and auditable. It is not an ML model yet.
That makes failures inspectable before moving to embeddings or learned policies.

## Variants

- `full_guarded`: current guarded strategy from Experiment 004.
- `context_gated`: guarded strategy plus context-conditioned outcome reuse.

## Metrics

- Top-3 successful action hit rate.
- Failed-action avoidance rate.
- Labeled top-3 retrieval quality.
- Top action changes versus `full_guarded`.
- Gate suppression count:
  - how many positive or negative historical outcomes were downweighted because
    context was incompatible.

## Success Criteria

The context gate is useful if it improves failed-action avoidance on the noisy
fixture without reducing top-3 successful action hit rate.

It is especially important to inspect:

- `aoe_cache_002`, where restart failed for shared cache staleness but succeeded
  for pod-local cache skew.
- `aoe_queue_003`, where restart failed for capacity-driven backlog but
  succeeded for one stuck consumer partition.
- `aoe_search_003`, where restart is a temporary mitigation but rollback is the
  durable fix for deploy-correlated memory leak.

## Expected Result

The gate should reduce the influence of misleading successful distractors. It
does not need to eliminate all misleading top-3 retrievals in this experiment,
but action aggregation should become safer.

## Next Best Step

After the gate is validated, map the episode fixture into ZeptoDB SQL tables and
rerun the same report through database-backed retrieval.
