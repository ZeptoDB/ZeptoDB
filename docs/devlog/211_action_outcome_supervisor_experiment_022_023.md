# 211: Action-Outcome Supervisor Experiment 022/023 Closure

Date: 2026-07-09
Status: Complete

## Context

Devlog 210 completed P0 hardening for the experimental Physical AI
Action-Outcome supervisor, but left two promotion questions open: broader
node-replacement validation and a product decision on whether the
supervisor-specific commit-ledger sink contract was sufficient for the current
shadow path.

## Changes

- Added a node-replacement regression for the SQL-backed supervisor adapter.
  - Node A commits one proposal under a managed ownership lease.
  - Node B takes over an expired lease with a higher epoch.
  - A stale node A runtime is fenced and observes no work.
  - Restarted node B completes the remaining proposal stream.
- Added a commit-ledger stress regression.
  - Six bounded passes insert 12 total proposals.
  - Three malformed evidence projection faults are injected.
  - Fresh runtime objects repair committed state after schema restoration.
  - Commit, decision, and evidence proposal ids remain unique after each pass.
- Added Experiment 022 and 023 procedure documents plus immutable result
  summaries under `docs/research/results/`.
- Updated design, governance, backlog, and completed-feature docs to reflect
  that these two P0 validation items are closed for controlled shadow pilots.

## Verification

```bash
ninja -C build -j$(nproc) zepto_tests

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.WorkerLeaseRollingRestartNodeReplacementFencesStaleOwnerAndConverges:ActionOutcomeSqlAdapterTest.CommitLedgerStressRepairsProjectionFailuresAcrossRestarts' \
  --gtest_brief=1

./build/tests/zepto_tests \
  --gtest_filter='ActionOutcomeSqlAdapterTest.*:ActionOutcomeSqlAdapterConfigTest.*' \
  --gtest_brief=1

cd build && ctest -j$(nproc) -E "Benchmark\.|K8s" \
  --output-on-failure --timeout 180

./tools/run-full-matrix.sh --stages=8 --force-resync

ZEPTO_S3_TEST_BUCKET=<temporary-bucket> \
  ./build/tests/zepto_tests --gtest_filter='S3Sink.*' --gtest_brief=1
```

Results:

- Build: pass on local x86_64.
- Focused Experiment 022/023 regressions: pass, 2/2.
- Action-Outcome SQL adapter suite: pass, 14/14.
- x86_64 CTest gate: pass, 1712/1712; 3 perf tests disabled and 1 live S3
  opt-in skipped.
- aarch64 stage 8 CTest gate: pass, 1712/1712; 3 perf tests disabled and 1
  live S3 opt-in skipped.
- Live S3 opt-in smoke: pass, 2/2, using temporary bucket
  `zeptodb-codex-s3-smoke-060795905711-20260709133343-522201`; bucket cleanup
  was verified gone in `ap-southeast-1`.

## Follow-ups

- Keep the runtime experimental and shadow-only until an explicit product
  promotion decision is made.
- Treat the SQL ownership lease as a supervisor-specific fencing guard, not a
  consensus election subsystem.
- Keep generic multi-table SQL transactions as a separate future capability;
  they are not required for the current supervisor-specific commit-ledger sink
  contract.
