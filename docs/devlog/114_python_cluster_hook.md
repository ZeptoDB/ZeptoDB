# Devlog 114 — Python cluster hook (P8-I5)

Date: 2026-05-02
Scope: `src/transpiler/python_binding.cpp`, `CMakeLists.txt`, `zepto_py/__init__.py`, `tests/test_python.py`, `docs/api/PYTHON_REFERENCE.md`

---

## Problem

Devlog 103 landed the core library-level fix for cluster-aware `INSERT`
routing: `ClusterNodeBase` abstract base, `QueryExecutor::set_cluster_node()`
setter, and a branch in `exec_insert`. On the C++ side it was wired into
the HTTP server (devlog 111) and the stateless ingest node (devlog 113).

The Python binding held a matching `cluster_node_` member and an
`ingest_routed_()` helper at the three `ingest` call sites, but there was
no way to **set** it from Python. The member was effectively dead code,
flagged in-source with:

```cpp
// TODO: Python cluster hook (BACKLOG P8-I5).
zeptodb::cluster::ClusterNodeBase* cluster_node_ = nullptr;
```

As a result, an in-process `zeptodb.Pipeline` used as a cluster node (e.g.
a research harness, a notebook-driven ingest, or a custom front-door)
would silently land every INSERT in its own local pipeline, mis-partitioning
the cluster exactly the way devlog 103 set out to fix.

## Design (references: devlogs 103, 111, 113)

Reuse the exact same bootstrap pattern as `tools/zepto_ingest_node.cpp`
(devlog 113, lines 136-170) but as member initialisation on `PyPipeline`:

- `QueryCoordinator` owns the shared `PartitionRouter`
- `CoordinatorRoutingAdapter` (non-template `ClusterNodeBase`
  implementation from devlog 111) reads the router + a peer `TcpRpcClient`
  map and routes each tick to the owning node
- Adapter returns `false` when the owner is unknown — caller (pybind11)
  surfaces this as a `RuntimeError` (same as the legacy "queue full"
  path)

The Python signature mirrors the ingest-node CLI:

```python
Pipeline.enable_cluster_routing(
    self_id: int,
    peers: list[tuple[int, str, int]],   # (node_id, host, http_port)
    remove_self_from_ring: bool = True,
    rpc_timeout_ms: int = 2000,
) -> None
```

`remove_self_from_ring=True` matches the **Option A** ingest-only pattern
from devlog 113 (self is not on the ring, so every tick forwards to a
remote). Set it to `False` when this pipeline is a full cluster node
that should also own a slice of the ring.

### Ownership + destruction order

Three new `PyPipeline` members (devlog 103's existing `cluster_node_` plus
three new owners):

```cpp
std::unique_ptr<zeptodb::cluster::QueryCoordinator> coordinator_;
std::unordered_map<NodeId, std::shared_ptr<RpcClientBase>> peer_rpc_;
std::unique_ptr<zeptodb::cluster::CoordinatorRoutingAdapter> routing_adapter_;
zeptodb::cluster::ClusterNodeBase* cluster_node_ = nullptr;
```

Lifetime contract (`CoordinatorRoutingAdapter` documents it): the adapter
holds raw pointers into the router inside `coordinator_`, into `*pipeline_`,
and into `peer_rpc_`. All three must outlive the adapter. C++ member
destruction order (reverse declaration order) — adapter first, then map,
then coordinator — happens to be correct by accident. The idempotent
teardown inside `enable_cluster_routing()` reset them explicitly in the
same order for clarity:

```cpp
cluster_node_ = nullptr;              // executor no longer reads the adapter
if (executor_) executor_->set_cluster_node(nullptr);
routing_adapter_.reset();             // adapter holds the raw ptrs
peer_rpc_.clear();                    // map holds the shared TCP clients
coordinator_.reset();                 // owns the router
```

### Lazy-executor path

`PyPipeline::execute_sql()` creates the `QueryExecutor` on first call. If
`cluster_node_` is already non-null at that point (user called
`enable_cluster_routing()` before the first SQL), the lazy path now also
wires the executor:

```cpp
if (!executor_) {
    executor_ = std::make_unique<zeptodb::sql::QueryExecutor>(*pipeline_);
    if (cluster_node_) executor_->set_cluster_node(cluster_node_);
}
```

## Implementation

### `src/transpiler/python_binding.cpp`

Concrete line ranges after this change:

| Section | Lines | Notes |
|---|---|---|
| New includes (coordinator / adapter / tcp_rpc / transport / `unordered_map`) | 13-30 | replaces the `cluster_node_base.h`-only include |
| `execute_sql` lazy-init wires cluster node | 280-284 | |
| `enable_cluster_routing` method body | 320-418 | mirrors `tools/zepto_ingest_node.cpp:136-170` |
| `ingest_routed_()` helper (unchanged from devlog 103) | 498-503 | |
| New members (`coordinator_`, `peer_rpc_`, `routing_adapter_`, `cluster_node_`) | 552-565 | replaces the stale `TODO: P8-I5` comment |
| `.def("enable_cluster_routing", ...)` | 686-708 | registered after `is_ready` |

Argument validation is explicit: the tuple loop pre-parses every peer
into a local vector *before* touching any of the four unique_ptrs, so a
`TypeError` or `ValueError` from a malformed peer tuple leaves the
pipeline in its prior state (either uninitialised or still holding the
previous wiring) — a proper strong-exception guarantee for the Python
caller.

Pybind11 rejection uses `py::type_error` and `py::value_error` (mapped to
Python `TypeError` / `ValueError`) so the caller can catch them
normally.

### `CMakeLists.txt`

The pybind11 `zeptodb` module previously linked against `zepto_core`,
`zepto_storage`, `zepto_ingestion`, `zepto_execution`, `zepto_sql`. Added
`zepto_cluster` (provides `TcpRpcClient`, `QueryCoordinator`,
`PartitionRouter` symbols the new method needs). One line added.

### `zepto_py/__init__.py`

Package docstring only — a "Cluster routing" section pointing at the
pybind11 method and its reference doc. The C++ binding is the public API
for this feature; wrapping it in a higher-level Python class would
duplicate argument validation and obscure the error surface.

### `tests/test_python.py`

New `TestClusterRouting` class with five test methods (one extra beyond
the four required by the task, covering the `self_in_ring` path):

| Test | What it proves |
|---|---|
| `test_enable_cluster_routing_empty_peers` | Zero peers + `remove_self_from_ring=True` does not raise; the pipeline still serves local queries (tail use case: reconfiguring a standalone pipeline). |
| `test_enable_cluster_routing_self_in_ring_ingests_locally` | `remove_self_from_ring=False` with self as the only node → adapter routes every tick through the local branch. End-to-end ingest + count round trip passes. |
| `test_enable_cluster_routing_idempotent` | Calling twice with different `self_id` and different peer configuration; first-wiring local data is still queryable; no crash, no leak. |
| `test_enable_cluster_routing_unreachable_peer` | `self_id=99999` + peer on a closed port. INSERT either raises a clean Python exception or returns; forbidden outcome is a segfault (pytest would surface rc != 0). |
| `test_enable_cluster_routing_bad_tuple` | `peers=[(0, "host")]` (missing port) must raise `TypeError` / `ValueError` from the binding, not crash. |

Rationale for five rather than four: the empty-peers case only covers
the teardown path where the adapter is constructed-but-never-invoked;
the `self_in_ring` case covers the adapter's *local* branch, which the
empty-peers test deliberately skips by keeping self out of the ring.
Without it the local branch would be exercised only by a separate
`test_coordinator_routing_adapter.cpp` suite (devlog 111), never by the
Python hook.

## Tests

```
$ cd build && ninja -j$(nproc) zepto_tests zeptodb
[2/2] Linking CXX shared module zeptodb.cpython-39-x86_64-linux-gnu.so

$ ./tests/zepto_tests --gtest_filter='CoordinatorRoutingAdapter*:DistributedInsert*:ClusterNode*'
13 tests run, 13 passed. (Round-trips through CoordinatorRoutingAdapter,
DistributedInsert, and the ClusterNode templated routing — all still
green after the Python module linked zepto_cluster.)

$ cd .. && python3 -m pytest tests/test_python.py -v -k cluster_routing
TestClusterRouting::test_enable_cluster_routing_empty_peers                         PASSED
TestClusterRouting::test_enable_cluster_routing_self_in_ring_ingests_locally        PASSED
TestClusterRouting::test_enable_cluster_routing_idempotent                          PASSED
TestClusterRouting::test_enable_cluster_routing_unreachable_peer                    PASSED
TestClusterRouting::test_enable_cluster_routing_bad_tuple                           PASSED
5 passed in 0.15s
```

Full `tests/test_python.py`: 31 passed pre-change, 36 passed post-change
(1 new `TestClusterRouting` class with 5 test methods — four required by
P8-I5 plus one extra covering the `self_in_ring` local-branch path). One
pre-existing unrelated flake in `TestZeroCopy.test_get_column_values` —
test-ordering pollution of the `symbol=102` partition, reproduces on
`main` with the change stashed). No regression attributable to this
change.

No new compiler warnings on `python_binding.cpp` (`-Wno-pedantic
-Wno-unused-parameter -O3 -march=native` flags unchanged; clean build on
x86_64 with clang-19).

## Build verification (x86_64 stage)

```
$ cd build && ninja -j$(nproc) zepto_tests zeptodb
[1/2] Building CXX object CMakeFiles/zeptodb.dir/src/transpiler/python_binding.cpp.o
[2/2] Linking CXX shared module zeptodb.cpython-39-x86_64-linux-gnu.so
```

Clean build; no warnings on the modified TU.

Cross-arch status (Option A, see `.kiro/skills/cross-arch-verification`):

- **x86_64**: verified locally (clang-19, clean build, tests above green).
- **aarch64**: pending EKS bench cluster run — no Graviton builder is
  available in this development environment. aarch64 will be verified
  as the first test on the EKS bench cluster; this devlog will be
  updated with the result once the run completes. The code is
  arch-neutral (no SIMD intrinsics, no struct-layout changes, no
  compile-time platform guards touched), so the EKS run is expected to
  be a formality rather than a discovery step.

## Files changed

| File | Change |
|---|---|
| `src/transpiler/python_binding.cpp` | +105 / −4: new includes, `enable_cluster_routing` method, four new members replacing the stale TODO, executor wire-up in the lazy-init path, pybind11 `.def` |
| `CMakeLists.txt` | +1: link the Python module against `zepto_cluster` |
| `zepto_py/__init__.py` | +24: docstring section pointing at the new method |
| `tests/test_python.py` | +101: `TestClusterRouting` class with 5 tests (no existing tests modified) |
| `docs/api/PYTHON_REFERENCE.md` | full signature + example |
| `docs/devlog/114_python_cluster_hook.md` | **new** — this file |
| `docs/BACKLOG.md` | P8-I5 row removed from Horizontal Ingest table; ✅ Done footer updated; Summary P8 count decremented; `last 113 → last 114` header bump |
| `docs/COMPLETED.md` | new bullet under 2026-05-02 |

## Follow-ups

None blocking. Noted for future work but not required by P8-I5:

- **`zepto_py` HTTP-client equivalent.** `zepto_py.ZeptoConnection` talks
  to a remote `zepto_http_server` over HTTP. Cluster routing there is
  handled server-side by `CoordinatorRoutingAdapter` (devlog 111), so no
  client-side method is needed. The ingest-node pattern would only be
  useful for a Python-hosted stateless front-door; out of scope.
- **`enable_cluster_routing(..., peers=[...])` host validation.** We
  accept any string hostname without a DNS lookup; the first bad peer is
  only discovered when the first non-local tick tries to connect, at
  which point `TcpRpcClient` surfaces a clean false. Adding a preflight
  connect would increase startup cost and mask the per-tick failure
  handling that `ingest_routed_` already has.
- **`disable_cluster_routing()`**. Not added — callers can pass an empty
  `peers=[]` with `remove_self_from_ring=False` and `self_id=<own_id>`
  to revert to single-node routing, or simply call
  `enable_cluster_routing` with an empty peer list. Adding a dedicated
  disable would be a second teardown codepath; the idempotent rebuild
  already covers the case.
