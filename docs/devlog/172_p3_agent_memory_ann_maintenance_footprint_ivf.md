# 172: P3 Agent Memory ANN Maintenance, Footprint, And IVF

Date: 2026-06-09
Status: Complete

## Context

P3 Agent Memory already had exact scan, sparse-projection ANN, optional HNSW,
clustered fixtures, and real embedding fixtures. The remaining stronger-ANN
work was to make live update/delete maintenance cheaper, expose memory and
sidecar footprint, and add a dependency-free IVF comparison path before choosing
a production default.

## Changes

- Added ANN `remove()` and `update_row_id()` maintenance to sparse projection,
  HNSW, and IVF paths, with active/tombstone tracking and row-id remaps after
  compacting deletes.
- Updated `AgentMemoryStore` writes/deletes so clean ANN indexes maintain
  embedding updates, partition changes, deletes, and moved rows incrementally,
  falling back to dirty rebuild only when maintenance cannot be applied.
- Added `AnnIndexStats::memory_bytes` and `tombstone_entries`, plus
  `AgentMemoryStats` fields for ANN memory bytes and persisted
  `records.bin` / `vectors.bin` / total sidecar bytes.
- Extended Agent Memory stats wire encoding, local and cluster HTTP stats JSON,
  and Prometheus metrics with ANN footprint, ANN tombstones, and sidecar byte
  gauges. The wire format keeps the prior stats prefix stable and reads the new
  footprint fields as an optional tail so newer coordinators can still decode
  older remote stats payloads during rolling upgrades.
- Added `AgentMemoryAnnMode::Ivf` and `IvfAnnIndex`, a dependency-free
  inverted-file baseline with per-partition online centroids and configurable
  list probing.
- Added `--agent-memory-ann ivf`,
  `--agent-memory-ann-ivf-centroids`, and
  `--agent-memory-ann-ivf-probe` to `zepto_http_server`.
- Extended `bench_agent_memory` with `--ann ivf`,
  `--ann-ivf-centroids`, `--ann-ivf-probe`, ANN memory/sidecar output, and
  compare profiles `ivf_fast` and `ivf_recall`.
- Updated C++/HTTP/design/backlog/completed docs to reflect update/delete ANN
  maintenance, footprint telemetry, and IVF support.

## Verification

- `cmake --build build --target zepto_tests bench_agent_memory -j$(nproc)`
- `cmake --build build --target zepto_http_server -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryStoreTest.Ann*:AgentMemoryStoreTest.Ivf*:AgentMemoryStoreTest.Hnsw*:AgentMemoryWireTest*:AgentMemoryStoreTest.SnapshotStatsTrackSuccessAndFailures:AgentMemoryHttpTest.ExposesStatsAndPrometheusMetrics'`
  passed 11/11 tests.
- `./build/tests/zepto_tests --gtest_filter='AgentMemoryWireTest*'` passed,
  including legacy stats payload decoding with missing footprint fields.
- `./build/bench_agent_memory --fixture real --embedding-file tests/fixtures/agent_memory_real_embeddings.txt --iters 1 --compare-ann --recall-queries 2 --skip-snapshot`
  completed with `ivf_fast` and `ivf_recall` at recall@16 1.000/1.000 on the
  small real embedding fixture.
- `./build/bench_agent_memory --records 128 --dim 16 --iters 1 --ann ivf --ann-min-records 1 --ann-ivf-centroids 8 --ann-ivf-probe 2 --recall-queries 2 --fixture clustered --clusters 8`
  completed with recall@K 1.000 and reported `ann_memory_bytes=15488`,
  `ann_tombstones=0`, and sidecar total bytes `68790`.

## Follow-Ups

- Run larger production embedding-dump profiles before changing the default ANN
  policy away from exact scan.
- Evaluate tenant-filter-heavy workloads when choosing the default ANN mode and
  candidate limits.
- Persisted ANN index sidecars remain optional future work if rebuild time,
  rather than scan latency, becomes the dominant operational cost.
