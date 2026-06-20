# 189: Action-Outcome JOIN/Window Replay Acceptance

Date: 2026-06-18
Status: Complete

## Context

Experiments 007 and 008 validated Action-Outcome replay through live single-node
and distributed ZeptoDB endpoints. Devlog 188 added a C++ regression for the
distributed typed-row string boundary. The next research risk was whether the
replay contract could move beyond row counts and simple WHERE filters into
JOIN/window acceptance.

## Changes

- Added `docs/research/tools/action_outcome_join_window_replay.py`.
  - Loads the Action-Outcome SQL seed into a live ZeptoDB endpoint.
  - Builds an in-memory SQLite control from the same SQL seed.
  - Derives query-level controls and recommendation rows.
  - Creates numeric acceptance projection tables with explicit `symbol` and
    `timestamp_ns` columns.
  - Validates numeric JOIN semantics over query controls and recommendations.
  - Validates ROW_NUMBER/LAG window semantics over recommendation ranking.
  - Probed the native string-key JOIN boundary; devlog 190 closes that
    boundary and updates the generated Experiment 009 result to pass.

- Added Experiment 009 documentation and generated results:
  - `docs/research/action_outcome_join_window_replay_experiment_009.md`
  - `docs/research/results/action_outcome_join_window_replay_009.md`

- Updated research tracking:
  - `docs/research/action_outcome_research_process_log.md`
  - `docs/BACKLOG.md`
  - `docs/COMPLETED.md`

## Verification

Syntax and live harness:

```bash
python3 -m py_compile docs/research/tools/action_outcome_join_window_replay.py

python3 docs/research/tools/action_outcome_join_window_replay.py \
  --url http://127.0.0.1:19341/ \
  --output docs/research/results/action_outcome_join_window_replay_009.md \
  --timeout 10
```

Result summary:

- 203/203 seed SQL statements loaded.
- Seed row-count verification passed.
- Projection row-count verification passed.
- Native string-window validation passed.
- Numeric JOIN/window acceptance passed.
- Initial native string-key JOIN probe remained blocked under the then-current
  hash JOIN executor. Devlog 190 adds the C++ regression and fix.

No C++ source changed in this devlog, so the full C++ suite was not rerun for
this documentation/tooling-only acceptance step.

## Follow-ups

- See devlog 190 for the completed string-key JOIN materialization fix.
- Port Experiment 009 into the distributed two-node harness after single-node
  alias-aware JOIN predicate handling is scoped.
