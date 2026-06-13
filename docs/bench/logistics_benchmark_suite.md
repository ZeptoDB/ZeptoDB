# Logistics Benchmark Suite

This benchmark suite defines the P9 logistics proof workload. It is a repeatable
scenario plan for future competitor runs against InfluxDB and TimescaleDB.

## Workloads

| Workload | Rows/sec target | Query proof |
|---|---:|---|
| 2K AGV pose streams | 200,000 | geofence + proximity filter |
| 1M sorter lane events | 1,000,000 | per-lane jam/anomaly aggregate |
| 50K RFID reads | 50,000 | entity timeline reconstruction |
| Cold-chain sensors | 100,000 | audit range scan by shipment |

## Tables

```sql
CREATE TABLE agv_pose (
  timestamp TIMESTAMP_NS,
  agv_id SYMBOL,
  lat FLOAT64,
  lon FLOAT64,
  heading_deg FLOAT64,
  battery_pct FLOAT64,
  quality INT32
)

CREATE TABLE sorter_metrics (
  timestamp TIMESTAMP_NS,
  line_id SYMBOL,
  lane_id SYMBOL,
  throughput INT64,
  jam_count INT64,
  motor_temp_c FLOAT64
)

CREATE TABLE rfid_reads (
  timestamp TIMESTAMP_NS,
  tag_id SYMBOL,
  gate_id SYMBOL,
  rssi FLOAT64,
  quality INT32
)
```

## Required Queries

```sql
SELECT count(*)
FROM agv_pose
WHERE ST_Within(lat, lon, 37.7749, -122.4194, 50)
```

```sql
SELECT lane_id, sum(jam_count), avg(motor_temp_c)
FROM sorter_metrics
WHERE timestamp BETWEEN 1710000000000000000 AND 1710000060000000000
GROUP BY lane_id
```

```sql
SELECT timestamp, gate_id, rssi
FROM rfid_reads
WHERE tag_id = 881122
ORDER BY timestamp ASC
```

## Pass Criteria

- Ingest target rate sustained for 10 minutes.
- No decode or ingest failures.
- P50/P99 query latency reported per workload.
- Result parity validated against a deterministic seed.
- x86_64 and aarch64 runs produce matching result counts.

## Factory 10 KHz Competitor Harness

Use `tools/run-factory-10khz-competitor-bench.sh` to standardize the P9
factory proof run across ZeptoDB, InfluxDB, and TimescaleDB. ZeptoDB uses
`bench_ingest_scale`; competitor workloads are injected through environment
commands because lab deployments vary.

```bash
ZEPTO_BENCH_ARGS="--host 127.0.0.1 --port 8123 --pods 1 --threads 8 --batch-size 10" \
INFLUX_BENCH_CMD="./vendor/influx/factory_10khz.sh" \
TIMESCALE_BENCH_CMD="./vendor/timescale/factory_10khz.sh" \
tools/run-factory-10khz-competitor-bench.sh --seconds 600 --require-competitors
```

For ZeptoDB-only smoke validation, use `--zepto-only`; the summary records
InfluxDB/TimescaleDB as skipped instead of silently treating them as passed.

For a self-contained local live proof, use the Docker-backed wrapper. It starts
ZeptoDB, InfluxDB, and TimescaleDB, then feeds the same deterministic rate-limited
factory workload into each system:

```bash
tools/run-factory-10khz-live-docker.sh \
  --seconds 60 \
  --rate 10000 \
  --batch-size 500 \
  --symbols 100 \
  --out bench-results/factory-10khz/p9-live-20260603
```

The P9 closure run is recorded in
[`results_factory_10khz_competitors.md`](results_factory_10khz_competitors.md):
ZeptoDB, InfluxDB, and TimescaleDB each sustained 10,000 rows/sec for 60 seconds
with 600,000 inserted and 600,000 verified rows.
