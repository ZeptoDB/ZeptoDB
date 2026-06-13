# 181: P2 Replication Cluster vs MPP Cluster Design

**Date:** 2026-06-13
**Area:** P2 Visibility & Launch / Distributed Cluster Design
**Status:** Complete

## Summary

Added the formal "Replication Cluster vs MPP Cluster" section to
`docs/design/phase_c_distributed.md`. The new section fixes the product and
engineering language for ZeptoDB's distributed story:

- replication is the durability and availability layer;
- MPP-style shard ownership, routed writes, and scatter-gather reads are the
  scale-out layer;
- ZeptoDB intentionally combines both instead of positioning itself as a
  replicated single-node OLAP database.

## Design Notes

The section now distinguishes three ideas that were previously spread across
the backlog and implementation notes:

1. A replication cluster improves availability and read redundancy, but does
   not remove the single logical write owner or hot-node memory/CPU ceiling.
2. An MPP cluster distributes data ownership, routes writes to partition
   owners, and pushes query fragments close to the owned data.
3. ZeptoDB's current distributed implementation is shard-aware and
   MPP-capable for selected query shapes, while still documenting that it is
   not yet a full general-purpose MPP SQL optimizer.

The comparison table is deliberately sales-friendly but implementation-grounded
so it can be reused in launch collateral without overstating current behavior.

## Scope Boundary

No code changed. The document records the current system boundary:

- implemented: `PartitionRouter`, routed ingest, WAL replication,
  `QueryCoordinator` scatter-gather, fencing, coordinator HA, ring broadcast,
  live rebalancing, and fire-and-forget DDL replication;
- not yet complete: cost-based distributed planning, arbitrary cross-node
  joins/windows/DISTINCT, replica-read placement, RDMA/CXL data-plane paths,
  and global cross-shard transactions.

## Documentation Sync

- Closed the P2 backlog row for "Replication-cluster vs MPP-cluster design
  doc".
- Added the completion to `docs/COMPLETED.md`.
- Updated backlog counts and next-action ordering.

## Verification

Docs-only change. Verification run:

- `git diff --check`
- `.github/scripts/check_english_first.sh`
- `.github/scripts/check_docs_updated.sh`
