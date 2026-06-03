# 159: P9 Factory 10KHz Live Competitor Run

Date: 2026-06-03
Status: Complete

## Context

P9 had one remaining open item after the ROS 2, Physical AI, and OPC-UA
closeout work: execute the factory 10KHz proof against real InfluxDB and
TimescaleDB deployments instead of only shipping a wrapper that could accept
external competitor commands.

## Changes

- `tools/run-factory-10khz-competitor-bench.sh`
  - Added `ZEPTO_BENCH_CMD` so the ZeptoDB workload can be supplied as a full
    command, matching the existing InfluxDB and TimescaleDB command hooks.
  - Renamed the duration variable away from Bash's special `SECONDS` variable.
- `tools/factory-10khz-workload.py`
  - Added a dependency-free deterministic workload runner for ZeptoDB,
    InfluxDB, and TimescaleDB.
  - Uses the same row generator, target rate, batch size, symbol count, and
    run id across all systems.
  - Verifies row counts after ingest and exits non-zero on failed writes or
    missing rows.
- `tools/run-factory-10khz-live-docker.sh`
  - Starts a local ZeptoDB HTTP server plus real Docker deployments of
    `influxdb:2.7` and `timescale/timescaledb:2.15.3-pg16`.
  - Runs the three workloads through the existing JSONL competitor harness.
  - Cleans up the ZeptoDB process and competitor containers by default.
- Documentation
  - Added `docs/bench/results_factory_10khz_competitors.md`.
  - Updated `docs/bench/logistics_benchmark_suite.md`, `docs/BACKLOG.md`, and
    `docs/COMPLETED.md`.
- Build hygiene
  - Linked `zepto_migration` against `zepto_auth` because migration loaders
    call the shared license gate; this keeps full default builds from failing
    at `zepto-migrate` link time.

## Verification

- Script hygiene:
  - `bash -n tools/run-factory-10khz-competitor-bench.sh`
  - `bash -n tools/run-factory-10khz-live-docker.sh`
  - `python3 -m py_compile tools/factory-10khz-workload.py`
- Full local verification:
  - `cmake --build build --target zepto_tests -j$(nproc)`
  - `./build/tests/zepto_tests`
  - `cmake --build build -j$(nproc)`
- Smoke:
  - `tools/run-factory-10khz-live-docker.sh --seconds 2 --rate 1000 --batch-size 200 --symbols 10 --skip-pull --out /tmp/zepto_p9_factory_smoke2`
  - Result: ZeptoDB, InfluxDB, and TimescaleDB all passed with 2,000 inserted
    and 2,000 verified rows.
- P9 closure run:
  - `tools/run-factory-10khz-live-docker.sh --seconds 60 --rate 10000 --batch-size 500 --symbols 100 --skip-pull --out bench-results/factory-10khz/p9-live-20260603`
  - ZeptoDB: 600,000 inserted, 600,000 verified, 0 failed, 9,999.98 rows/sec.
  - InfluxDB: 600,000 inserted, 600,000 verified, 0 failed, 9,999.98 rows/sec.
  - TimescaleDB: 600,000 inserted, 600,000 verified, 0 failed, 9,998.68 rows/sec.
  - Docker cleanup verified: no `zepto-p9-*` containers remained.

## Follow-ups

- The P9 backlog is closed. Longer customer-lab runs can reuse the same wrapper
  with `--seconds 600`, but no P9 implementation item remains open.
