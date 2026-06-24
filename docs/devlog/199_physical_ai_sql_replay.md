# 199: Physical AI SQL Replay

Date: 2026-06-23
Status: Complete
Classification: Research-only

## Context

Experiment 013 showed that context-gated Physical AI Action-Outcome Memory can
avoid repeating unsafe robot actions in a Python fixture. The next question was
whether that evidence could be replayed through live ZeptoDB SQL tables that
look more like a robot or fleet operating system: incidents, robot state,
sensor summaries, poses, actions, outcomes, recommendations, retrievals, and
suppression audits.

## Changes

- Added `docs/research/tools/physical_ai_sql_replay.py`.
  - Reuses the Experiment 013 fixture and baseline harness.
  - Creates nine native ZeptoDB SQL tables for Physical AI replay.
  - Materializes robot/fleet-shaped telemetry, action/outcome history,
    recommendation rankings, retrieval evidence, and suppression rows.
  - Validates row counts, failed-repeat JOIN, context-gated recovery JOIN,
    suppression audit JOIN, action/outcome JOIN, robot state ASOF JOIN, sensor
    ASOF JOIN, `ROW_NUMBER`, `LAG`, and `ST_Within`.
  - Stores semantic strings plus stable integer codes where the current native
    ASOF projection path is integer-oriented.
- Added `docs/research/physical_ai_action_outcome_sql_replay_experiment_014.md`.
- Generated:
  - `docs/research/results/physical_ai_action_outcome_sql_replay_014.md`
  - `docs/research/results/physical_ai_action_outcome_sql_replay_014.sql`
- Updated Action-Outcome research tracking docs.

## Verification

```bash
ninja -C build -j$(nproc) zepto_http_server

python3 -m py_compile \
  docs/research/tools/physical_ai_action_outcome_baseline.py \
  docs/research/tools/physical_ai_sql_replay.py

python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

./build/zepto_http_server --port 19341 --no-auth --storage-mode pure

python3 docs/research/tools/physical_ai_sql_replay.py \
  --url http://127.0.0.1:19341/ \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_action_outcome_sql_replay_014.md \
  --sql-output docs/research/results/physical_ai_action_outcome_sql_replay_014.sql \
  --timeout 10
```

Result summary:

| Check | Status |
| --- | --- |
| Row counts | pass |
| Failed-repeat JOIN | pass |
| Context top-action JOIN | pass |
| Suppression audit JOIN | pass |
| Action/outcome JOIN | pass |
| Robot state ASOF JOIN | pass |
| Sensor ASOF JOIN | pass |
| ROW_NUMBER window | pass |
| LAG window | pass |
| ST_Within geofence | pass |
| Overall SQL replay | pass |

## Follow-ups

- Port the Physical AI replay into a two-node live topology.
- Split edge-local immediate safety memory from fleet-global consolidation.
- Decide whether a future runtime/API change is needed for richer string
  projection in declared-table ASOF joins; keep that separate from this
  research-only experiment.
