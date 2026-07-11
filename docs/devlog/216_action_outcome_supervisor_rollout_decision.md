# 216: Action-Outcome Supervisor Rollout Decision

Date: 2026-07-11
Status: Complete

## Context

The Action-Outcome supervisor had enough runtime hardening for controlled
shadow pilots, but its product decision was still documented as an open choice
between a controlled pilot and a promoted operator feature.

## Changes

- Added `rollout_stage` to the supervisor runtime config and status snapshot.
- Accepted only `controlled_shadow_pilot`; `promoted_operator_feature` now fails
  validation until GA/operator gates are deliberately reopened.
- Persisted the rollout stage in server-local and SQL catalog-backed SQL
  adapter config JSON.
- Exposed the stage in `GET /admin/action-outcome-supervisor` and
  `/metrics`.
- Updated HTTP, C++, design, governance, backlog, and completion docs to keep
  public positioning shadow-only.

## Verification

- Added focused runtime, SQL-adapter validation, HTTP admin, persistence, and
  metrics regressions.
- Full x86_64 CTest:
  `ninja -C build -j$(nproc) zepto_tests && cd build && ctest -j$(nproc) -E "Benchmark\\.|K8s" --output-on-failure --timeout 180`
  - 1742/1742 passed; live S3 opt-in skipped.
- Full aarch64 Graviton stage:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`
  - 1742/1742 passed; live S3 opt-in skipped.

## Follow-ups

- Do not promote this to an operator feature until the release branch records a
  new GA gate decision, public docs, operator runbook, and passing GitHub
  Actions.
