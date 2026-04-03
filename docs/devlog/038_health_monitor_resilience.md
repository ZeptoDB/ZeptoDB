# 035 — HealthMonitor DEAD Recovery + UDP Fault Tolerance

**Date**: 2026-03-27
**Status**: Completed
**P8-High**: HealthMonitor DEAD Recovery, HealthMonitor UDP Fault Tolerance

---

## Background

Two operational stability issues:

1. **DEAD recovery impossible**: Even when a DEAD node recovers, it remains DEAD forever. No rejoin path exists.
2. **Insufficient UDP fault tolerance**: UDP packet loss alone can trigger DEAD determination. Socket bind failures are silently ignored.

## Changes

### 1. DEAD Recovery — REJOINING State + Rejoin Protocol

`health_monitor.h`:
- Added `NodeState::REJOINING` (= 6)
- When a DEAD node resumes receiving heartbeats: DEAD → REJOINING → (resynchronization) → ACTIVE
- `on_rejoin(RejoinCallback)` — registers a resynchronization callback. Returns to ACTIVE on true, stays REJOINING on false and retries on next heartbeat
- Added REJOINING to `node_state_str()`

`cluster_node.h`:
- `on_node_state_change()`: Re-adds node to router on REJOINING → ACTIVE transition (same path as existing JOINING → ACTIVE)

### 2. UDP Fault Tolerance — 3 Consecutive Misses + Bind Fatal + TCP Heartbeat

New `HealthConfig` fields:
- `tcp_heartbeat_port` (default 9101) — secondary TCP heartbeat port
- `consecutive_misses_for_suspect` (default 3) — number of consecutive misses before SUSPECT transition
- `enable_dead_rejoin` (default true) — enables DEAD recovery
- `fatal_on_bind_failure` (default true) — throws exception on UDP bind failure

`HealthMonitor` changes:
- Added `consecutive_misses_` map — per-node consecutive heartbeat miss count
- `check_timeouts()`: calculates miss count proportional to elapsed time before SUSPECT determination
- `setup_udp_socket()`: throws `std::runtime_error` on bind failure (fatal_on_bind_failure=true)
- `tcp_recv_loop()`: TCP heartbeat receive thread — compensates for UDP loss
- `tcp_probe()`: TCP connect probe before SUSPECT → DEAD transition — only confirms DEAD on failure
- `inject_heartbeat()`: resets miss count, handles rejoin from DEAD state

### Socket Changes

- `sock_` → `udp_sock_` + `tcp_sock_` (UDP/TCP separation)
- `send_thread_`, `recv_thread_`, `check_thread_` + `tcp_recv_thread_` (4 threads)
- `stop()`: checks joinable before join (safe shutdown)

## Tests

- Existing `HealthMonitor.StateTransitions` — passed (backward compatible)
- Existing `HealthMonitor.GetActiveNodes` — passed
- Existing `Failover.HealthMonitorIntegration_DeadTriggersFailover` — passed
- All 796 tests passed

## File Changes

| File | Changes |
|------|---------|
| `include/zeptodb/cluster/health_monitor.h` | REJOINING state, consecutive miss, TCP heartbeat, bind fatal, rejoin protocol |
| `include/zeptodb/cluster/cluster_node.h` | Re-add to router on REJOINING→ACTIVE transition |
| `docs/BACKLOG.md` | Marked 2 P8-High items as completed |
