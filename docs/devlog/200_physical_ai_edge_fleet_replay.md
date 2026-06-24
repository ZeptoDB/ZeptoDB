# 200: Physical AI Edge/Fleet Replay

Date: 2026-06-23
Status: Complete
Classification: Research-only

## Context

Experiment 014 validated Physical AI Action-Outcome replay through a single
live ZeptoDB SQL endpoint. The next research step was to split the memory loop
into an edge-local safety path and a fleet-global audit path.

## Changes

- Added `docs/research/tools/physical_ai_edge_fleet_replay.py`.
  - Starts from the Experiment 013/014 fixture and scoring helpers.
  - Materializes edge-local incident, expected-action, robot-state,
    sensor-summary, decision, and suppression tables.
  - Materializes fleet-global expected-action, action-outcome, delayed
    edge-decision, retrieval, and suppression audit tables.
  - Validates edge-local immediate recovery, risky-action suppression, robot
    ASOF JOIN, and sensor ASOF JOIN.
  - Validates fleet-global delayed consolidation, recovery JOIN, suppression
    audit JOIN, retrieval `ROW_NUMBER`, and consolidation-lag `LAG`.
- Added `docs/research/physical_ai_edge_fleet_replay_experiment_015.md`.
- Generated:
  - `docs/research/results/physical_ai_edge_fleet_replay_015.md`
  - `docs/research/results/physical_ai_edge_fleet_replay_015_edge.sql`
  - `docs/research/results/physical_ai_edge_fleet_replay_015_fleet.sql`
- Updated Action-Outcome research tracking docs.

## Verification

```bash
python3 -m py_compile \
  docs/research/tools/physical_ai_action_outcome_baseline.py \
  docs/research/tools/physical_ai_sql_replay.py \
  docs/research/tools/physical_ai_edge_fleet_replay.py

python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

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

Result summary:

| Check | Status |
| --- | --- |
| Edge row counts | pass |
| Fleet row counts | pass |
| Edge immediate recovery | pass |
| Edge risky-action suppression | pass |
| Edge robot ASOF | pass |
| Edge sensor ASOF | pass |
| Fleet consolidated recovery | pass |
| Fleet suppression audit JOIN | pass |
| Fleet consolidation lag | pass |
| Fleet ROW_NUMBER | pass |
| Fleet LAG | pass |
| Overall edge/fleet replay | pass |

The edge node stored 82 research rows; the fleet node stored 87 research rows.

## Follow-ups

- Replace harness-driven edge-to-fleet consolidation with an explicit bounded
  feed or replication path.
- Test dropped, duplicated, and late consolidation rows.
- Test edge operation while the fleet node is unavailable.
- Test fleet audit replay after restart.
