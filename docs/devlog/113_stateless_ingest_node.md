# Devlog 113 — Stateless `zepto_ingest_node` binary (P8-I3)

Date: 2026-04-30
Scope: `tools/zepto_ingest_node.cpp` (new), `CMakeLists.txt`, `deploy/docker/Dockerfile.bench`, `deploy/docker/Dockerfile.bench.arm64`, `deploy/helm/zeptodb/templates/ingest-deployment.yaml` (new), `deploy/helm/zeptodb/values.yaml`, `docs/operations/KUBERNETES_OPERATIONS.md`.

---

## Problem

After devlog 111 (`CoordinatorRoutingAdapter`) and devlog 112 (DDL replication) the HTTP INSERT path is finally cluster-aware: every pod forwards a tick to the right owner. However, ingest and query/storage still share a pod. That means:

- scaling ingest means scaling query/storage (HPA on CPU couples them)
- a burst of INSERT traffic evicts query-side CPU headroom on the same pods
- HDB I/O, arena pressure, and ingest drain threads all share the same cgroup

P8-I3 introduces an **ingest-only** pod type so the two tiers can scale independently. The ingest pod holds zero data, runs no queries, and exists only to accept HTTP `INSERT` and forward each tick over RPC to the correct storage pod.

## Design decision — Option A: self_id=99999

`CoordinatorRoutingAdapter` (devlog 111) expects a tuple of `(router, mutex, local_pipeline, self_id, remote_clients)`. For symbols whose `PartitionRouter::route()` returns `self_id`, the adapter calls `local_pipeline->ingest_tick()` directly; otherwise it picks the matching `RpcClientBase` in the remote map.

Two ways to make a "pipeline that never stores":

- **Option A (adopted).** Pick a `self_id` that no storage pod uses (default: `99999`). The consistent-hash ring therefore never returns `self_id` from `route()`, so every tick flows through an RPC client. A real `ZeptoPipeline` is still constructed and passed to the adapter — not for storage, but so the `QueryExecutor`'s `SchemaRegistry` can resolve `table_name → table_id` when parsing an `INSERT`. The pipeline is configured `PURE_IN_MEMORY` with a 1-thread drain and a 4096-slot ring (the minimum) to keep memory usage negligible.
- **Option B (rejected).** Skip `add_local_node` entirely and let the ring contain only remotes. This would also work — the adapter already returns the closest node when `self_id` is not on the ring — but it leaves the `QueryCoordinator` in a state that differs from how every other binary uses it, and it would require a real code change in the adapter to handle the "self not on ring" path robustly. Option A needs **zero changes** to the adapter or the coordinator.

Option A also makes DDL replication trivially correct: when a CREATE/DROP/ALTER arrives at the ingest pod, `QueryExecutor::execute()` updates the local `SchemaRegistry` (which the executor needs for later INSERTs) and `HttpServer` then calls `QueryCoordinator::forward_ddl_to_remotes()` (devlog 112) so every storage pod gets the DDL too. No special-casing needed.

## Implementation

### New binary `tools/zepto_ingest_node.cpp` (~200 lines)

Structure mirrors `tools/zepto_http_server.cpp` but drops everything that touches data:

- No HA mode, no `--peer` / `--rpc-port` / `--hdb-dir` / `--storage-mode` / `--ticks` / `--web-dir` / `--tenant` / `--jwt-*`.
- No sample data seeding, no bootstrap CREATE TABLE.
- No rebalance manager, no `PartitionMigrator`, no `TcpRpcServer` (nobody forwards ticks *to* the ingest pod).
- Requires at least one `--add-node id:host:port`; errors out otherwise (a no-op ingest pod is never useful).

Flags kept: `--port` (default `8124`), `--node-id` (default `99999`), `--add-node` (repeatable), `--no-auth`, `--log-level`, `--help`.

Wire-up in order:

1. Minimal `PipelineConfig` (`PURE_IN_MEMORY`, 1 drain thread, 4096-slot ring).
2. `QueryCoordinator::add_local_node(self_addr, pipeline)` then `add_remote_node(...)` for each `--add-node`.
3. Peer `TcpRpcClient` pool keyed by `NodeId`. RPC port follows the project-wide convention `peer_http_port + 100`.
4. `CoordinatorRoutingAdapter` over `(coordinator->router(), coordinator->router_mutex(), &pipeline, node_id, &peer_rpc)`.
5. `AuthManager::Config { enabled = !no_auth }` (no API-key seeding, no rate limit, no audit buffer).
6. `QueryExecutor` + `executor.set_cluster_node(routing_adapter.get())`.
7. `HttpServer(executor, port, TlsConfig{}, auth)` + `set_coordinator(coordinator.get(), node_id)` + `set_ready(true)` + `start_async()`.
8. SIGINT/SIGTERM → `server.stop()`.

### Dockerfiles

Added `zepto_ingest_node` to the `ninja`, `strip`, and `COPY --from=builder` lines in both `Dockerfile.bench` and `Dockerfile.bench.arm64`. No new runtime dependencies — links against the same libraries as `zepto_http_server`.

### Helm chart (opt-in scaffold)

New template `deploy/helm/zeptodb/templates/ingest-deployment.yaml` — rendered only when `ingest.enabled=true`. Produces a `Deployment` + `Service` pair labelled with `app.kubernetes.io/component: ingest` so it is distinct from the storage tier.

Values (`values.yaml`):

```yaml
ingest:
  enabled: false        # opt-in
  replicas: 2
  noAuth: true
  extraArgs: []         # e.g. {--add-node,0:zeptodb-0.zeptodb-headless:8123,...}
  service:
    type: ClusterIP
  resources:
    requests: { cpu: "1000m", memory: "1Gi" }
    limits:   { cpu: "2000m", memory: "2Gi" }
```

**Storage-node discovery is intentionally a TODO.** A production-grade version needs an init container that resolves every storage pod's headless DNS entry into an `--add-node` flag. Until that lands, operators pass storage peers explicitly via `extraArgs`:

```bash
helm upgrade zeptodb ./deploy/helm/zeptodb -n zeptodb \
  --set ingest.enabled=true \
  --set-string 'ingest.extraArgs={--add-node,0:zeptodb-0.zeptodb-headless:8123,--add-node,1:zeptodb-1.zeptodb-headless:8123,--add-node,2:zeptodb-2.zeptodb-headless:8123}'
```

## Relationship to existing binaries

| Binary | Ingest | Storage | Query | HA | Use case |
|--------|:------:|:-------:|:-----:|:--:|----------|
| `zepto_http_server` | ✅ | ✅ | ✅ | optional | Standalone / full pod |
| `zepto_data_node` | via RPC | ✅ | ✅ | — | Leaf storage pod (multi-process tests) |
| `zepto_ingest_node` | ✅ (forwards) | ❌ | ❌ | — | Stateless ingest-only (P8-I3) |

All three share the same library targets (`zepto_server zepto_sql zepto_core zepto_storage zepto_ingestion zepto_cluster zepto_execution`). No new libraries introduced.

## Verification

```
$ ninja zepto_ingest_node zepto_tests
[2/2] Linking CXX executable zepto_ingest_node
$ ./zepto_ingest_node --help
Usage: ./zepto_ingest_node [options]
  --port PORT               HTTP port (default: 8124)
  --node-id N               Self node ID (default: 99999, must not collide with any storage node ID)
  ...
$ ./zepto_ingest_node --port 18200 --no-auth --add-node '1:127.0.0.1:18201' &
ZeptoDB ingest node: http://localhost:18200 (stateless, node_id=99999, forwarding to 1 storage node(s))
  Storage node 1 → 127.0.0.1:18201 (rpc 127.0.0.1:18301)
$ curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18200/ping
200
$ curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18200/health
200

$ ./tests/zepto_tests
[  PASSED  ] 1292 tests.
```

Baseline 1292/1292 preserved — no new tests added because the routing correctness is already covered by `test_coordinator_routing_adapter.cpp` (4 tests). The ingest node is a thin assembly of already-tested components.

Helm template sanity check with `helm template` confirms the Deployment + Service render only when `ingest.enabled=true` and that `extraArgs` appear verbatim in the container `args`.

## Follow-ups (not in scope)

- Storage-node discovery init container. Today the operator supplies `--add-node` flags by hand.
- Backpressure propagation from storage pods back to the ingest pod. Currently an overwhelmed storage pod will return `false` from `ingest_tick` and the ingest pod counts it as a drop.
- Python DSL equivalent (`PyPipeline` that targets an ingest pod's HTTP endpoint) — tracked as P8-I5.
- Ingest-rate HPA on `zepto_pipeline_ticks_per_sec` — tracked as P8-I4.
