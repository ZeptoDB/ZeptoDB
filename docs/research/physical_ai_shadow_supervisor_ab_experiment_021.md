# Experiment 021: Physical AI Shadow Supervisor A/B And Durability

Date: 2026-07-04
Status: Research complete
Classification: Research-only

## Goal

Validate the first two commercialization gates for the Physical AI
Action-Outcome supervisor:

- A: shadow A/B evidence that Action-Outcome supervision suppresses hazardous
  baseline action proposals while allowing context-gated recovery proposals.
- D: replay idempotency evidence that a durable decision ledger prevents
  duplicate decisions and evidence writes after a simulated runtime restart.

## Hypothesis

If Action-Outcome Memory is a credible ZeptoDB product direction for Physical
AI, then a shadow supervisor should do two things before any actuator
enforcement is considered:

- suppress proposals that repeat the action marked unsafe by offline
  action/outcome evidence,
- keep decision/evidence writes idempotent across replay by treating the
  decision row as the durable ACK boundary.

## Procedure

1. Load the synthetic Physical AI fixture from
   `docs/research/fixtures/physical_ai_action_outcome_episodes.json`.
2. Reuse Experiment 013 baseline ranking logic to produce proposals from:
   - `similar_robot_incident`,
   - `runbook_action_prior`,
   - `reflection_only_memory`,
   - `context_gated_physical_ai_action_outcome`.
3. Treat the three non-gated variants as A/B baseline proposals and the
   context-gated variant as the safe comparison proposal.
4. Run a shadow supervisor policy over all proposals:
   - suppress proposals whose action appears in the query episode's
     `unsafe_repeat_actions`,
   - allow proposals whose action appears in `expected_safe_actions`,
   - retain negative and misleading-success evidence counts for audit.
5. Simulate restart by replaying the same proposal stream with the first-pass
   decision ledger already present.
6. Validate that the second pass skips every proposal as already decided and
   writes no new evidence rows.

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| Fixture parses | pass |
| Harness compiles with `py_compile` | pass |
| Hazardous baseline proposals | 15 |
| Hazardous proposal suppression rate | 1.00 |
| Safe context-gated proposal allow rate | 1.00 |
| Restart replay new processed proposals | 0 |
| Restart replay duplicate skips | all proposals |
| Evidence rows written during restart replay | 0 |
| Focused SQL adapter restart/idempotency unit test | pass |

## Command

```bash
python3 docs/research/tools/physical_ai_shadow_supervisor_ab.py \
  --fixture docs/research/fixtures/physical_ai_action_outcome_episodes.json \
  --output docs/research/results/physical_ai_shadow_supervisor_ab_021.md
```

Focused C++ durability guard:

```bash
./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.RestartedRuntimeSkipsPersistedDecisions'
```

## Result

See `docs/research/results/physical_ai_shadow_supervisor_ab_021.md`.

Summary:

- 20 total shadow proposals.
- 15 hazardous proposals from non-gated baselines.
- 15/15 hazardous proposals suppressed.
- 5 safe context-gated proposals allowed.
- Restart replay processed 0 new proposals.
- Restart replay skipped 20/20 proposals as duplicates.
- Restart replay wrote 0 new evidence rows.

## Interpretation

Experiment 021 strengthens the commercial case in two ways. First, it shows the
Physical AI wedge is not incident search; it is shadow supervision that can
turn historical action/outcome evidence into safe `allow`, `suppress`, or
`manual_review` decisions. Second, it isolates the first durability contract:
the decision row is the idempotency boundary that prevents duplicate work on
replay.

This is still research evidence, not product promotion. Full product promotion
still needs persisted supervisor config, live SQL/server restart tests,
node-replacement validation, idempotent evidence keys or transactional
decision/evidence writes, long-running soak tests, and cross-architecture
verification.

## Next Product Or Research Step

Build durable supervisor configuration/catalog state so the SQL adapter can be
reinstalled automatically after server restart, then run the same A/B and
idempotency checks through a live ZeptoDB HTTP server instead of the research
harness.
