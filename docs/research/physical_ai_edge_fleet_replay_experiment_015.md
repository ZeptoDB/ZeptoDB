# Physical AI Action-Outcome Experiment 015 Edge/Fleet Replay

Date: 2026-06-23
Status: Research complete
Classification: Research-only

## Goal

Validate a two-node Physical AI memory shape:

- edge-local memory suppresses unsafe robot actions immediately,
- fleet-global memory receives slower consolidated evidence for audit and
  cross-robot learning.

Experiment 014 proved the single-node native SQL replay. Experiment 015 splits
that evidence across two live ZeptoDB HTTP endpoints.

## Hypothesis

Physical AI deployments should not wait for fleet/cloud consolidation before
blocking a known unsafe action. A robot or edge controller should make the
immediate safety decision locally, then send compact decision, retrieval, and
suppression evidence to a fleet node for slower audit and learning.

The expected result is:

- edge-local node chooses expected recovery actions,
- edge-local node suppresses the unsafe query action immediately,
- edge-local ASOF joins bind incidents to current robot/sensor state,
- fleet-global node receives delayed consolidated decisions,
- fleet-global audit JOIN identifies the misleading hard distractors,
- fleet-global window functions preserve retrieval ordering and consolidation
  lag history.

## Inputs

- Fixture:
  `docs/research/fixtures/physical_ai_action_outcome_episodes.json`
- Baseline harness:
  `docs/research/tools/physical_ai_action_outcome_baseline.py`
- Experiment 014 helper:
  `docs/research/tools/physical_ai_sql_replay.py`
- Experiment 015 harness:
  `docs/research/tools/physical_ai_edge_fleet_replay.py`
- Edge SQL replay:
  `docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql`
- Fleet SQL replay:
  `docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql`
- Result:
  `docs/research/results/physical_ai_edge_fleet_replay_015.md`

## Procedure

1. Start an edge-local ZeptoDB server on port 19441.
2. Start a fleet-global ZeptoDB server on port 19442.
3. Load the Physical AI fixture and recompute the Experiment 013 context-gated
   comparison.
4. Materialize edge-local tables:
   - incidents,
   - expected actions,
   - robot state,
   - sensor summaries,
   - immediate decisions,
   - immediate suppressions.
5. Materialize fleet-global tables:
   - expected actions,
   - historical action outcomes,
   - delayed edge decisions,
   - top-3 retrieval evidence,
   - suppression audit rows.
6. Validate edge-local SQL:
   - row counts,
   - recovery-action JOIN,
   - risky-action suppression count,
   - robot state ASOF JOIN,
   - sensor ASOF JOIN.
7. Validate fleet-global SQL:
   - row counts,
   - consolidated recovery-action JOIN,
   - delayed consolidation lag,
   - suppression audit JOIN,
   - retrieval `ROW_NUMBER`,
   - consolidation-lag `LAG`.

## Boundary

This is a research-only harness. The delayed edge-to-fleet transfer is modeled
by Python writing SQL rows to two live ZeptoDB endpoints. It is not a new
runtime replication, routing, or control-plane feature.

## Command

```bash
./build/zepto_http_server --port 19441 --node-id 1 --no-auth --storage-mode pure

./build/zepto_http_server --port 19442 --node-id 8 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_edge_fleet_replay.py \
  --edge-url http://127.0.0.1:19441/ \
  --fleet-url http://127.0.0.1:19442/ \
  --edge-stats-url http://127.0.0.1:19441/stats \
  --fleet-stats-url http://127.0.0.1:19442/stats \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_edge_fleet_replay_015.md \
  --edge-sql-output docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql \
  --fleet-sql-output docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql \
  --timeout 10
```

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| Harness compiles with `py_compile` | pass |
| Edge server accepts all DDL/DML | pass |
| Fleet server accepts all DDL/DML | pass |
| Edge row counts | pass |
| Fleet row counts | pass |
| Edge recovery-action JOIN | pass |
| Edge risky-action suppression | pass |
| Edge robot ASOF JOIN | pass |
| Edge sensor ASOF JOIN | pass |
| Fleet consolidated recovery JOIN | pass |
| Fleet suppression audit JOIN | pass |
| Fleet consolidation lag | pass |
| Fleet `ROW_NUMBER` | pass |
| Fleet `LAG` | pass |

## Result

See `docs/research/results/physical_ai_edge_fleet_replay_015.md`.

Summary:

- Overall edge/fleet replay status: pass.
- Edge-local node stored 82 research rows.
- Fleet-global node stored 87 research rows.
- Edge-local node selected all five expected recovery actions before
  consolidation.
- Edge-local node suppressed all five unsafe query actions immediately.
- Fleet-global node received five delayed decisions with lag >= 250 ms.
- Fleet-global audit JOIN exposed all five misleading hard distractors.
- Edge ASOF, fleet JOIN, fleet `ROW_NUMBER`, and fleet `LAG` checks passed.

## Interpretation

The experiment validates the product thesis for Physical AI memory separation:
the robot/edge path owns immediate safety, while the fleet path owns slower
global learning and audit.

The result also sharpens the next engineering question. The memory primitive is
useful, but production needs an explicit edge-to-fleet consolidation path with
idempotency, ordering, late-arrival handling, and loss/duplicate telemetry.

## Next Product Or Research Step

Replace harness-driven consolidation with a bounded edge-to-fleet feed or
replication path, then test:

- dropped consolidation rows,
- duplicate consolidation rows,
- late-arriving fleet audit evidence,
- edge operation during fleet node outage,
- fleet replay after restart.

Status update: Experiment 016 completed this research step with an explicit
bounded edge outbox, fleet inbox, ACK, telemetry, duplicate, dropped, late,
outage, and restart replay harness. The next step is runtime connector
promotion, not another direct harness copy.
