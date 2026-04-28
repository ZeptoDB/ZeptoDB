# Devlog 105 — OPC-UA Tier 1 Pre-2b Production Hardening

> **Scope.** Pure-engine fixes that land *before* touching `open62541`. Clears four
> BACKLOG P9 Tier 1 blockers / quality items (2n, 2p, 2q, 2o) so the real
> `UA_Client` integration in item 2b (next sprint) has a clean substrate to build
> on.
>
> **Not in this devlog.** No `UA_Client_connect`, no subscription callback, no
> reconnect loop — those are 2b / 2i. No BACKLOG / COMPLETED mutation yet either;
> those move when 2b lands.

## Summary

| BACKLOG | Title | Surface touched |
|---|---|---|
| P9 #2n | Float-safety clamp in `coerce_variant_to_int64` | `src/feeds/opcua_consumer.cpp` — `Float` / `Double` branch |
| P9 #2p | Config validation in `start()` | `src/feeds/opcua_consumer.cpp` — `start()` head |
| P9 #2q | Sector-aware default profiles | `include/zeptodb/feeds/opcua_consumer.h` + `.cpp` — `Profile` enum, `apply_profile()` |
| P9 #2o | Reconnect / timeout knobs (config-struct only) | `include/zeptodb/feeds/opcua_consumer.h` — three `uint32_t` fields |

All four items are S-sized. Diff is 2 engine files, 1 test file, 1 design-doc
section, 1 new devlog (this one).

## 2n — Float-safety clamp

The previous `Float` / `Double` branch of `coerce_variant_to_int64` did

```cpp
out = static_cast<int64_t>(v.f64 * value_scale);   // UB on overflow / NaN / Inf
```

which is undefined behaviour on out-of-range, `NaN`, or ±`Inf` inputs. In
practice gcc / clang on x86_64 produces `INT64_MIN` (0x8000000000000000) for
everything outside `[INT64_MIN, INT64_MAX]`, which silently poisons VWAP /
aggregates downstream.

Fix:

```cpp
const double scaled = v.f64 * value_scale;
if (!std::isfinite(scaled)) return false;                              // NaN / ±Inf
if (scaled >= static_cast<double>(INT64_MAX)) { out = INT64_MAX; return true; }
if (scaled <= static_cast<double>(INT64_MIN)) { out = INT64_MIN; return true; }
out = static_cast<int64_t>(scaled);
return true;
```

Non-finite inputs now return `false` so the caller (`on_data_change`) bumps
`decode_errors` — identical to the pre-existing `VariantType::Unsupported`
path.

### Flipped tests

- `OpcUaEdgeCoerce.DoubleScaleOverflow` — was `EXPECT_TRUE(out == INT64_MIN || out == INT64_MAX)` (documenting the bug); now `EXPECT_EQ(out, INT64_MAX)` for `+1e30 × 1e10` and `EXPECT_EQ(out, INT64_MIN)` for `-1e30 × 1e10`.
- `OpcUaEdgeCoerce.DoubleNaNAndInf` — was `EXPECT_TRUE(coerce(...))` with the result discarded; now `EXPECT_FALSE(coerce(...))` for `NaN`, `+Inf`, and `-Inf`.

Each flipped assertion carries a `// Flipped by P9-2n — was documenting the bug`
marker.

## 2p — Config validation

`start()` previously accepted:

- duplicate `node_id` entries (first emplace wins, second entry silently dropped),
- empty `node_id` strings (map accepts `""` happily).

Validation now runs **before** the license gate (so config bugs surface with
the correct cause instead of "license missing"):

```cpp
std::unordered_set<std::string> seen;
for (const auto& n : config_.nodes) {
    if (n.node_id.empty())         { ZEPTO_ERROR(...); return false; }
    if (!seen.insert(n.node_id).second) {
        ZEPTO_ERROR("duplicate node_id '{}'", n.node_id); return false;
    }
}
```

The pre-existing `endpoint.empty()` and `nodes.empty()` checks stay as siblings.

### Flipped / renamed tests

A rename is allowed here because the test meaning changed from
"here is the bug" to "here is the reject":

- `OpcUaEdgeConfig.DuplicateNodeIdFirstWinsSilently` → `DuplicateNodeIdRejected` — asserts `start()` returns `false` on duplicate `node_id`.
- `OpcUaEdgeConfig.EmptyNodeIdAccepted` → `EmptyNodeIdRejected` — asserts `start()` returns `false` on empty `node_id`.

## 2q — Sector-aware default profiles

Industrial sectors ship with very different sustained-rate / burst profiles.
Shipping one set of defaults (the Generic `queue_size=10, retries=3`) is a bad
fit for both a fab (10 KHz sampling, need 1000-deep per-node queue) and a
steel mill (5 KHz vibration). We expose a single opt-in:

```cpp
OpcUaConfig cfg;
cfg.apply_profile(OpcUaConfig::Profile::Fab);   // or Auto / Steel / Generic
// user overrides still win:
cfg.queue_size = 500;
```

Profile values:

| Profile | `queue_size` | `sampling_interval_ms` | `publishing_interval_ms` | `backpressure_retries` |
|---|---|---|---|---|
| Generic | 10 (unchanged) | 50 (unchanged) | 100 (unchanged) | 3 (unchanged) |
| Fab | 1000 | 0.1 | 10 | 20 |
| Auto | 100 | 1.0 | 10 | 10 |
| Steel | 100 | 0.2 | 10 | 10 |

### New tests

- `OpcUaProfile.FabOverridesBurstyDefaults` — `queue_size == 1000` after `apply_profile(Fab)`.
- `OpcUaProfile.GenericLeavesDefaults` — `queue_size == 10`, `backpressure_retries == 3`.
- `OpcUaProfile.UserSetValueWinsAfterProfile` — `apply_profile(Fab)` then `queue_size = 500` → `500`.

## 2o — Reconnect / timeout knobs (config-only)

Three new fields on `OpcUaConfig`:

```cpp
uint32_t connect_timeout_ms    = 5000;   // initial UA_Client_connect timeout
uint32_t session_timeout_ms    = 60000;  // server session timeout
uint32_t reconnect_interval_ms = 2000;   // base reconnect backoff (P9 #2i)
```

These are pure struct additions in this stage — the actual `UA_Client_connect`
call in 2b will honour `connect_timeout_ms`, session creation will honour
`session_timeout_ms`, and the reconnect loop from 2i will use
`reconnect_interval_ms` as the base back-off. Adding the fields now lets 2b
focus on protocol wiring without touching the config schema.

### New test

- `OpcUaConfig.ReconnectTimeoutDefaults` — `connect_timeout_ms == 5000`, `session_timeout_ms == 60000`, `reconnect_interval_ms == 2000`.

## Verification

```
$ cd build && ninja zepto_tests
[4/4] Linking CXX executable tests/zepto_tests

$ ./tests/zepto_tests --gtest_filter="OpcUa*"
[==========] 38 tests from 16 test suites ran.
[  PASSED  ] 38 tests.

$ ./tests/zepto_tests
[==========] 1270 tests from 172 test suites ran.
[  PASSED  ] 1270 tests.
```

Baseline was 1266. Net `+4`: three new `OpcUaProfile.*` tests + one
`OpcUaConfig.ReconnectTimeoutDefaults`. Two renamed tests (duplicate / empty)
did not change the count.

## What this unblocks

Item 2b (real `UA_Client_connect` / CreateSubscription / CreateMonitoredItems
wiring) can now:

- rely on `coerce_variant_to_int64` being safe on any `UA_Variant` the server
  sends — no UB path to worry about inside the data-change callback,
- rely on `start()` refusing a malformed subscription list before it opens a
  network socket,
- honour `connect_timeout_ms` / `session_timeout_ms` without re-threading the
  config struct,
- expose `apply_profile(Fab|Auto|Steel)` to pilot customers as a one-liner.

## Cross-references

- Design: `docs/design/opcua_connector.md` §13 (updated — blockers 2n/2p marked fixed).
- Prior PoC: `docs/devlog/101_opcua_connector_poc.md` (untouched).
- BACKLOG P9 items 2n / 2p / 2q / 2o — will be marked done when 2b lands in the next sprint.
