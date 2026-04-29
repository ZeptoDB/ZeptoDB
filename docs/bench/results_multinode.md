# Multi-Node Benchmark Results (EKS) — 2026-04-28

**Cluster:** EKS `zepto-bench` (1.35, Auto Mode), region ap-northeast-2
**Instance:** c6i.xlarge (amd64) / r8g.xlarge equivalent (arm64 / Graviton)
**Image:** `060795905711.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb:bench-multinode-10a8268-{amd64,arm64}`
**Commit:** `10a8268` (Sprint 1–3 OPC-UA + ingest scale-out + cluster-aware routing)
**Driver:** 8 parallel `curl` clients from a loadgen pod on a distinct bench node
**Method:** `DROP TABLE; CREATE TABLE; sample /stats before; run 20s; sample /stats after; rate = Δ / 20`

---

## TL;DR

**Horizontal ingest scale-out does NOT happen today.** The multi-pod deployment serves ingest correctly at the protocol level (pods start, join cluster, respond to /admin/cluster), but HTTP `INSERT` statements bypass `PartitionRouter` entirely and land on whichever pod the Kubernetes Service LB happens to pick. As a result:

- **Ingest rate is flat across 1 / 2 / 3 pods.**
- **Per-pod stats show all writes land on exactly one pod**, not distributed across the ring.
- **`SELECT count(*)` against the cluster LB returns only the count from whichever pod received it** — aggregate correctness is not enforced across pods.

This is a known code path — `tools/zepto_http_server.cpp:297-304` has an explicit `TODO(devlog 103)` to call `executor.set_cluster_node(&cluster_node)`. The `QueryExecutor` routing path itself (devlog 103) is correctly implemented and unit-tested (`tests/unit/test_distributed_insert.cpp` — 4 tests, all pass with an in-test `CountingClusterNode`). The gap is the binary-level wire-up.

---

## Raw numbers

### amd64 (c6i.xlarge)

| N pods | Ingest rate | Per-pod distribution (writes landed on) |
|---|---|---|
| 1 | 379 /s | pod-0 only (sole pod) |
| 2 | 381 /s | pod-0: +11,430 • pod-1: 0 |
| 3 | 341 /s | pod-0: 0 • pod-1: +6,813 • pod-2: 0 |
| 1 (re-run) | 366 /s | pod-0 only |

### arm64 (Graviton)

| N pods | Ingest rate | Per-pod distribution |
|---|---|---|
| 1 | 460 /s | pod-0 only |
| 2 | 461 /s | (not captured per-pod, same flat pattern) |
| 3 | 448 /s | pod-0: 0 • pod-1: +8,967 • pod-2: 0 |

### Architecture comparison

- arm64 N=1 = **460 /s** vs amd64 N=1 = **379 /s**: **arm64 +21% faster single-pod**
- Consistent with `docs/bench/eks_arch_benchmark_report.md` finding that Graviton wins concurrent ingest
- Both architectures show the same flat scaling curve, confirming the bottleneck is binary-level (routing), not arch-specific

---

## Correctness check (B3)

**3-pod arm64 state after bench:**

```
cluster-wide SELECT count(*)        → 8967  (via LB; actually from pod-1)
sum(volume)                         → 134,505,000

Per-pod via headless service:
  pod-0: 0    rows
  pod-1: 8967 rows  ← all writes landed here
  pod-2: 0    rows
```

**This is a correctness failure for any multi-pod deployment that assumes writes distribute.** The numbers happen to be "consistent" only because all data is on one pod and every SELECT accidentally hits the same pod.

---

## Why the flat curve — root cause

File: `tools/zepto_http_server.cpp`, lines 297-304 (commit 10a8268):

```cpp
// HTTP server
zeptodb::sql::QueryExecutor executor(pipeline);
// TODO(devlog 103): when zepto_http_server is wired into cluster mode
// (i.e. a ClusterNode<Transport> is constructed here), call
//   executor.set_cluster_node(&cluster_node);
// so that INSERT statements route to the partition owner via
// PartitionRouter instead of landing on whichever pod received the
// HTTP request. Currently single-node HTTP only → null cluster_node
// preserves original direct-to-pipeline behaviour.
zeptodb::server::HttpServer server(executor, port, ...);
```

Devlog 103 shipped the `QueryExecutor::set_cluster_node()` API and the `ClusterNodeBase` abstraction, with 4 passing unit tests in `test_distributed_insert.cpp` that verify correct routing when a stub `CountingClusterNode` is injected. But the production binary never constructs a real `ClusterNode<TcpBackend>` and never calls the setter. The existing HTTP-server cluster plumbing (`QueryCoordinator`, `PartitionRouter` for rebalance, `TcpRpcServer` for query fan-out) covers **reads only** — SQL `INSERT` still calls `pipeline_.ingest_tick()` directly on whatever pod received the HTTP request.

### What the other scaling fixes do (and don't) address

| Fix | Devlog | Works | Does not solve |
|---|---|---|---|
| Phase 1 drain threads + ring capacity | 102 | Single-pod vertical ingest (bench needed to confirm) | Multi-pod |
| Pod placement hardening | 104 | Pods land on distinct nodes (confirmed: 3 NodeClaims across 3 AZs) | Write routing |
| `ClusterNodeBase` + QueryExecutor branch | 103 | **Library-level API exists**, unit-tested | Binary-level wire-up |
| `set_routing()` for Kafka/MQTT/OPC-UA consumers | 101/081/Kafka | Feed-driven ingest routes correctly | HTTP/SQL INSERT path |

The feed consumers (Kafka / MQTT / OPC-UA) are the only ingest paths that currently route across pods correctly, because they accept a `set_routing()` API at startup. Any deployment driving ingest through HTTP `INSERT` — including every benchmark in this repo that uses curl or `bench_rebalance` — is silently running single-pod.

---

## What shipped correctly

Despite the scaling null-result, three important findings:

1. **Pods deploy correctly on distinct nodes.** Hard antiAffinity + topologySpreadConstraints from devlog 104 work as designed — 3 pods, 3 NodeClaims, 3 AZs (a/b/c), zero co-location.
2. **Cluster membership works.** `/admin/cluster` on every pod returns `"mode":"cluster","node_count":N`. The ring is correctly formed.
3. **Rolling scale-up/down works.** 1 → 2 → 3 → 1 transitions complete in ~90s each with zero pod crashes.
4. **Cost discipline works.** `./tools/eks-bench.sh sleep` cleaned up all NodeClaims + cluster teardown on exit. Total bench cost ≈ $2 for cluster time.

---

## Required follow-up

Single backlog item blocks real horizontal scaling:

**P8 — wire `zepto_http_server` to construct a `ClusterNode<TcpBackend>` and call `executor.set_cluster_node(&cluster_node)`**

Effort: M. Dependencies: none (all prerequisite APIs shipped in devlog 103). Unblocks:
- Linear ingest scale-out with pod count on HTTP INSERT path
- Correct multi-pod aggregates
- Real "horizontal scale-out" claim in documentation

Suggested acceptance criteria:
- N=3 amd64 ingest rate is ≥2× N=1 (allowing for RPC overhead)
- Per-pod `stats().ticks_ingested` distribution within 20% of uniform
- `SELECT count(*)` via LB matches sum of per-pod counts

---

## Separate issue — OOM under partition explosion

During early testing, we OOMKilled a pod when a curl loop generated distinct symbol IDs without bound. Each new `(table_id, symbol_id)` allocates a 32 MB arena. At ~210 partitions × 32 MB ≈ 6.7 GB, the 12 GB pod limit was hit.

This is not a regression — it's a known sizing consideration documented in `docs/operations/KUBERNETES_OPERATIONS.md` — but the default chart resource limits (4 GiB → 8 GiB after devlog 104) can OOM under workloads with thousands of distinct symbols. For factory workloads with stable symbol sets this is fine; for bench runs that sweep many symbol IDs, either:
- Use a bounded symbol range (bench did this: 80 symbols across 8 workers)
- Lower arena size per partition (requires code change, not exposed via Helm)
- Increase memory limits (10s of GB for full market-data-scale symbol counts)

---

## Files

- Benchmark driver: `/tmp/ingest-clean.sh` (ephemeral, not committed)
- Helm values: `/tmp/bench-values-x86.yaml`, `/tmp/bench-values-arm64.yaml` (ephemeral)
- Image tags: `zeptodb:bench-multinode-10a8268-{amd64,arm64}` in ECR
- Git commit: `10a8268`

---

## Bottom line

Three sprints of infrastructure work (devlogs 102/103/104) **prepared** the engine for horizontal scale-out — ring-based routing, stateful placement, lock-free multi-thread ingest, test coverage — but the final 5-line wire-up in `zepto_http_server.cpp` is missing. Until that lands, multi-pod EKS deployments of ZeptoDB serving HTTP traffic are effectively running as single-pod from an ingest perspective. Queries happen to return consistent results only because all data is co-located on one pod by accident.

The engine is fast (arm64 N=1 = 460 /s over HTTP curl; in-process bench_pipeline measured 5.5M /s). The scaling limitation is architectural wiring, not engine capacity.
