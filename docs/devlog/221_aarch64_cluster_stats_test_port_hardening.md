# 221: aarch64 Cluster Stats Test Port Hardening

Date: 2026-07-11
Status: Complete

## Context

The release aarch64 stage exposed a single parallel CTest failure in
`HttpCluster.DataNodeStats_IncludesIdHostPort`. The test used a synthetic port
offset and performed one immediate stats RPC after starting `TcpRpcServer`,
which left a small bind/readiness race under the heavily parallel Graviton
suite.

## Changes

- Switched the cluster stats RPC regression and hostname-resolution companion
  test to the shared kernel-assigned `pick_free_port()` helper.
- Added a bounded stats-readiness helper that retries until the expected stats
  marker is visible or a two-second deadline expires.

## Verification

- PASS:
  `./build/tests/zepto_tests --gtest_filter='HttpCluster.DataNodeStats_IncludesIdHostPort:HttpCluster.TcpRpcClient_ResolvesHostname' --gtest_brief=1`
- PASS:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`

## Follow-ups

- None expected if the rerun passes; this is a test isolation hardening only.
