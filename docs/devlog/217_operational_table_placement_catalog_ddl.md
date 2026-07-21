# 217: Operational Table Placement Catalog And DDL

Date: 2026-07-11
Status: Complete

## Context

Experiment 012 validated explicit placement for small operational/control
tables, but placement still lived as an admin runtime override. Production
operators need placement to travel with table metadata and survive coordinator
restart.

## Changes

- Added placement metadata to `TableSchema` and schema-catalog JSON durability.
- Added `CREATE TABLE ... WITH (placement = hash_by_table)` and
  `WITH (placement = pinned_node, node_id = N)` parsing and validation.
- Added `QueryCoordinator::apply_catalog_table_placements()` and automatic
  catalog placement re-apply after local/remote node registration and DDL.
- Persisted successful `set_table_placement()` / `clear_table_placement()`
  admin updates back to the local schema catalog.
- Updated SQL, HTTP, C++, distributed design, governance, backlog, and
  completion docs.

## Verification

- Added focused SQL parser/executor, schema-catalog durability, and distributed
  coordinator restart tests.
- Full x86_64 CTest:
  `ninja -C build -j$(nproc) zepto_tests && cd build && ctest -j$(nproc) -E "Benchmark\\.|K8s" --output-on-failure --timeout 180`
  - 1742/1742 passed; live S3 opt-in skipped.
- Full aarch64 Graviton stage:
  `./tools/run-full-matrix.sh --stages=8 --force-resync`
  - 1742/1742 passed; live S3 opt-in skipped.

## Follow-ups

- Keep placement experimental until rebalance/failover semantics and operator
  guidance are promoted separately.
- Continue with bounded small-table JOIN limits/feature-rule hardening.
