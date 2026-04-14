# Devlog 063 — PTP Clock Sync Detection

Date: 2026-04-14

## What

PTP (Precision Time Protocol) clock synchronization detection for distributed ASOF JOIN strict mode. When clock skew between cluster nodes exceeds a threshold, distributed ASOF JOINs are rejected to prevent inaccurate time-based matching.

## Implementation

- `PtpClockDetector` class with 4 sync states: `SYNCED`, `DEGRADED`, `UNSYNC`, `UNAVAILABLE`
- Detects PTP hardware (`/dev/ptp*`, `/sys/class/ptp/`), chrony daemon, systemd-timesyncd
- Configurable `max_offset_ns` threshold (default 1μs)
- `strict_mode` config — when enabled, distributed ASOF JOIN returns error on bad sync
- `GET /admin/clock` HTTP endpoint — JSON with status, offset_ns, ptp_available
- Thread-safe (atomic + mutex)
- Graceful degradation: systems without PTP report `UNAVAILABLE` (not an error)

## Files

- `include/zeptodb/cluster/ptp_clock_detector.h` — header with PtpSyncStatus, PtpConfig, PtpClockDetector
- `src/cluster/ptp_clock_detector.cpp` — detection logic (154 lines)
- `tests/unit/test_ptp_clock_detector.cpp` — 22 tests

## Tests

22 tests covering: status transitions, threshold config, concurrent access, systems without PTP, inject/read, zero threshold edge case.
