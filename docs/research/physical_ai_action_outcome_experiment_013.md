# Physical AI Action-Outcome Experiment 013

Date: 2026-06-23
Status: Research complete
Classification: Research-only

## Goal

Compare similar robot incident retrieval, runbook/action-prior recommendation,
reflection-only memory, and context-gated Physical AI Action-Outcome Memory on
a synthetic robot/fleet edge fixture.

The experiment tests whether a context-gated temporal memory can better avoid
previously risky robot actions while still choosing the right recovery action.

## Hypothesis

Physical AI agents need more than semantic similarity and generic runbook
priors. A past robot action is only safe to reuse when the temporal motif,
topology, payload, human proximity, and change/environment context are
compatible.

Context-gated Action-Outcome Memory should therefore improve:

- recovery Top-1 hit rate,
- risky-repeat avoidance,
- hazardous top-action suppression,
- auditability of misleading but retrieved prior incidents.

## Inputs

- Fixture:
  `docs/research/fixtures/physical_ai_action_outcome_episodes.json`
- Harness:
  `docs/research/tools/physical_ai_action_outcome_baseline.py`
- Result:
  `docs/research/results/physical_ai_action_outcome_013.md`

## Baselines

| Variant | Pattern approximated | What it can miss |
| --- | --- | --- |
| `similar_robot_incident` | Similar robot incident retrieval. | Finds superficially similar incidents without learning whether an action was safe in this context. |
| `runbook_action_prior` | Same-incident-family runbook or historical action prior. | Reuses historically successful actions even when payload, zone, human proximity, or weather differs. |
| `reflection_only_memory` | Experiential memory from reflections/postmortems. | Recalls text and outcomes, but lacks structured gates over telemetry/topology context. |
| `context_gated_physical_ai_action_outcome` | Target Physical AI Action-Outcome Memory. | Still heuristic, but explicitly gates past outcomes before action aggregation. |

## Procedure

1. Load five Physical AI incident families:
   - warehouse AGV dock slip,
   - mobile robot LiDAR occlusion,
   - industrial arm torque spike,
   - cold-chain temperature excursion,
   - drone GPS drift near geofence boundary.
2. For each family, evaluate one failed/unsafe query episode.
3. Include hard distractors where the unsafe action previously succeeded only
   under a different safety context, such as training lanes, supervised
   airspace, simulated payloads, or no-human cells.
4. Rank historical incidents and recommended actions for each baseline.
5. Measure whether the method avoids repeating the unsafe query action.
6. Measure whether the top action is in the expected safe recovery-action set.
7. Record top-3 retrieval quality and context-gate suppressions.

## Command

```bash
python3 docs/research/tools/physical_ai_action_outcome_baseline.py \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_action_outcome_013.md
```

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| Fixture parses as JSON | pass |
| Harness compiles with `py_compile` | pass |
| All four variants run | pass |
| Query families evaluated | 5 |
| Context-gated risky-repeat avoidance | 1.00 |
| Context-gated recovery Top-1 hit rate | 1.00 |
| Context-gated hazardous top-action rate | 0.00 |
| Report records suppressions | yes |

## Result

See `docs/research/results/physical_ai_action_outcome_013.md`.

Summary:

- `context_gated_physical_ai_action_outcome` reached 1.00 recovery Top-1 hit
  rate.
- It reached 1.00 risky-repeat avoidance.
- It reduced hazardous top-action rate to 0.00.
- The three non-gated baselines each recorded 0.00 recovery Top-1 hit rate,
  0.00 risky-repeat avoidance, and 1.00 hazardous top-action rate on the hard
  distractor fixture.
- The gated variant retained misleading incidents in the evidence view for
  audit while
  suppressing unsafe outcome reuse during action aggregation.

## Interpretation

The commercial wedge is not "robot incident search." It is deciding whether a
past robot outcome is safe to reuse in the current temporal context.

This maps cleanly to ZeptoDB's Physical AI positioning: ROS/robot telemetry
provides the event-time substrate, while Action-Outcome Memory records
actions, outcomes, suppressions, and replayable evidence.

## Next Best Step

Replay the fixture through native ZeptoDB SQL tables:

- telemetry windows,
- action/outcome tables,
- ASOF JOIN between robot state and incident/action rows,
- ROW_NUMBER ranking for candidate recommendations,
- suppression JOIN audit.

Completed by Experiment 014:
`docs/research/physical_ai_action_outcome_sql_replay_experiment_014.md`.
