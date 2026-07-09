# Physical AI Supervisor Commit-Ledger Stress Results

Generated at: 2026-07-09T13:24:21Z
Classification: Experimental runtime path validation

## Purpose

Experiment 023 validates the supervisor-specific commit-ledger sink contract
under repeated projection failures and fresh runtime restarts.

## Summary

| Metric | Value |
| --- | ---: |
| Bounded passes | 6 |
| Proposals per pass | 2 |
| Total proposals | 12 |
| Injected projection faults | 3 |
| Final commit rows | 12 |
| Final decision rows | 12 |
| Final evidence rows | 12 |

## Fault And Repair

| Fault Type | Count | Repair Path | Result |
| --- | ---: | --- | --- |
| Malformed evidence projection | 3 | Restore schema, run fresh runtime | pass |
| Sink error budget exhaustion | 3 | Fail pass, retry after repair | pass |
| Runtime restart between fault and repair | 3 | Reinstall SQL hooks from config object | pass |

## Acceptance

| Criterion | Status |
| --- | --- |
| six bounded passes complete | pass |
| three projection faults injected | pass |
| faulted passes fail through sink budget | pass |
| repair passes converge after schema restoration | pass |
| commit rows equal total proposals | pass |
| decision rows equal total proposals | pass |
| evidence rows equal total proposals | pass |
| proposal ids remain unique in all sink tables | pass |

## Verification

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.CommitLedgerStressRepairsProjectionFailuresAcrossRestarts' \
  --gtest_brief=1
```

Result: pass, 1/1 focused test.

Broader gates:

- x86_64 CTest: pass, 1712/1712.
- aarch64 stage 8 CTest: pass, 1712/1712.
- Live S3 opt-in smoke: pass, 2/2; temporary bucket cleanup verified gone.

## Interpretation

The commit ledger remains the correct effectively-once boundary for the current
Action-Outcome supervisor sink. Projection tables can be repaired from the
ledger after schema restoration without duplicate proposal ids. This does not
promote a generic multi-table SQL transaction primitive.
