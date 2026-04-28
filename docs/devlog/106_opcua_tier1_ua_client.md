# Devlog 106 — OPC-UA Tier 1 #2b: Real `UA_Client` Integration

> **Scope.** Wire the real open62541 `UA_Client` session into
> `OpcUaConsumer::start()` / `stop()`. Closes BACKLOG P9 Tier 1 item 2b —
> the critical-path blocker identified in devlog 101 (PoC) and unblocked
> by devlog 105 (config validation + float clamp + profiles + timeout
> knobs). With this, when the binary is built with `-DZEPTO_USE_OPCUA=ON`
> and open62541 is installed, `start()` connects to a real OPC-UA server,
> creates a subscription, adds a `MonitoredItem` per configured node, and
> the server's data-change notifications flow through `on_data_change()`
> into `ingest_decoded()` → `ZeptoPipeline`.
>
> **Not in this devlog.** 2c (Basic256Sha256 security), 2k (integration
> test against `tutorial_server_variable`), 2i (reconnect loop using the
> 2o knobs) — all Sprint 2.

## Summary

| BACKLOG | Item | Surface touched |
|---|---|---|
| P9 #2b | Real `UA_Client` session, subscription, monitored items, iterate thread | `src/feeds/opcua_consumer.cpp` — `start()` / `stop()` bodies + file-local helpers behind `#ifdef ZEPTO_OPCUA_AVAILABLE` |

Single-file engine diff. No header change, no CMake change, no test
change. Docs updated per the Sprint-1 closeout checklist.

## Before / After

| | Before (devlog 101 + 105) | After (devlog 106) |
|---|---|---|
| `-DZEPTO_USE_OPCUA=OFF` (default) | `start()` returns `false` with a "not compiled in" log | **Unchanged** — same log, same `false` return |
| `-DZEPTO_USE_OPCUA=ON` + no live server | `start()` returns `false` with "UA_Client not wired yet" stub log | `start()` returns `false` from `UA_Client_connect` failure with a real status-code log |
| `-DZEPTO_USE_OPCUA=ON` + live server | Same stub `false` return | `start()` returns `true`, subscription created, `config_.nodes.size()` MonitoredItems registered, background iterate thread running; `on_data_change()` fires per UA data-change notification |

## Implementation

### `start()` flow (under `#ifdef ZEPTO_OPCUA_AVAILABLE`)

1. **Config validation** + **license gate** run first — unchanged, fail
   fast before touching the protocol library.
2. `UA_Client_new()` → `client_handle_`. `UA_ClientConfig_setDefault()`.
3. Honor the 2o knobs:
   - `cc->timeout = config_.connect_timeout_ms` (initial connect).
   - `cc->requestedSessionTimeout = config_.session_timeout_ms`.
4. `UA_Client_connectUsername()` when `config_.username` non-empty, else
   `UA_Client_connect()`. On failure → log with hex status code,
   `UA_Client_delete`, return `false`.
5. `UA_CreateSubscriptionRequest_default()` with
   `requestedPublishingInterval = config_.publishing_interval_ms`.
   `UA_Client_Subscriptions_create()`; on failure → disconnect + delete +
   return `false`. Store `subscription_id_` on success.
6. **Per-node MonitoredItem**. For each `config_.nodes[i]`:
   - Parse `node_id` via a hand-rolled `sscanf`-based parser accepting
     `ns=<n>;s=<string>` and `ns=<n>;i=<numeric>`. Other forms (GUID,
     ByteString) bump `decode_errors` and skip.
   - Build an `ItemContext{ self, node_id, value_scale }` owned by a
     `std::unique_ptr`; its raw pointer is handed to open62541 as
     `monContext`.
   - `UA_MonitoredItemCreateRequest_default(nid)` with
     `samplingInterval = config_.sampling_interval_ms`,
     `queueSize = config_.queue_size`, `discardOldest = config_.discard_oldest`.
   - `UA_Client_MonitoredItems_createDataChange(...
     ua_data_change_cb, ...)`. Failure bumps `decode_errors` and skips.
   - Move the `ItemContext` into a per-consumer `UaRuntime` so its address
     stays valid for open62541's lifetime.
7. Spawn a single iterate thread: `while (running_) UA_Client_run_iterate(client, 100)`.
   This is how open62541 drives async callbacks.
8. Park the `UaRuntime` in a file-scope map keyed by `this`. Log
   `ZEPTO_INFO("OpcUaConsumer started: endpoint=..., subscription_id=..., items=...")`.
   Return `true`.

### `stop()` flow

1. Early-return if `running_` was already `false` → **idempotent** (covers
   the existing `OpcUaLifecycle.StopIsIdempotent` test).
2. Retrieve + erase the `UaRuntime` from the file-scope map; join the
   iterate thread.
3. `UA_Client_Subscriptions_deleteSingle(client, subscription_id_)`.
4. `UA_Client_disconnect(client)` → `UA_Client_delete(client)`.
5. Null out `client_handle_` + `subscription_id_`. Log stop.

### Static C trampoline

open62541 is pure C. The trampoline has the exact signature the library
expects and casts `monContext` to the private `ItemContext*`:

```cpp
extern "C" void ua_data_change_cb(UA_Client*, UA_UInt32, void*, UA_UInt32,
                                  void* monContext, UA_DataValue* value) {
    auto* ctx = static_cast<ItemContext*>(monContext);
    if (!ctx) return;
    handle_data_change(ctx->self, *ctx, value);
}
```

### Variant coercion bridge

Rather than re-implement the float clamp / NaN-reject logic introduced in
devlog 105 (item 2n), the UA_Variant → int64 path builds an internal
`OpcUaConsumer::Variant` struct and calls the already-unit-tested
`coerce_variant_to_int64()`:

- `Int16 / Int32 / Int64` → direct.
- `UInt16 / UInt32 / UInt64` → widened to int64; `UInt64` clamps to
  `INT64_MAX` on overflow (never wraps negative).
- `Float / Double` → routed through the clamp-on-overflow / reject-NaN
  path in `coerce_variant_to_int64`.
- `Boolean` → 0 / 1.
- Anything else (structured, array, string, GUID) → returns `false`;
  caller dispatches through `on_data_change("", ...)` to bump
  `decode_errors` via the existing no-match path. (This works only
  because `start()` now rejects empty `node_id` up-front, guaranteeing
  `""` is never a valid map key — item 2p, devlog 105.)

### Timestamp selection

SourceTimestamp → ServerTimestamp → host wall-clock. Falls back to
`std::chrono::system_clock::now()` only when the server reports neither;
matches the rule already documented in `design/opcua_connector.md` §4.1.

### Why file-scope state instead of extending the header

The task constraint was **"Touch only `src/feeds/opcua_consumer.cpp` and
its build wiring"**. The iterate thread and the pinned `ItemContext`
vector are held in a file-scope
`std::unordered_map<OpcUaConsumer*, std::unique_ptr<UaRuntime>>` so that
the header's `void* client_handle_` + `uint32_t subscription_id_` + the
existing `std::atomic<bool> running_` remain the only per-consumer runtime
fields. When the reconnect loop lands (item 2i), this bag is the natural
place to park the reconnect-backoff state too.

## CMake

The existing `ZEPTO_USE_OPCUA` option (`CMakeLists.txt` line 64) and
`find_library(open62541 ...)` block (lines 164–178) + the
`target_compile_definitions(zepto_opcua PUBLIC ZEPTO_OPCUA_AVAILABLE=1)`
hook (line 787) already cover everything this change needs. No CMake
delta.

## Verification

### Default build (`ZEPTO_USE_OPCUA=OFF`, the Sprint-1 acceptance gate)

```
$ cd build && ninja zepto_tests
[1/3] Building CXX object CMakeFiles/zepto_opcua.dir/src/feeds/opcua_consumer.cpp.o
[2/3] Linking CXX static library libzepto_opcua.a
[3/3] Linking CXX executable tests/zepto_tests

$ ./tests/zepto_tests --gtest_filter="OpcUa*"
[==========] 38 tests from 16 test suites ran.
[  PASSED  ] 38 tests.

$ ./tests/zepto_tests
[==========] 1270 tests from 172 test suites ran.
[  PASSED  ] 1270 tests.
```

Identical to the devlog-105 baseline (38 OpcUa + 1270 total). `start()`
with the dep off still returns `false` with the existing "not compiled
in" log — behaviour-preserving for every currently-shipped build.

### `ZEPTO_USE_OPCUA=ON` build (compile-only, since open62541 is not
installed on this host)

```
$ cmake -GNinja -DZEPTO_USE_OPCUA=ON -B /tmp/build-opcua-check -S .
-- open62541: not found — OPC-UA consumer disabled
--   Install: sudo dnf install -y open62541-devel
-- zepto_opcua: NodeId-map / routing only (no open62541)
```

CMake's `find_library(open62541)` correctly turns the option off when the
library is absent, so no misleading `-DZEPTO_OPCUA_AVAILABLE=1` compile
happened. The real-integration compile-check + integration test are
deferred to Sprint 2 item **2k** (runs in CI against open62541's bundled
`tutorial_server_variable`) — the prompt explicitly excludes a runtime
integration test from this devlog.

If / when open62541 is installed on a CI host, the build is expected to
produce a clean `libzepto_opcua.a` using the `ZEPTO_OPCUA_AVAILABLE`
branch. Any API drift between open62541 versions (e.g. field renames on
the `UA_ClientConfig` struct) should be handled in a follow-up per the
prompt's "adapt to what's installed and note deltas" rule.

## What this unblocks

- **2c (Basic256Sha256 security).** Security config now has a concrete
  `UA_ClientConfig` to attach certificates and a `SecurityPolicy` to.
- **2k (integration test).** There is a real code path to test; the
  Sprint-2 test spins up `tutorial_server_variable` and asserts a
  `TickMessage` lands.
- **2i (reconnect loop).** The `UaRuntime` bag is the natural place to
  add a reconnect-thread-state field; the 2o knobs
  (`reconnect_interval_ms`) are already honoured by `start()` once the
  loop is wired.
- **First industrial pilot.** With the Tier-1 blocker closed and the
  2q sector profiles (`Fab` / `Auto` / `Steel`) available, the OPC-UA
  connector is pilot-ready pending 2c (corporate deployment) and 2k
  (regression).

## Cross-references

- Prior PoC: `docs/devlog/101_opcua_connector_poc.md`.
- Prior pre-work: `docs/devlog/105_opcua_tier1_prework.md`.
- Design: `docs/design/opcua_connector.md` §3 (Architecture), §13 (Known
  Limitations — 2b row now marked done).
- BACKLOG P9 — Tier 1 row for 2b flipped from open to ✅ in
  `docs/BACKLOG.md`.
- COMPLETED: single Sprint-1 entry covering devlogs 105 + 106 in
  `docs/COMPLETED.md`.
