# 139: Cross-Arch Ingest And Rebalance Verification Fixes

Date: 2026-05-29
Status: Complete

## Context

Cross-architecture verification exposed an aarch64-only data-loss race in
multi-drain ingest and several flaky TCP/rebalance tests under high-parallel
CTest runs.

## Changes

- Added per-partition write locking around schema initialization and
  `ColumnVector` append paths so multiple drain threads cannot concurrently
  append into the same partition.
- Protected `PartitionManager` map reads and writes with shared/exclusive
  locking.
- Added a same-symbol multi-drain regression test.
- Replaced arithmetic TCP test port offsets in `test_coordinator.cpp` with a
  per-process ephemeral port map.
- Made rebalance pause/resume tests deterministic by using explicit multi-move
  plans, bandwidth throttling, and enough rows to keep a move in flight while
  pause is observed.

## Verification

- Targeted x86_64:
  - `MultiDrain.FourDrainThreads_MultiSymbol` and
    `MultiDrain.FourDrainThreads_SameSymbolPreservesAllRows` repeated 20 times.
  - TCP/migration/multi-drain targeted filter passed outside the sandbox.
  - Rebalance pause/resume targeted filter passed outside the sandbox.
- Full cross-architecture matrix:
  - `./tools/run-full-matrix.sh --stages=1,2,8 --force-resync`
  - x86_64: 100% passed, 0 failed out of 1502; 3 disabled perf tests and 1
    opt-in S3 upload skipped.
  - aarch64: 100% passed, 0 failed out of 1502; same disabled/skipped set.
  - Logs: `/tmp/zepto_full_matrix_20260529_150318`.

## Follow-ups

- None.
