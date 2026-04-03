# 031 — CoordinatorHA ↔ K8sLease/FencingToken Integration

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: CoordinatorHA ↔ K8sLease Integration

---

## Background

CoordinatorHA was not using K8sLease/FencingToken, so during a network partition
two Active coordinators could exist simultaneously. Writes from a stale coordinator were not rejected.

## Changes

### coordinator_ha.h
- Added `require_lease`, `LeaseConfig` to `CoordinatorHAConfig`
- Added `FencingToken fencing_token_` member
- Added `std::unique_ptr<K8sLease> lease_` member
- Added `fencing_token()`, `epoch()`, `lease()` accessors
- Extracted `try_promote()` private method

### coordinator_ha.cpp

**init()**:
- When `require_lease=true`, creates K8sLease + registers on_elected/on_lost callbacks
- On ACTIVE initialization, immediately calls `fencing_token_.advance()` + sets epoch on peer RPC

**start()**:
- Starts K8sLease (joins lease competition)

**try_promote()** (new):
- If lease is required, checks `lease_->try_acquire()` — rejects promote on failure
- `fencing_token_.advance()` — epoch bump
- `peer_rpc_->set_epoch(new_epoch)` — propagates epoch to RPC client
- Existing node re-registration + promotion callback

**monitor_loop()**:
- Delegates promote path to `try_promote()`
- On lease acquisition failure, retries (resets last_pong instead of breaking)

**on_lost callback**:
- Automatically demotes on lease loss (ACTIVE → STANDBY)

## Split-brain Prevention Flow

```
Coordinator A (epoch=5) ──X── network partition
Coordinator B: lease acquired → try_promote()
  → fencing_token_.advance() → epoch=6
  → peer_rpc_.set_epoch(6)

Coordinator A recovers, tries write with epoch=5
  → Data node: FencingToken::validate(5) fails (last_seen=6)
  → REJECTED
```

## Backward Compatibility

- `require_lease=false` (default) → existing behavior unchanged (ping-based failover)
- `require_lease=true` → production mode (lease + fencing)
- All existing 796 tests pass (no regression)
