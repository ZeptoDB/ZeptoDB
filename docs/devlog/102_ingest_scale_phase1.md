# Devlog 102 — Ingest Scale-Out Phase 1

**Date:** 2026-04-25
**Priority:** P7 — Performance / Engine
**Status:** Complete — two tunables exposed, zero architectural changes.

---

## Motivation

A single `ZeptoPipeline` had two compile-time ingest ceilings that were
easy to hit on real workloads:

| Ceiling | Where | Pre-102 value |
|---|---|---|
| Drain-thread parallelism | `PipelineConfig::drain_threads` | `1` |
| Ring-buffer capacity | `TickPlant::TickQueue = MPMCRingBuffer<…, 65536>` | `65 536` slots |

When the ring buffer saturates, `ZeptoPipeline::ingest_tick()` falls
through to a **synchronous** `store_tick()` as a no-data-loss safety
net. That path is ~34× slower than the async queue-then-drain path in
the ingest bench: **6.6 M ticks/s → 197 K ticks/s** the moment the queue
fills. A single drain thread makes this inevitable on any box with more
than ~2 producer cores.

The `MPMCRingBuffer` is already lock-free multi-producer /
multi-consumer and `src/core/pipeline.cpp::start()` already spawns
`config_.drain_threads` threads in a loop — raising the count scales
almost for free until `PartitionManager::get_or_create` lock
contention dominates. So Phase 1 deliberately does **no** architectural
work: it exposes and tunes two existing parameters.

Horizontal scaling (stateless `zepto_ingest_node` + ingest-rate HPA) is
Phase 2 and is tracked in `BACKLOG.md` under P8.

---

## What changed

### Code

| File | Change |
|---|---|
| `include/zeptodb/core/pipeline.h` | `drain_threads` default `1 → 0` (sentinel = auto); new `ring_buffer_capacity` field (default `0` = engine default 65536); new `drain_thread_count()` accessor; new static `resolve_ring_buffer_capacity()` helper |
| `src/core/pipeline.cpp` | `start()` resolves `drain_threads == 0 → max(2, hw_concurrency()/4)`; ctor passes resolved capacity to `TickPlant`; startup log now reports both effective values |
| `include/zeptodb/ingestion/ring_buffer.h` | **New** `MPMCRingBufferDynamic<T>` — runtime-sized twin of `MPMCRingBuffer<T, Capacity>` (same lock-free protocol, mask = `capacity_ - 1`). Compile-time class untouched |
| `include/zeptodb/ingestion/tick_plant.h` | `TickPlant` now takes a runtime `capacity` ctor arg (default `kDefaultCapacity = 65536`); `TickQueue` aliased to `MPMCRingBufferDynamic<TickMessage>`; new `capacity()` accessor |
| `src/ingestion/tick_plant.cpp` | Ctor body updated |
| `tools/zepto_http_server.cpp` | New flags `--drain-threads N` and `--ring-buffer-capacity N` (both default 0 = engine default); env-var fallback (`ZEPTO_DRAIN_THREADS` / `ZEPTO_RING_BUFFER_CAPACITY`) used when flags are absent, so Helm-set env vars take effect without rewriting the container `CMD` |
| `deploy/helm/zeptodb/values.yaml` | New `pipeline.drainThreads` / `pipeline.ringBufferCapacity` (both default `0`) |
| `deploy/helm/zeptodb/templates/{configmap,statefulset,deployment}.yaml` | Threaded through to configmap, env vars (`ZEPTO_DRAIN_THREADS` / `ZEPTO_RING_BUFFER_CAPACITY`), and the `start.sh` CLI construction in the cluster-mode initContainer |
| `tests/unit/test_tick_plant.cpp` | Seven focused Phase 1 tests (drain-threads auto / explicit, capacity default / custom / non-pow2 / below-range / above-range) |
| `docs/design/layer2_ingestion_network.md` | New § "Ingest capacity tuning" subsection |
| `docs/operations/KUBERNETES_OPERATIONS.md` | New § 5 "Vertical ingest tuning" subsection |
| `docs/BACKLOG.md` | P7-I1 / P7-I2 marked ✅; Phase 2 items added under P8 |
| `docs/COMPLETED.md` | One-line entry pointing at this devlog |

### Before / After defaults

| Tunable | Pre-102 | Post-102 | Notes |
|---|---|---|---|
| `drain_threads` | `1` (fixed) | `0 = auto` → `max(2, hw/4)`; any `>=1` honored verbatim | Floor 2, no cap — `std::max(1, …)` defense-in-depth clamp in `start()` preserved |
| `ring_buffer_capacity` | N/A (compile-time 65536) | `0 = engine default (65536)`; any power of two in `[4096, 16 777 216]` | Validated at ctor (throws `std::invalid_argument` on bad value) |

Legacy callers that set `cfg.drain_threads = N` (tests, benches,
`test_coordinator.cpp`) continue to work unchanged and get exactly `N`
threads.

### Explicit-value semantics

- `drain_threads = 0` → sentinel, auto-resolve.
- `drain_threads = 1` → one drain thread (opt-out from auto; unusual but valid).
- `drain_threads = N` → exactly `N`.

### Validation

`ZeptoPipeline::resolve_ring_buffer_capacity(size_t)` is static and
pure; callers can pre-validate a config before constructing the
pipeline. Invalid values throw `std::invalid_argument` with the
offending value in the message. Valid range: powers of two in
`[4096, 16 777 216]`.

Chosen over a `bool start()` return because (a) existing start() is
`void` and (b) failing fast at construction matches the
`MPMCRingBufferDynamic` invariant better than deferred allocation.

---

## Helm `values.yaml` additions

```yaml
pipeline:
  drainThreads: 0          # 0 = auto (max(2, hw/4)); N>0 = explicit
  ringBufferCapacity: 0    # 0 = 65536; power of two in [4096, 16777216]
```

Examples:

```bash
# IoT pilot — small burst absorber, spare CPU for query workload
helm upgrade zeptodb ./deploy/helm/zeptodb \
  --set pipeline.drainThreads=2 \
  --set pipeline.ringBufferCapacity=65536

# Auto factory — moderate bursts, balanced
helm upgrade zeptodb ./deploy/helm/zeptodb \
  --set pipeline.drainThreads=4 \
  --set pipeline.ringBufferCapacity=262144

# Semi fab — 10 kHz × ~30 k tags, CMP bursts
helm upgrade zeptodb ./deploy/helm/zeptodb \
  --set pipeline.drainThreads=8 \
  --set pipeline.ringBufferCapacity=1048576
```

Operators observing `TickPlant queue full! Dropping tick seq=…` in
logs, or seeing `ticks/s` drop by >10× under burst, should raise
`ringBufferCapacity` first, then `drainThreads`.

---

## Startup log (operator-visible)

```
ZeptoPipeline 초기화 (arena=32MB, batch=256, mode=0, ring_capacity=262144)
TickPlant initialized (queue capacity=262144)
ZeptoPipeline 시작 완료 (drain_threads=4, ring_capacity=262144)
```

Both effective values appear in `kubectl logs` at INFO level.

---

## Tuning guidance

| Workload | Tags × rate | Suggested `drainThreads` | Suggested `ringBufferCapacity` |
|---|---|---|---|
| IoT pilot | 1 k × 1 Hz | `0` (auto) or `2` | `65536` (default) |
| Auto factory | 5 k × 100 Hz | `4` | `262144` |
| Semi fab | 30 k × 10 kHz | `8` | `1048576` |
| HFT feed | 100 k × 1 MHz | `hw_concurrency / 2` | `4194304` |

Rule of thumb: size the ring buffer for ~10 ms of peak ingest so the
drain threads have time to catch up before the sync fallback kicks in.
Drain threads should not exceed the number of arena / partition lock
holders effectively competing.

---

## Tests

Seven new unit tests in `tests/unit/test_tick_plant.cpp`:

| Suite.Case | What it asserts |
|---|---|
| `IngestPhase1DrainThreads.SentinelZeroAutoAtLeastTwo` | `drain_threads=0` → `drain_thread_count() >= 2` after `start()` |
| `IngestPhase1DrainThreads.ExplicitValueHonoredExactly` | `drain_threads=4` → `drain_thread_count() == 4` |
| `IngestPhase1RingCapacity.DefaultIs65536` | Default config → `tick_plant().capacity() == 65536` |
| `IngestPhase1RingCapacity.CustomPowerOfTwoHonored` | `ring_buffer_capacity=262144` → capacity reflects 262144 |
| `IngestPhase1RingCapacity.NonPowerOfTwoRejected` | `100000` → ctor throws `std::invalid_argument` |
| `IngestPhase1RingCapacity.BelowRangeRejected` | `2048` (pow2 but < 4096) → throws |
| `IngestPhase1RingCapacity.AboveRangeRejected` | `33554432` (> 16 Mi) → throws |

No performance tests in this devlog — QA stage owns them.

Full regression: **1262 / 1262 tests pass** (was 1255 pre-Phase-1, +7
new).

---

## Non-goals (deferred to Phase 2 / BACKLOG P8)

1. **Horizontal ingest scale-out.** Stateless `zepto_ingest_node`
   binary that runs ingest-only and forwards over cluster RPC. With N
   ingest nodes and a shared data-node pool, aggregate ingest scales
   past a single pod's `hw_concurrency` ceiling.
2. **Ingest-rate HPA.** A custom metric (`zepto_pipeline_ticks_per_sec`)
   consumed by HPA to scale the ingest tier independently of the
   storage / query tier.
3. **Runtime capacity change.** Growing / shrinking the ring buffer
   without a restart (not planned; restart is fine for this knob).
4. **`store_tick()` fast-path restoration.** The sync fallback itself
   could be made faster — today its ~34× cliff is the safety net, not
   the target. Phase 1 raises the async ceiling so the cliff is rarely
   hit.

---

## Related devlogs

- `001_e2e_pipeline.md` — original drain-thread design
- `099_ingest_path_recovery.md` — `store_tick` column-pointer caching
  (the other side of the ingest path)
- `101_opcua_connector_poc.md` — previous devlog
