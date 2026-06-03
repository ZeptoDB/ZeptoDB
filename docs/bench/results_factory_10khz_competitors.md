# Factory 10KHz Competitor Live Run

Date: 2026-06-03

## Scope

This run closes the P9 factory competitor proof item by executing the same
deterministic live ingest workload against:

- ZeptoDB local `zepto_http_server`
- InfluxDB `influxdb:2.7`
- TimescaleDB `timescale/timescaledb:2.15.3-pg16`

All competitor systems ran as real local Docker deployments. The harness used
batched writes with a fixed target rate rather than preloading static files.

## Command

```bash
tools/run-factory-10khz-live-docker.sh \
  --seconds 60 \
  --rate 10000 \
  --batch-size 500 \
  --symbols 100 \
  --skip-pull \
  --out bench-results/factory-10khz/p9-live-20260603
```

## Results

| System | Status | Target rows/sec | Duration | Inserted rows | Verified rows | Failed rows | Achieved rows/sec |
|--------|--------|-----------------|----------|---------------|---------------|-------------|-------------------|
| ZeptoDB | PASS | 10,000 | 60.000s | 600,000 | 600,000 | 0 | 9,999.98 |
| InfluxDB | PASS | 10,000 | 60.000s | 600,000 | 600,000 | 0 | 9,999.98 |
| TimescaleDB | PASS | 10,000 | 60.008s | 600,000 | 600,000 | 0 | 9,998.68 |

Raw JSONL summary:

```jsonl
{"system":"zeptodb","status":"pass","started_ns":1780512420905703541,"ended_ns":1780512480976214809,"log":"bench-results/factory-10khz/p9-live-20260603/zeptodb.log"}
{"system":"influxdb","status":"pass","started_ns":1780512480994250835,"ended_ns":1780512541195970194,"log":"bench-results/factory-10khz/p9-live-20260603/influxdb.log"}
{"system":"timescaledb","status":"pass","started_ns":1780512541213082003,"ended_ns":1780512601640031499,"log":"bench-results/factory-10khz/p9-live-20260603/timescaledb.log"}
```

## Notes

- This is a correctness and live-sustain proof, not a maximum-throughput shootout.
  The runner rate-limits all systems to the Sector-B 10KHz target.
- ZeptoDB and InfluxDB were verified through query count APIs. TimescaleDB was
  verified through `psql` inside the live container.
- The generated `bench-results/` logs are intentionally local run artifacts;
  this document records the durable result.
