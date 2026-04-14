# Devlog 062: Rebalance Bandwidth Throttling

Date: 2026-04-14

## Summary

Added bandwidth throttling to the live rebalancing system to prevent network saturation during large partition migrations.

## Design

`BandwidthThrottler` is a lightweight, thread-safe rate limiter that tracks bytes transferred in a sliding 1-second window. When the transfer rate exceeds the configured limit, `record()` blocks the calling thread until the rate drops below the threshold.

### Key Properties

- **Thread-safe**: atomic counters for bytes and window start time
- **Sliding window**: 1-second window with automatic reset to avoid drift
- **Zero overhead when disabled**: `record()` returns immediately when limit is 0
- **Runtime adjustable**: `set_limit_mbps()` and `reset()` take effect immediately

### Integration Points

```
RebalanceManager
  ├── owns BandwidthThrottler (member)
  ├── initializes from RebalanceConfig::max_bandwidth_mbps
  ├── set_max_bandwidth_mbps() for runtime changes
  └── passes &throttler_ to PartitionMigrator via set_throttler()

PartitionMigrator::migrate_symbol()
  └── calls throttler_->record(batch.size() * 64) after each chunk transfer

/admin/rebalance/status
  └── includes max_bandwidth_mbps in JSON response
```

### Rate Calculation

```
current_rate = total_bytes_in_window * 1e6 / elapsed_microseconds

if current_rate > limit:
    sleep_us = (total_bytes / limit_bytes_per_sec * 1e6) - elapsed_us
```

## Configuration

```cpp
struct RebalanceConfig {
    uint32_t max_bandwidth_mbps = 0;  // 0 = unlimited
    // ...
};
```

Runtime update via `RebalanceManager::set_max_bandwidth_mbps(mbps)` or HTTP API.

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/cluster/bandwidth_throttler.h` | Added `reset()` method |
| `tests/unit/test_bandwidth_throttler.cpp` | 10 unit tests (new file) |
| `tests/CMakeLists.txt` | Registered test file |

## Test Coverage (10 tests)

| Test | What it verifies |
|------|-----------------|
| `UnlimitedDoesNotBlock` | limit=0 completes instantly |
| `ZeroBytesDoesNotBlock` | record(0) is a no-op |
| `ThrottlesWhenOverLimit` | 2MB at 1MB/s sleeps ~1s |
| `LimitMbpsAccessor` | get/set limit round-trips |
| `SetLimitResetsWindow` | set_limit_mbps clears counters |
| `ResetClearsCounters` | reset() zeroes bytes_in_window |
| `RuntimeLimitChange` | unlimited→1MB/s starts throttling |
| `ConcurrentRecordNoRace` | 4 threads, no data race |
| `ConcurrentRecordWithThrottle` | 4 threads with active throttle |
| `DefaultConstructorIsUnlimited` | default ctor → limit=0 |
