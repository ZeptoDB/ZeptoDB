# 213: Physical AI edge/fleet production hardening

Date: 2026-07-09
Status: Complete

## Context

Devlog 212 added the server-owned SQL/HTTP adapter for the experimental
Physical AI edge/fleet connector. The remaining controlled-pilot blockers were
durable config/cursor state, explicit idempotent sink docs, restart/fault
coverage, bounded SQL/backpressure controls, and admin audit coverage.

## Changes

- Added `HttpServer::set_edge_fleet_connector_config_persistence()` for
  versioned file persistence of successful SQL/HTTP adapter configs. Restart
  reloads the config, reinstalls SQL/HTTP hooks, recreates local default tables
  when requested, and starts the connector when the persisted config was
  enabled.
- Kept ACK/cursor state as a separate `checkpoint_path` concern and added HTTP
  restart coverage proving checkpoint reload skips already ACKed outbox rows,
  pages past ACKed prefixes under small SQL limits, and avoids duplicating fleet
  ACK/inbox rows.
- Hardened runtime limits with `max_failures_per_pass` and `retry_backoff_ms`,
  and hardened the SQL adapter with positive `outbox_query_limit`,
  ACK-ledger paging, and `max_outbox_bytes` for decoded outbox cells.
- Added mutating admin audit events for edge/fleet configure/start/failure/
  clear outcomes and regression coverage for audit-buffer and rate-limit
  behavior.
- Documented the idempotent sink contract: stable `feed_event_id`, ACK table as
  durable delivery ledger, `AppliedButAckFailed` replay behavior, and
  projection dedupe expectations for append-only bootstrap tables.

## Verification

- `ninja -C build -j$(nproc) zepto_tests`
- `./build/tests/zepto_tests --gtest_filter='EdgeFleetFeedConnectorTest.*:EdgeFleetConnectorRuntimeTest.*:EdgeFleetSqlHttpAdapterTest.*:MetricsProviderTest.EdgeFleetConnector*:EdgeFleetConnectorAdminAuthTest.*' --gtest_brief=1`
  — 33/33 passed.
- `ctest -j$(nproc) -E "Benchmark\.|K8s" --output-on-failure --timeout 180`
  from `build/` — 0 failed out of 1724 run tests; 3 perf tests disabled and
  live S3 opt-in skipped.
- `./tools/run-full-matrix.sh --stages=8 --force-resync` — aarch64 Graviton
  SSH stage passed in 175s with 0 failed out of 1724 run tests; 3 perf tests
  disabled and live S3 opt-in skipped. Logs:
  `/tmp/zepto_full_matrix_20260709_162340`.
- `ZEPTO_S3_TEST_BUCKET=<temporary-bucket> ./build/tests/zepto_tests
  --gtest_filter='S3Sink.*' --gtest_brief=1` — 2/2 passed against a temporary
  S3 bucket in `ap-southeast-1`; recursive cleanup, bucket deletion, and
  post-delete `head-bucket` failure verified the bucket was gone/unusable.

## Follow-ups

- Run long-duration soak/fault validation over the server runtime.
- Add node-replacement evidence over live server-managed edge/fleet tables.
- Keep the connector experimental until the GA/operator rollout decision is
  recorded.
