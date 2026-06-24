# Physical AI Action-Outcome Experiment 014 SQL Replay

Date: 2026-06-23
Status: Research complete
Classification: Research-only

## Goal

Replay the Physical AI Action-Outcome fixture through live ZeptoDB native SQL so
the robot-safety result from Experiment 013 is validated outside the Python-only
baseline harness.

The experiment materializes robot-operation-shaped tables for incidents,
expected recovery actions, historical action outcomes, robot state telemetry,
sensor summaries, poses, recommendations, retrieval evidence, and context-gate
suppressions.

## Hypothesis

If Action-Outcome Memory is a useful Physical AI primitive, the same evidence
should survive native SQL materialization:

- non-gated baselines should still repeat unsafe Top-1 actions,
- context-gated Action-Outcome Memory should still select expected recovery
  actions,
- misleading hard distractors should remain visible through a suppression audit,
- event-time robot state and sensor summaries should join to incident rows
  through ASOF JOIN,
- recommendation ranking should be replayable with SQL window functions.

## Inputs

- Fixture:
  `docs/research/fixtures/physical_ai_action_outcome_episodes.json`
- Baseline harness:
  `docs/research/tools/physical_ai_action_outcome_baseline.py`
- SQL replay harness:
  `docs/research/tools/physical_ai_sql_replay.py`
- Generated SQL:
  `docs/research/results/physical_ai_action_outcome_sql_replay_014.sql`
- Result:
  `docs/research/results/physical_ai_action_outcome_sql_replay_014.md`

## Procedure

1. Start a live local ZeptoDB HTTP server.
2. Load the Physical AI fixture and compute the four Experiment 013 variants:
   - `similar_robot_incident`,
   - `runbook_action_prior`,
   - `reflection_only_memory`,
   - `context_gated_physical_ai_action_outcome`.
3. Create nine native SQL tables:
   - `physical_ai_incidents_014`,
   - `physical_ai_expected_actions_014`,
   - `physical_ai_action_outcomes_014`,
   - `physical_ai_robot_state_014`,
   - `physical_ai_sensor_summary_014`,
   - `physical_ai_pose_014`,
   - `physical_ai_recommendations_014`,
   - `physical_ai_retrieval_014`,
   - `physical_ai_suppressions_014`.
4. Insert robot/fleet-shaped rows:
   - incident family, robot id, site, zone, payload, temporal motif, change
     context, unsafe action, and expected recovery actions,
   - historical action/outcome rows with recovery metrics,
   - three robot state rows per query incident,
   - three sensor summary rows per query incident,
   - one geospatial pose row per query incident,
   - recommendation/ranking rows for all four variants,
   - retrieval and suppression audit rows.
5. Validate native SQL row counts.
6. Validate hash JOINs for:
   - unsafe Top-1 action repetition,
   - context-gated Top-1 recovery action,
   - suppression audit evidence,
   - expected action to positive historical action/outcome.
7. Validate ASOF JOINs for robot state and sensor summary binding.
8. Validate `ROW_NUMBER` and `LAG` recommendation windows.
9. Validate `ST_Within` geofence filtering for the AGV dock pose.

## Native SQL Notes

The replay uses materialized composite keys for multi-field operational
relationships, such as `expected_action_key`, `incident_action_key`, and
`suppression_key`. This keeps the validation on ZeptoDB's native hash JOIN path
without depending on multi-column string predicate shape.

The current native ASOF path returns numeric projections, so the replay stores
semantic robot/action/metric strings alongside stable integer codes and validates
ASOF using those codes.

These are research-harness choices, not new runtime behavior or public API
changes.

## Command

```bash
./build/zepto_http_server --port 19341 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_sql_replay.py \
  --url http://127.0.0.1:19341/ \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_action_outcome_sql_replay_014.md \
  --sql-output docs/research/results/physical_ai_action_outcome_sql_replay_014.sql \
  --timeout 10
```

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| SQL replay harness compiles with `py_compile` | pass |
| Fixture parses as JSON | pass |
| Live ZeptoDB server accepts all DDL/DML | pass |
| All table row counts match expected materialization | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Suppression audit JOIN | pass |
| Action/outcome JOIN | pass |
| Robot state ASOF JOIN | pass |
| Sensor ASOF JOIN | pass |
| `ROW_NUMBER` window | pass |
| `LAG` window | pass |
| `ST_Within` geofence check | pass |

## Result

See `docs/research/results/physical_ai_action_outcome_sql_replay_014.md`.

Summary:

- Overall SQL replay status: pass.
- 9 tables materialized.
- 227 total research rows inserted.
- 15 failed unsafe Top-1 recommendations found across the three non-gated
  baselines.
- 5/5 context-gated Top-1 recommendations matched expected recovery actions.
- 5 misleading hard distractors were exposed through the suppression audit JOIN.
- Robot state ASOF JOIN and sensor ASOF JOIN both passed.
- `ROW_NUMBER`, `LAG`, and `ST_Within` checks passed.

## Interpretation

Experiment 014 turns the Physical AI claim into replayable SQL evidence:
ZeptoDB can store robot incident context, action outcomes, retrieved evidence,
suppression decisions, event-time robot state, sensor summaries, and geospatial
pose rows in one time-series database and validate the decision trail with
native SQL.

The result does not promote a new product feature. It shows that the
Action-Outcome research primitive has a concrete Physical AI SQL replay shape
that can be extended to edge-local and fleet-global memory experiments.

## Next Product Or Research Step

Port the replay into a two-node live topology and split memory into:

- edge-local robot memory for immediate unsafe-action suppression,
- fleet-global memory for slower cross-robot consolidation and audit.

Completed by Experiment 015:
`docs/research/physical_ai_edge_fleet_replay_experiment_015.md`.
