# 096 — Flake tally update: `QueryCoordinator.TwoNodeRemote_OrderByLimit`

## Context

Full-matrix run on 2026-04-18 (`tools/run-full-matrix.sh --all`) hit 1
flake on the arm64 unit stage (stage 8, remote Graviton host). All other
stages passed.

## Test

`QueryCoordinator.TwoNodeRemote_OrderByLimit` — failed once, passed on
isolated rerun (`ctest -R '^QueryCoordinator\.TwoNodeRemote_OrderByLimit$'`).

## Classification

Member of the existing `tcp_rpc_pool` RESOURCE_LOCK family (see devlog
087). Verified 2026-04-18 that `tests/serial_tests.cmake` explicitly
enumerates `QueryCoordinator.TwoNodeRemote_OrderByLimit` inside the
`tcp_rpc_pool` `foreach` block (part of the full
`QueryCoordinator.TwoNodeRemote_*` list — 14 tests), so the test is
already serialised against its siblings in the TCP/RPC cluster family.
No cmake or .cpp change required.

## Pattern / tally

`tcp_rpc_pool` family flake observations since the RESOURCE_LOCK landed
(devlog 087):

1. devlog 087 itself documented 3 pre-fix observations across a 3×
   `ctest -j$(nproc)` QA pass
   (`TcpRpcServerThreadPool.ConcurrentRequestsWithSmallPool`,
   `TcpRpc.SqlQueryRoundTrip`, `CoordinatorHA.StandbyForwardsToActive`)
   — these motivated the lock.
2. devlog 090 noted `QueryCoordinator.TwoNodeRemote_DistributedAvg_Correct`
   flaking 1× in the full-suite run (post-fix; already covered by the
   wildcard).
3. This run (096): `QueryCoordinator.TwoNodeRemote_OrderByLimit`, 1×,
   arm64 stage 8.

Post-fix tally: 2 observations across ~6 months of full-matrix runs,
both in the `QueryCoordinator.TwoNodeRemote_*` subset, both passed on
isolated rerun. No regression trend; the RESOURCE_LOCK + wildcard
enumeration continues to hold the family at a tolerable flake rate.

## Action

Documentation only:

- Appended observation line to devlog 087 "`tcp_rpc_pool` family —
  post-fix flake observations" subsection.
- Added this devlog (096).
- Added bullet to `docs/COMPLETED.md`.

No code, no cmake, no test file changes.
