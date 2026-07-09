# Experiment 022: Physical AI Supervisor Node-Replacement Validation

Date: 2026-07-09
Status: Research complete
Classification: Experimental runtime path validation

## Goal

Validate that the experimental SQL-backed Action-Outcome supervisor can survive
a node-replacement shaped handoff without duplicate decisions, lost proposals,
or stale-owner work.

## Hypothesis

If the SQL catalog/config and managed ownership lease are sufficient for
controlled shadow pilots, then a replaced supervisor owner should be fenced
after a new owner takes over an expired lease, while the new owner continues
processing the remaining bounded proposal stream idempotently.

## Procedure

1. Create the default Action-Outcome SQL contract tables.
2. Configure node A with:
   - `worker_ownership_required=true`
   - `worker_lease_enabled=true`
   - `worker_owner_id=node-a`
   - `batch_limit=1`
   - a bounded proposal query limit.
3. Insert three proposals and positive historical evidence for their action.
4. Run node A once and verify exactly one proposal is committed.
5. Expire node A's SQL lease to simulate replacement after a stopped runtime.
6. Start node B with the same supervisor name and a different owner id.
7. Verify node B takes ownership with a higher epoch and processes the next
   proposal while repairing/skipping the committed prefix.
8. Restart a stale node A object and verify it idles with no proposals.
9. Restart node B and verify the remaining proposal converges.
10. Verify decision, evidence, and commit rows are complete and unique.

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| Node A first pass commits one proposal | pass |
| Expired node A lease can be taken over by node B | pass |
| Replaced node A is fenced after node B takeover | pass |
| Restarted node B processes the remaining proposal | pass |
| Final owner is node B with epoch 2 | pass |
| Decision, evidence, and commit row counts converge to 3 | pass |
| Commit proposal ids are unique | pass |

## Command

```bash
./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.WorkerLeaseRollingRestartNodeReplacementFencesStaleOwnerAndConverges' \
  --gtest_brief=1
```

## Result

See `docs/research/results/physical_ai_supervisor_node_replacement_022.md`.

Summary:

- 3 proposals committed exactly once.
- Node B took over the expired node A lease with epoch 2.
- A stale node A runtime observed 0 batch proposals and processed 0 rows.
- Decision, evidence, and commit projections all converged to 3 rows.

## Interpretation

Experiment 022 closes the remaining operational node-replacement validation
item for the current shadow-only supervisor path. The validation proves the
SQL lease/heartbeat guard can fence stale owner identities in the bounded
runtime contract. It does not turn the lease into a general consensus or leader
election subsystem.

## Next Product Step

Keep the supervisor experimental and shadow-only until an explicit product
promotion decision is made. If the runtime is widened beyond controlled pilots,
document operator rollout, rollback, and multi-node lease monitoring policy.
