# 215: Physical AI Edge/Fleet Live Promotion Validation

Date: 2026-07-11
Status: Complete

## Context

Devlogs 212-213 moved the Physical AI edge/fleet connector from a standalone
replay path to a server-owned SQL/HTTP runtime with durable config, checkpoint
reload, idempotent sink docs, bounded SQL loads, backpressure controls, and
admin audit coverage. The remaining production evidence was server-runtime
soak/restart behavior and node-replacement convergence over live edge/fleet
tables.

## Changes

- Fixed remote SQL/HTTP ACK lookup in `EdgeFleetSqlHttpAdapterState`: ACK checks
  now narrow by numeric `stream_seq` and compare `feed_event_id` client-side.
  This avoids remote string-column comparison drift while preserving
  `feed_event_id` as the stable idempotency key.
- Added a server-runtime restart soak regression over SQL contract tables. The
  test repeatedly recreates the runtime with the same checkpoint and ACK ledger
  until 24 mixed decision/retrieval/suppression events converge.
- Added a node-replacement regression where node A processes the first ACK
  prefix and a replacement runtime loads the checkpoint, pages past ACKed rows,
  and consolidates node B rows without marking them late.
- Added a two-live-HTTP-node regression: separate edge and fleet `HttpServer`
  instances serve the SQL contract tables while the built-in SQL/HTTP adapter
  uses `edge_sql_url` and `fleet_sql_url` through the server runtime.

## Verification

- `ninja -C build -j$(nproc) zepto_tests`
- `./build/tests/zepto_tests --gtest_filter='EdgeFleetSqlHttpAdapterTest.*' --gtest_brief=1`
  - 10/10 passed locally.
- Full x86_64 CTest after the remote ACK fix:
  `ninja -C build -j$(nproc) zepto_tests && cd build && ctest -j$(nproc) -E "Benchmark\\.|K8s" --output-on-failure --timeout 180`
  - 1742/1742 passed; live S3 opt-in skipped.
- Full aarch64 Graviton stage after the cluster stats port hardening:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`
  - 1742/1742 passed; live S3 opt-in skipped.
- Live S3 opt-in release smoke:
  `ZEPTO_S3_TEST_BUCKET=zeptodb-codex-s3-smoke-* ./build/tests/zepto_tests --gtest_filter='S3Sink.*' --gtest_brief=1`
  - 2/2 passed against a temporary bucket; recursive cleanup and bucket-gone
    verification completed.

## Follow-ups

- Make the explicit GA/operator rollout decision for the supported edge/fleet
  scope.
- Update public positioning once the rollout scope is approved.
- Verify GitHub Actions after the branch is pushed before promoting the feature
  out of experimental status.
