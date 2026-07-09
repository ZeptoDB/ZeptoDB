# Physical AI Supervisor Node-Replacement Results

Generated at: 2026-07-09T13:24:21Z
Classification: Experimental runtime path validation

## Purpose

Experiment 022 validates the SQL-backed Action-Outcome supervisor under a
node-replacement shaped sequence: node A processes work, its lease expires,
node B takes over, stale node A is fenced, and restarted node B finishes the
proposal stream.

## Summary

| Metric | Value |
| --- | ---: |
| Total proposals | 3 |
| Final commit rows | 3 |
| Final decision rows | 3 |
| Final evidence rows | 3 |
| Stale node A batch proposals | 0 |
| Stale node A processed rows | 0 |
| Final owner epoch | 2 |

## Ownership Handoff

| Step | Owner | Epoch | Processed | Duplicate Skips | Result |
| --- | --- | ---: | ---: | ---: | --- |
| Initial pass | node-a | 1 | 1 | 0 | first proposal committed |
| Replacement pass | node-b | 2 | 1 | >=1 | expired lease taken over |
| Stale probe | node-a | 1 | 0 | 0 | fenced / idle |
| Restarted owner | node-b | 2 | 1 | >=1 | final proposal committed |

## Acceptance

| Criterion | Status |
| --- | --- |
| node A first pass commits one proposal | pass |
| expired lease takeover promotes node B | pass |
| stale node A processes no work after takeover | pass |
| restarted node B completes remaining proposal | pass |
| commit rows converge to 3 | pass |
| decision rows converge to 3 | pass |
| evidence rows converge to 3 | pass |
| commit proposal ids remain unique | pass |

## Verification

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.WorkerLeaseRollingRestartNodeReplacementFencesStaleOwnerAndConverges' \
  --gtest_brief=1
```

Result: pass, 1/1 focused test.

Broader gates:

- x86_64 CTest: pass, 1712/1712.
- aarch64 stage 8 CTest: pass, 1712/1712.
- Live S3 opt-in smoke: pass, 2/2; temporary bucket cleanup verified gone.

## Interpretation

The supervisor-specific SQL lease is sufficient to fence a stale owner in the
validated shadow runtime flow. This closes the operational node-replacement
validation item for controlled pilots, while preserving the documented limit:
the SQL lease is not a general consensus election protocol.
