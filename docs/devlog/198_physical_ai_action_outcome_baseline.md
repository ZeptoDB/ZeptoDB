# 198: Physical AI Action-Outcome Baseline

Date: 2026-06-23
Status: Complete
Classification: Research-only

## Context

The AIOps Action-Outcome experiments showed that context-gated outcome reuse can
avoid unsafe repeated remediation. Physical AI has the same shape, but the
context is robot-specific: temporal motifs, payload, zone, human proximity,
sensor state, and environment changes decide whether a prior action is safe to
reuse.

## Changes

- Added `docs/research/fixtures/physical_ai_action_outcome_episodes.json`.
  - Covers AGV dock slip, LiDAR occlusion, robot arm torque spike, cold-chain
    temperature excursion, and drone GPS drift near a boundary.
  - Each query episode records an unsafe failed action plus expected safe
    recovery actions.
  - Adds hard distractors where the unsafe action succeeded only in a different
    safety context, such as training lanes, supervised airspace, simulated
    payloads, or no-human cells.
- Added `docs/research/tools/physical_ai_action_outcome_baseline.py`.
  - Compares `similar_robot_incident`, `runbook_action_prior`,
    `reflection_only_memory`, and
    `context_gated_physical_ai_action_outcome`.
  - Reports Top-3 safe hit, recovery Top-1 hit, risky-repeat avoidance,
    hazardous top-action rate, retrieval quality, and context-gate
    suppressions.
- Added `docs/research/physical_ai_action_outcome_experiment_013.md`.
- Generated `docs/research/results/physical_ai_action_outcome_013.md`.

## Verification

```bash
python3 -m json.tool docs/research/fixtures/physical_ai_action_outcome_episodes.json >/tmp/physical_ai_action_outcome_episodes.pretty.json

python3 -m py_compile \
  docs/research/tools/physical_ai_action_outcome_baseline.py

python3 docs/research/tools/physical_ai_action_outcome_baseline.py \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_action_outcome_013.md
```

Result summary:

| Variant | Recovery Top-1 Hit | Risky Repeat Avoidance | Hazardous Top Action |
| --- | ---: | ---: | ---: |
| `similar_robot_incident` | 0.00 | 0.00 | 1.00 |
| `runbook_action_prior` | 0.00 | 0.00 | 1.00 |
| `reflection_only_memory` | 0.00 | 0.00 | 1.00 |
| `context_gated_physical_ai_action_outcome` | 1.00 | 1.00 | 0.00 |

## Follow-ups

- Replay Experiment 013 through native ZeptoDB SQL tables and ASOF JOINs.
- Add ROS-style telemetry rows for each incident family.
- Compare edge-local memory against fleet-global memory consolidation.
