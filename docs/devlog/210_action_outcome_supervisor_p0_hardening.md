# 210: Action-Outcome Supervisor P0 Hardening

Date: 2026-07-05
Status: Complete

## Context

The Action-Outcome supervisor had reached controlled single-server durability:
SQL adapter config can survive an HTTP server restart and decision rows suppress
duplicate proposal work. The next production blockers were sink idempotency and
cluster-safe worker ownership.

## Changes

- Made SQL-backed evidence summary writes idempotent by `proposal_id`.
  - Before inserting evidence, the sink checks whether an evidence summary for
    the proposal already exists.
  - If a previous pass inserted evidence but failed before inserting the
    decision row, retry writes only the missing decision row.
- Added an optional ownership/fencing gate to `ActionOutcomeSqlAdapterConfig`.
  - `require_worker_ownership`
  - `worker_owner_id`
  - `worker_owner_epoch`
  - `ownership_table`
  - `ownership_supervisor_column`
  - `ownership_owner_id_column`
  - `ownership_epoch_column`
- Added default SQL bootstrap for
  `physical_ai_supervisor_ownership(supervisor_name, owner_id, owner_epoch)`.
- Proposal loading now fail-closes to no work when ownership is required and
  the configured worker id/epoch does not match the ownership row.
- Proposal loading now queries committed repair candidates separately from
  undecided anti-join candidates, so a committed prefix at
  `proposal_query_limit == batch_limit` cannot starve new proposals.
- Hardened `tools/run-full-matrix.sh` stage 8 so a forced Graviton resync also
  refreshes stale optional-feature CMake caches and keeps Arrow/Flight +
  Parquet coverage aligned with the x86_64 gate.
- Persisted and restored ownership fields through the durable supervisor config
  JSON path.
- Extended HTTP admin parsing so controlled pilots can configure ownership
  fencing through `POST /admin/action-outcome-supervisor`.
- Strengthened the live HTTP restart test so restored SQL hooks process work
  only when the persisted owner id/epoch matches the SQL ownership row.

## Tests

- `ActionOutcomeSqlAdapterTest.RetryAfterDecisionInsertFailureDoesNotDuplicateEvidence`
  reproduces evidence-success/decision-failure, fixes the decision table, then
  verifies retry does not duplicate evidence.
- `ActionOutcomeSqlAdapterTest.WorkerOwnershipGateSkipsNonOwnerAndAllowsCurrentOwner`
  verifies non-owner and stale epoch workers idle while the current owner
  processes the proposal.
- `ActionOutcomeSqlAdapterTest.WorkerOwnershipEpochHandoffFencesReplacedOwner`
  verifies a node-a to node-b ownership handoff fences the replaced owner and
  lets the new owner process remaining work.
- `ActionOutcomeSqlAdapterTest.UndecidedProposalsProgressPastCommittedPrefixAtQueryLimit`
  reproduces a committed prefix filling the SQL query limit, then verifies the
  next undecided proposals still commit in the following pass.
- `MetricsProviderTest.ActionOutcomeSupervisorSqlAdapterConfigPersistsAndReloadsAfterHttpRestart`
  now covers persisted ownership fields through an actual HTTP server restart.

## Verification

```bash
ninja -C build -j$(nproc)

cd build && ctest -j$(nproc) -E "Benchmark\.|K8s" --output-on-failure --timeout 180

./tools/run-full-matrix.sh --stages=8 --force-resync

ZEPTO_S3_TEST_BUCKET=<temporary-bucket> \
  ./build/tests/zepto_tests --gtest_filter='S3Sink.*' --gtest_brief=1

PYTHONPATH=/home/ec2-user/zeptodb python3 -m pytest -v

cd web && pnpm test && pnpm lint
```

Results:

- Build: pass on x86_64 and aarch64.
- Focused Action-Outcome regressions: pass, 22/22.
- SQL-backed soak/fault harness:
  `zepto_action_outcome_soak --iterations 2 --proposals-per-pass 4
  --batch-limit 4 --proposal-query-limit 4 --sleep-ms 0 --fault-every 0`
  pass.
- x86_64 CTest gate: 1710/1710 passed, 3 disabled, 1 live S3 opt-in skipped.
- aarch64 CTest gate: 1710/1710 passed, 3 disabled, 1 live S3 opt-in skipped.
  The Graviton cache was refreshed with `ZEPTO_USE_FLIGHT=ON` and
  `ZEPTO_USE_PARQUET=ON`, so Arrow/Flight and Parquet coverage matched x86_64.
- Live S3 opt-in smoke: pass, 2/2, using temporary bucket
  `zeptodb-codex-s3-smoke-060795905711-20260705060441-1057100`; object and
  bucket cleanup completed.
- Python: pass, 241/241.
- Web: `pnpm test` and `pnpm lint` pass.
- Integration shell tests: HTTP HDB, HTTP tenant, and multiprocess pass.
- Extra binaries: `test_feeds` pass 23/23, `test_migration` pass 131/131.
- Disabled perf tests were manually run and pass: Kafka, MQTT, and OPC-UA
  single-thread hot paths.

## Open Production Issues

- SQL worker lease/heartbeat fencing is now built in, but it is still not a
  full consensus election subsystem.
- The commit ledger makes the supervisor sink retry-idempotent and repairable,
  but it is not a generic multi-table SQL transaction primitive.
- Broader operational node-replacement validation is still recommended before
  removing the experimental/shadow-only label.
