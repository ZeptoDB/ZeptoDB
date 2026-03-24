# Devlog 023: ARM Graviton Build Verification

**Date:** 2026-03-24
**Phase:** Cross-platform verification

## Summary

Completed ARM Graviton (aarch64) build and full test verification for ZeptoDB.
Confirmed 766/766 tests passing on both x86_64 and aarch64.

## Environment

| | x86 Instance | Graviton Instance |
|---|---|---|
| Architecture | x86_64 | aarch64 |
| CPU | Intel Xeon 6975P (8 vCPU) | Graviton (4 vCPU) |
| RAM | — | 15 GB |
| OS | Amazon Linux 2023 | Amazon Linux 2023 |
| Compiler | Clang 19.1.7 | Clang 19.1.7 |
| Highway SIMD | 1.2.0 | 1.2.0 |
| LLVM JIT | 19.1.7 | 19.1.7 |

## Build

- CMake + Ninja, Release mode
- 137/137 targets built successfully (identical on both sides)
- Only warnings present, no errors
- Python binding (`zeptodb.cpython-39-aarch64-linux-gnu.so`) generated successfully

## Test Results

| Test Suite | x86_64 | aarch64 |
|---|---|---|
| Unit Tests | 619/619 ✅ | 619/619 ✅ |
| Feed Tests | 21/21 ✅ | 21/21 ✅ |
| Migration Tests | 126/126 ✅ | 126/126 ✅ |
| **Total** | **766/766** | **766/766** |

## Bug Fix

`FIXMessageBuilderTest.BuildLogon` test failed on both sides.

- Cause: Created with `FIXMessageBuilder("ZEPTO", "SERVER")` but expected `49=APEX` — SenderCompID mismatch
- Fix: `49=APEX` → `49=ZEPTO` (tests/feeds/test_fix_parser.cpp:165)
- Not an ARM-specific issue, existing test typo

## Benchmark (Graviton)

| Metric | Graviton | x86 (previous) |
|---|---|---|
| xbar GROUP BY 1M | **7.99ms** | 24ms |
| ITCH Parser | 17.18 ns/msg (58.2M msg/s) | 23.3 ns/msg (42.9M msg/s) |
| FIX Parser | 358.97 ns/msg (2.79M msg/s) | — |

The 3x faster xbar on Graviton is likely due to HugePages fallback + memory access pattern differences.
Accurate comparison requires re-measurement under identical conditions (HugePages enabled, same core count).

## Notes

- HugePages not configured on Graviton instance → fallback warning in all tests (no functional impact)
- Parquet/S3 OFF on both sides (arrow-devel/aws-sdk not installed)
- UCX 1.12 linked successfully
