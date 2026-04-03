# 032 — WalReplicator Replication Guarantees (Quorum / Retry / Backpressure)

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: WalReplicator Replication Guarantees

---

## Background

The existing WalReplicator was async best-effort:
- Data dropped silently when queue exceeded capacity
- No retries on send failure (only error count incremented)
- No distinction when some replicas fail among multiple replicas

## Changes

### ReplicatorConfig Additional Fields

| Field | Default | Description |
|-------|---------|-------------|
| `quorum_w` | 0 | Minimum ACK count (0 = all replicas) |
| `retry_queue_capacity` | 64K | Retry queue size |
| `retry_interval_ms` | 100 | Retry interval |
| `max_retries` | 3 | Maximum retry attempts |
| `backpressure` | false | true = block producer when queue is full |
| `backpressure_timeout_ms` | 500 | Maximum block wait time |

### ReplicatorStats Additional Fields

| Field | Description |
|-------|-------------|
| `retried` | Number of batches successfully retried |
| `retry_exhausted` | Number of batches dropped after exceeding max_retries |
| `backpressured` | Number of enqueue calls blocked by backpressure |

### Implementation

**Quorum write**: In `flush_batch()`, sends to each replica and tallies ACK count.
Success if `ack_count >= quorum_w`. If quorum_w=0, all must succeed.

**Retry queue**: Failed send batches are stored in `retry_queue_`.
`send_loop()` calls `process_retry_queue()` on every iteration.
Retries if attempts < max_retries, drops and logs if exceeded.

**Backpressure**: When `backpressure=true` and queue is full, blocks the
producer with `cv_.wait_for()`. Released by `cv_.notify_all()` after batch swap
in `send_loop()`. Drops on timeout.

### Backward Compatibility

All defaults match existing behavior:
- `quorum_w=0` → attempts all replicas (same as before)
- `max_retries=3` → retries added (previously: 0)
- `backpressure=false` → drops on queue overflow (same as before)

## Tests

All existing 796 tests passed. ReplicaDown test confirmed retry-then-drop logging.
