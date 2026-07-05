# 209: Action-Outcome Supervisor Config Persistence

Date: 2026-07-04
Status: Complete

## Context

Devlog 208 proved that persisted decision rows are the duplicate-suppression
boundary for the SQL-backed Action-Outcome supervisor. The next durability gap
was the adapter configuration itself: after a real HTTP server restart, the
server still needed to reinstall SQL-backed hooks without hand-wiring them
again in process memory.

## Experimental Boundary

- Classification: experimental runtime path.
- Scope: server-local durable config for the experimental SQL adapter only.
- Persistence format: versioned JSON written through a temp file and rename.
- Restore behavior: when embedding code enables persistence, an existing file
  is loaded, the SQL adapter is reinstalled, default tables may be recreated
  idempotently, and the supervisor is restarted when the persisted config says
  `enabled=true`.
- Disable behavior: `DELETE /admin/action-outcome-supervisor` removes the
  persisted SQL adapter config file when persistence is enabled.
- Non-goals: no catalog-wide config state, no cluster ownership election, no
  transactional evidence/decision sink, and no actuator-enforcement semantics.

## Changes

- Added
  `HttpServer::set_action_outcome_supervisor_config_persistence(path)`.
- Persisted the SQL adapter config after successful
  `POST /admin/action-outcome-supervisor` calls with
  `sql_adapter_enabled=true`.
- Persisted runtime fields, SQL adapter table/column fields, adapter limits,
  `enabled`, and `sql_adapter_create_tables`.
- Added load-time validation so corrupt or unsupported config files fail before
  hooks are installed.
- Stopped experimental workers from `HttpServer::stop()` so a server restart
  does not leave background lifecycle work running behind a stopped listener.
- Added a live HTTP restart regression:
  `MetricsProviderTest.ActionOutcomeSupervisorSqlAdapterConfigPersistsAndReloadsAfterHttpRestart`.
  The test stores SQL adapter config, stops the HTTP server object, creates a
  new server object on the same port and executor, loads the persisted config,
  verifies `worker_hooks_configured=true`, then enables the supervisor without
  reposting `sql_adapter_enabled`. The proposal is processed only if the SQL
  hooks were automatically reinstalled.
- Added a focused empty-path validation regression.

## Verification

```bash
cmake --build build --target zepto_tests -j$(nproc)

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.ActionOutcomeSupervisorSqlAdapterConfigPersistsAndReloadsAfterHttpRestart'

./build/tests/zepto_tests \
  --gtest_filter='MetricsProviderTest.ActionOutcomeSupervisor*:ActionOutcomeSqlAdapterTest.*:ActionOutcomeSqlAdapterConfigTest.*'
```

Results:

- Build: pass.
- Live HTTP restart persistence regression: pass, 1/1.
- Focused Action-Outcome supervisor/admin/SQL adapter regression: pass, 11/11.

## Follow-ups

- Move durable supervisor config from server-local file persistence into a
  catalog/cluster-aware control plane before product promotion.
- Add cluster-safe worker ownership so only one node processes a proposal
  shard.
- Add idempotent evidence keys or transaction-like decision/evidence writes.
- Add node-replacement and long-running soak/fault validation.
- Run cross-architecture verification before declaring the path promoted.
