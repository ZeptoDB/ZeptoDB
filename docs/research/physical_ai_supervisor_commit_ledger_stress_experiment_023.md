# Experiment 023: Physical AI Supervisor Commit-Ledger Stress

Date: 2026-07-09
Status: Research complete
Classification: Experimental runtime path validation

## Goal

Stress the Action-Outcome supervisor's supervisor-specific commit-ledger sink
contract under repeated projection failures, fresh runtime objects, bounded
batches, and repair passes.

## Hypothesis

If the atomic commit ledger is sufficient for the current supervisor sink
contract, then projection failures after commit should be repairable without
duplicating decision or evidence rows, even when each repair pass is performed
by a fresh runtime object.

## Procedure

1. Create the default proposal, history, decision, evidence, commit, and
   ownership tables.
2. Configure the runtime with `batch_limit=2`,
   `max_sink_errors_per_pass=1`, and a bounded proposal query limit.
3. Insert one positive and one negative historical outcome.
4. Run six passes; before each pass, insert two new proposals.
5. On every odd pass:
   - replace the evidence projection with a malformed table,
   - run a fresh runtime and require the pass to fail on evidence projection
     repair,
   - restore the projection schema,
   - run another fresh runtime to repair committed state and process remaining
     proposals.
6. After every pass, verify commit, decision, and evidence row counts and
   proposal-id uniqueness.

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| Six bounded passes complete | pass |
| Three projection faults are injected | pass |
| Faulted passes fail closed through the sink error budget | pass |
| Repair passes converge after schema restoration | pass |
| Final commit rows equal total proposals | pass |
| Final decision rows equal total proposals | pass |
| Final evidence rows equal total proposals | pass |
| Commit, decision, and evidence proposal ids remain unique | pass |

## Command

```bash
./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.CommitLedgerStressRepairsProjectionFailuresAcrossRestarts' \
  --gtest_brief=1
```

## Result

See `docs/research/results/physical_ai_supervisor_commit_ledger_stress_023.md`.

Summary:

- 6 passes completed.
- 12 proposals converged.
- 3 malformed-projection faults were injected and repaired.
- Commit, decision, and evidence tables each ended with 12 unique proposal ids.

## Interpretation

Experiment 023 supports keeping the supervisor-specific commit-ledger sink
contract for the current shadow runtime. The ledger is an effectively-once
boundary for this bounded proposal/decision/evidence path, but it remains
deliberately narrower than a generic multi-table SQL transaction primitive.

## Next Product Step

Do not block the Action-Outcome supervisor shadow pilot on generic transaction
work. Track generic multi-table transactions separately if broader SQL surfaces
need them.
