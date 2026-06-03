# 156 — open62541 Detection Hardening

**Date:** 2026-06-02
**Status:** Complete

## Context

Devlog 155 added live OPC-UA Historical Access and OPC-UA server mode, but the
build only checked for `libopen62541` with `find_library()`. That made live
validation brittle because source-built packages, distro packages, and vendor
SDK images expose open62541 through different metadata, and Historical Access
requires the separate `UA_ENABLE_HISTORIZING` build option.

## Changes

- `CMakeLists.txt` now detects open62541 through `find_package(open62541
  CONFIG)`, then `pkg-config`, then manual include/library fallback.
- The selected dependency is linked as an imported target instead of a raw
  library path, preserving include directories and target metadata.
- Configure now checks whether `UA_ENABLE_HISTORIZING` is present in the
  installed headers and reports `OPC-UA` plus `OPC-UA HA` in the build summary.
- The OPC-UA design doc now documents the detection order and the Amazon Linux
  package caveat.

## Verification

```bash
cmake -S /home/ec2-user/zeptodb -B /home/ec2-user/zeptodb/build
cmake -S /home/ec2-user/zeptodb -B /tmp/zeptodb-opcua-detect-check -DZEPTO_USE_OPCUA=ON
cmake --build build --target zepto_tests zepto-opcua-browse -j$(nproc)
./build/tests/zepto_tests --gtest_filter='OpcUaUaDatetime.*:OpcUaHistoricalAccess.*:OpcUaServerMode.*'
./build/zepto-opcua-browse --endpoint opc.tcp://127.0.0.1:4840
```

Default configure reports `OPC-UA: OFF` and `OPC-UA HA: OFF`. The
`ZEPTO_USE_OPCUA=ON` configure path correctly reports that open62541 is not
available in this Amazon Linux image and keeps the fail-closed no-open62541
build. The focused OPC-UA tests passed 11/11. The browse CLI returned the
expected default-build diagnostic exit code.

## Benefit

This reduces production bring-up risk: a deployment image now shows whether it
has live OPC-UA connectivity and live Historical Access before runtime, and
misbuilt open62541 packages no longer masquerade as fully supported HA builds.
