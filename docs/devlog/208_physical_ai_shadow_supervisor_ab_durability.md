# 208: Physical AI Shadow Supervisor A/B And Durability

Date: 2026-07-04
Status: Complete

## Context

The next commercialization step after the SQL-backed Action-Outcome supervisor
adapter is evidence, not a wider API. The product thesis needs proof that
shadow supervision improves over non-gated action proposals and that the
decision row acts as a durable idempotency boundary after replay or runtime
restart.

## Changes

- Added Experiment 021:
  - `docs/research/physical_ai_shadow_supervisor_ab_experiment_021.md`
  - `docs/research/tools/physical_ai_shadow_supervisor_ab.py`
  - `docs/research/results/physical_ai_shadow_supervisor_ab_021.md`
- The harness reuses Experiment 013 baseline ranking to generate 20 shadow
  proposals:
  - 15 hazardous proposals from non-gated baselines,
  - 5 safe proposals from the context-gated Action-Outcome variant.
- The harness validates two gates:
  - A: all hazardous baseline proposals are suppressed and all safe
    context-gated proposals are allowed,
  - D: a simulated restart replay skips all proposals from the first-pass
    decision ledger and writes no new evidence rows.
- Added `ActionOutcomeSqlAdapterTest.RestartedRuntimeSkipsPersistedDecisions`
  to verify that a fresh runtime object using the same SQL tables skips
  proposals that already have persisted decision rows.

## Verification

```bash
python3 -m py_compile docs/research/tools/physical_ai_shadow_supervisor_ab.py

python3 docs/research/tools/physical_ai_shadow_supervisor_ab.py \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_shadow_supervisor_ab_021.md

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.RestartedRuntimeSkipsPersistedDecisions'
```

Results:

- Python harness compile: pass.
- Experiment 021 harness: pass.
- Experiment 021 result:
  - 20 total shadow proposals,
  - 15/15 hazardous proposals suppressed,
  - 5/5 safe context-gated proposals allowed,
  - restart replay processed 0 new proposals,
  - restart replay skipped 20/20 proposals as duplicates,
  - restart replay wrote 0 new evidence rows.
- Focused C++ durability test: pass, 1/1.

## Follow-ups

- Persist supervisor SQL adapter config in catalog/config state so hooks are
  reinstalled automatically after full server restart.
- Run the same A/B and idempotency checks through a live ZeptoDB HTTP server.
- Add idempotent evidence keys or transaction-like decision/evidence writes.
- Add node-replacement and long-running soak validation before product
  promotion.
