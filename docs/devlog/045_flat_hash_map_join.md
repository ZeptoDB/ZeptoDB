# Devlog 045: FlatHashMap — Join Operator Hash Map Replacement

**Date**: 2026-04-02
**Status**: ✅ Complete

## Summary

Replaced all `std::unordered_map` usage in join operators (ASOF, Hash, Window) with a custom `FlatHashMap` using CRC32 hardware-accelerated hashing and open-addressing linear probing.

## Motivation

`std::unordered_map` is node-based — every bucket is a heap-allocated linked list node. This causes:
- Pointer chasing on every lookup → cache misses
- Poor memory locality for build/probe patterns
- Multi-cycle hash computation (default `std::hash`)

Join operators are the primary consumer of hash maps in the execution engine, and build/probe is on the critical path for every JOIN query.

## Design

### FlatHashMap (`include/zeptodb/execution/flat_hash_map.h`)

- **Hash**: `_mm_crc32_u64` (SSE4.2) — single-cycle hardware instruction. Fallback to splitmix64 on non-x86.
- **Probing**: Linear probing with power-of-2 capacity. Cache-friendly sequential access.
- **Load factor**: ≤ 75% (capacity = next power of 2 above `entries * 4/3`). Expected probe length < 2.
- **No erase**: Join maps are build-once, probe-many. No tombstone overhead.
- **API**: `operator[]` for insert/access, `find()` returns pointer (nullptr = miss), `for_each()` for iteration.

### Key specialization

Specialized for `int64_t` keys (symbol IDs, timestamps) — the only key type used in join operators.

## Changes

| File | Change |
|------|--------|
| `include/zeptodb/execution/flat_hash_map.h` | New: FlatHashMap implementation |
| `src/execution/join_operator.cpp` | Replace all `std::unordered_map` with `FlatHashMap` |
| `tests/unit/test_flat_hash_map.cpp` | New: 9 unit tests |
| `tests/CMakeLists.txt` | Add test file |

## Affected operators

- `AsofJoinOperator::execute` — symbol grouping
- `HashJoinOperator::execute` — INNER/LEFT/RIGHT/FULL build/probe
- `WindowJoinOperator::execute` — symbol grouping + time range lookup

## Test results

- 9 new FlatHashMap unit tests: all pass
- 23 existing HashJoin tests: all pass (regression-free)
- 52 total join/window-related tests: all pass
