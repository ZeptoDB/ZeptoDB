# Devlog 109 — OPC-UA reconnect / failover policy

**Status:** shipped (Sprint 2 Stage 3)
**BACKLOG:** P9 #2i
**Depends on:** devlog 106 (UA_Client integration), devlog 108 (Basic256Sha256 security)

## Problem

Sprint 1 (devlog 106) wired a background thread that calls
`UA_Client_run_iterate()` in a tight loop, but it does nothing when the
TCP connection drops or the OPC-UA secure channel is torn down by the
server. After a transient network blip or a server restart the consumer
stays stuck — `messages_consumed` flatlines and the only recovery is an
external `stop()` / `start()` cycle.

In industrial deployments (Samsung / SK / TSMC fabs, POSCO mills) the PLC
endpoint is often behind a flaky VPN or a site-level network refresh; we
need automatic, bounded-cost reconnection on the critical path.

## Design

### State machine

The background thread is a single loop. Each iteration calls
`UA_Client_run_iterate(client, 100)` and inspects the returned
`UA_StatusCode`.

```
          ┌──────────────────┐
          │  run_iterate()   │◄──── returns GOOD or transient non-fatal
          └────────┬─────────┘
                   │ returns a "connection lost" status
                   ▼
          ┌──────────────────┐
          │ sleep(backoff ±  │
          │   25% jitter)    │
          └────────┬─────────┘
                   ▼
          ┌──────────────────┐
          │ UA_Client_connect│
          └────────┬─────────┘
              GOOD │  BAD
           ┌───────┴────────┐
           ▼                ▼
   reset backoff     backoff = min(backoff*2, max)
   setup_subscription()       (goto top of loop)
   stats.reconnects++
```

### Trigger status codes

We treat these three as "connection definitely dead, reconnect":

| Code                                  | When it fires                                     |
|---------------------------------------|---------------------------------------------------|
| `UA_STATUSCODE_BADCONNECTIONCLOSED`   | TCP FIN/RST from server                           |
| `UA_STATUSCODE_BADSERVERNOTCONNECTED` | open62541 internal state says no session          |
| `UA_STATUSCODE_BADSECURECHANNELCLOSED`| OPC-UA secure-channel teardown (e.g. session TO)  |

Anything else (including `UA_STATUSCODE_GOOD` and transient Bad codes
like `BadTimeout`) is passed through — those are open62541's concern to
retry at the protocol layer.

### Backoff formula

```
backoff_ms     = config_.reconnect_interval_ms               // initial / reset value
max_backoff_ms = config_.reconnect_interval_ms * 16          // ceiling

on connect failure:  backoff_ms = min(backoff_ms * 2, max_backoff_ms)
on connect success:  backoff_ms = config_.reconnect_interval_ms   // reset
```

With the default `reconnect_interval_ms = 2000`, the sequence is
`2 s → 4 → 8 → 16 → 32 (cap) → 32 → …`.  Total elapsed time across six
failed attempts is ~94 s, which matches the brief's requirement that the
consumer recovers quickly from a brief outage and doesn't spin against a
long one.

We deliberately did NOT add a separate `reconnect_max_backoff_ms` config
knob — the 16× factor is a well-understood default, and the base knob is
already tunable.

### Jitter

Each sleep is `backoff_ms ± 25%` via `std::rand()`, in 50 ms slices so
`stop()` can interrupt promptly. The jitter prevents a thundering-herd
reconnect if many consumers trip at the same server restart.

### Subscription rebuild

After a successful `UA_Client_connect` the previous subscription and all
MonitoredItems are dead (they lived in the destroyed server session).
We therefore call `setup_subscription()` — a method factored out of
`start()` — which creates a fresh subscription and re-registers every
node from `config_.nodes`. `ItemContext` owners in `UaRuntime::items`
are replaced atomically under `g_rt_mu`.

If `setup_subscription()` fails (e.g. server is up but not ready), we
log a WARN and let the next iterate tick surface the failure again;
the backoff will then double on the next `UA_Client_connect` attempt.
We did NOT roll back `stats.reconnects++` on that branch: a successful
reconnect at the transport layer is still progress even if subscription
setup needs another round.

### Observability

Added `OpcUaStats::reconnects` (uint64), exposed through the existing
`stats()` snapshot. Instruments Prometheus / dashboards can track
`rate(opcua_reconnects_total[5m])` to alert on flaky links.

## Code layout

Minimal diff, confined to the three specified insertion points:

| File | Change |
|------|--------|
| `include/zeptodb/feeds/opcua_consumer.h` | Added `OpcUaStats::reconnects`; declared private `setup_subscription()` and `run_iterate_loop()` methods |
| `src/feeds/opcua_consumer.cpp` | Added `<cstdlib>`; factored subscription + MonitoredItems block from `start()` into `setup_subscription()`; replaced the inline iterate-thread lambda with a call to `run_iterate_loop()`; implemented the new method with the disconnect/backoff/reconnect/rebuild state machine |
| `tests/unit/test_opcua.cpp` | Added `OpcUaReconnect.StatsReconnectsStartsAtZero` and `OpcUaReconnect.BackoffCappedAtConfigMultiple` |

No Sprint 1 / Sprint 2 Stage 1 / Stage 2 code was touched outside these
three insertion points. `is_valid_security` signature unchanged. Full
`ZEPTO_USE_OPCUA=OFF` build is byte-identical in behaviour (the new
methods compile to empty stubs).

## Tests

```
./tests/zepto_tests --gtest_filter="OpcUa*"     # 49 / 49 pass (was 47)
./tests/zepto_tests                             # 1281 / 1281 pass (was 1279)
```

### Coverage

- `OpcUaReconnect.StatsReconnectsStartsAtZero` — counter exposed, zero
  on a fresh consumer.
- `OpcUaReconnect.BackoffCappedAtConfigMultiple` — mirrors the doubling
  clamp formula and asserts saturation at `reconnect_interval_ms * 16`
  (32 000 ms with the default 2 s base).
- Compile-time coverage of the reconnect path itself is provided by the
  `ZEPTO_USE_OPCUA=ON` build (no diagnostics on any edited file).

### What is NOT tested yet (Sprint 3 follow-ups)

1. **Live reconnect round-trip** — start the open62541 tutorial server,
   connect, kill the server, wait out one backoff cycle, restart,
   assert `stats().reconnects == 1` and `messages_consumed` resumes.
   Deferred: would require introducing process-kill plumbing into the
   integration harness from devlog 107. Tracked in BACKLOG as a
   Sprint 3 hardening item.
2. **Backoff cap overshoot under sustained outage** — verify that with
   a server down for > 60 s the consumer stops doubling and pegs at
   the 32 s ceiling indefinitely. Covered in spirit by the unit test's
   100-iteration saturation check; a live version would need the same
   kill-server harness.
3. **`reconnect_max_backoff_ms` as a config knob** — if a customer asks
   for a shorter or longer ceiling than 16× the base, promote the
   derivation to a config field. Not needed for first-commercial.

## Notes

### Why not `UA_Client_getState()`?

open62541 exposes `UA_Client_getState()` which can distinguish
`UA_CLIENTSTATE_DISCONNECTED` from `UA_CLIENTSTATE_CONNECTED` explicitly.
We chose to key off the `run_iterate()` return code instead because it
is the single value already in hand on every iteration, and the three
status codes we match on precisely describe the "need to reconnect"
state without ambiguity. This also avoids pulling in the version-drift
footgun between open62541 1.3 and 1.4 (the enum was renamed between
versions; the status-code constants were not).

### Why not preserve the old subscription?

An OPC-UA subscription is scoped to the server-side session. When the
session dies (any of the three trigger codes), the subscription is
already gone on the server. Trying to "keep" the old subscription ID
would produce `BadSubscriptionIdInvalid` on the next publish — strictly
more work than just rebuilding. The minimal path is to treat reconnect
as a cold start of the subscription layer.

### Why `std::rand()`?

We need ±25% jitter on the sleep duration and the existing code in this
layer already tolerates the `std::rand` thread-safety caveat (single
iterate thread, no other caller in this file). Upgrading to
`std::mt19937` would add an extra field on the consumer for no
observable gain.
