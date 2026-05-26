# Devlog 117 — Ingest-rate HPA (P8-I4)

> Closes the last open item under "Horizontal Ingest" in `docs/BACKLOG.md`.
> Date: 2026-05-13.

## Problem

The default Helm HPA auto-scaled on CPU and memory utilization. For
ingest-shaped workloads those signals are a poor proxy:

- A pod can be CPU-idle while its ingest ring buffer saturates and
  ticks start spilling onto the synchronous `store_tick()` fallback
  path (~34× throughput cliff, see devlog 102).
- A pod can be CPU-busy from a heavy query while ingest is light —
  the HPA scales out for the wrong reason and creates empty replicas
  that have to be re-balanced into.
- CPU/mem thresholds also do not match the operator's mental model
  ("scale when ingest crosses N ticks/s/pod").

The fix shipped here exposes the actual ingest rate as a Prometheus
gauge and wires it into the HPA as a `Pods` custom metric. CPU/memory
remain configured on the same HPA as a safety net for non-ingest
workloads.

## Changes

### C++ engine

- `include/zeptodb/server/metrics_collector.h` — added
  `double ingest_ticks_per_sec() const`. Lock-free against the
  single-writer collector thread: acquire-loads `head_` and `count_`,
  reads the two newest ring-buffer entries, and computes
  `(latest.ticks_ingested - prev.ticks_ingested) * 1000 /
   (latest.timestamp_ms - prev.timestamp_ms)`.
  - Returns `0.0` when fewer than 2 snapshots exist, when the time
    delta is 0, or when the tick counter went backwards (counter
    restart / wrap clamp). Never returns a negative value.
- `src/server/http_server.cpp::build_prometheus_metrics()` — emits
  `zepto_ingest_ticks_per_sec` (gauge) right after
  `zepto_ticks_ingested_total`. Uses `std::fixed << setprecision(2)`
  for stable scrape output. Falls back to `0.00` when the metrics
  collector is absent (defensive null-check; in practice it is
  always created in the existing constructors).

No new third-party deps. Pure C++20.

### Helm chart

- `deploy/helm/zeptodb/values.yaml` — added under `autoscaling:`:
  ```yaml
  targetIngestRate: 50000      # ticks/sec/pod target
  ingestRateEnabled: false     # opt-in until prometheus-adapter is shipped
  ```
- `deploy/helm/zeptodb/templates/hpa.yaml` — appends a `Pods` metric
  for `zepto_ingest_ticks_per_sec` (`AverageValue: targetIngestRate`)
  after the existing CPU/memory `Resource` metrics, gated on
  `autoscaling.ingestRateEnabled`. Default `helm install` is unchanged
  (CPU 70% / memory 80% only).

### Tests

- `tests/unit/test_metrics_collector.cpp` — new
  `MetricsCollectorIngestRateTest` fixture, 5 cases:
  - `FirstSampleRateIsZero` — 0 and 1 snapshots both return 0.
  - `SteadyStateRate` — 1000 ticks bumped over ~100ms reports a
    positive rate (5–20K ticks/s tolerance for CI scheduler variance).
  - `IdlePeriodDecaysToZero` — two snapshots with no counter delta
    return exactly 0.
  - `CounterResetClampsToZero` — backwards counter movement (1000 →
    500) clamps to 0 instead of emitting a negative rate.
  - `ConcurrentReadersNoCrash` — 4 reader threads call
    `ingest_ticks_per_sec()` for 50ms while the collector thread runs
    at 10ms cadence; readers always observe `>= 0`, no crashes.
- `tests/unit/test_features.cpp` — extended `MetricsProviderTest` with
  `PrometheusMetricsExposesIngestRate`, asserting the new HELP / TYPE
  / numeric-value lines are present in `GET /metrics`.

### Docs

- `docs/design/phase_c_distributed.md` — new "Ingest-rate HPA"
  subsection in §5 with the prometheus-adapter ConfigMap snippet.
- `docs/design/logging_observability.md` — added
  `zepto_ingest_ticks_per_sec` to the Engine Metrics table.
- `docs/operations/KUBERNETES_OPERATIONS.md` — added an "Ingest-rate
  HPA" subsection under §5 Scaling: prerequisites, enablement
  one-liner, tuning guidance, Karpenter compatibility note.
- `docs/BACKLOG.md` — moved the P8-I4 row from "Horizontal Ingest
  (remaining)" into "Recent completions" with a devlog 117 reference.
- `.kiro/KIRO.md` — bumped "Current last:" to
  `117_ingest_rate_hpa.md` → next 118.

## Sample `/metrics` output (new lines)

```
# HELP zepto_ingest_ticks_per_sec Instantaneous ingest rate (ticks/sec), computed from last two metrics snapshots
# TYPE zepto_ingest_ticks_per_sec gauge
zepto_ingest_ticks_per_sec 0.00
```

Under sustained 100K-tick/s synthetic ingest, the gauge stabilizes
around `100000.00` with the default 3-second `MetricsCollector`
interval (per-snapshot delta = 300K ticks / 3000 ms).

## Helm verification

`helm template ... --set autoscaling.ingestRateEnabled=true` produces:

```yaml
metrics:
  - type: Resource          # safety net, unchanged
    resource: { name: cpu, target: { type: Utilization, averageUtilization: 70 } }
  - type: Resource          # safety net, unchanged
    resource: { name: memory, target: { type: Utilization, averageUtilization: 80 } }
  - type: Pods              # new — P8-I4
    pods:
      metric: { name: zepto_ingest_ticks_per_sec }
      target: { type: AverageValue, averageValue: "50000" }
```

Default render (without the flag) keeps only the two `Resource`
metrics — backward compatible with existing deployments.

## Risk

- **Adapter dependency** — `Pods` metric is unavailable without
  `prometheus-adapter`. Default-off behind `ingestRateEnabled=false`
  prevents accidental "stuck pending HPA" scenarios.
- **Counter wrap on restart** — clamped to 0 in
  `ingest_ticks_per_sec()`. The HPA simply sees a brief 0-rate sample
  on pod restart and treats it as a momentary idle period.
- **Cross-arch** — pure metrics + Helm; no SIMD/Highway code touched.
  ARM64 (Graviton) verification is best-effort given environment
  constraints, but the code path uses only `std::atomic` and plain
  arithmetic which are bit-identical between architectures.

## Follow-ups (not in this devlog)

- Ship a default `prometheus-adapter` rule as part of the chart
  (currently operators provide their own ConfigMap).
- Consider exposing a `zepto_ingest_ticks_per_sec` series labeled by
  table when per-table autoscaling becomes a requirement.
