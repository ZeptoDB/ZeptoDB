# Devlog 084 — Graviton ARM64 LLVM 15/19 Header-Lib Mismatch Fix

## Symptom
Graviton ARM64 CI (Amazon Linux 2023 runner) failed at link time with ~200
`undefined reference to llvm::...` errors for `jit_engine.cpp` (CI run 24631525023).

## Root Cause
AL2023 runners have two LLVM installs:
- LLVM 15 (dnf `llvm-devel`) → `/usr/lib64/cmake/llvm`
- LLVM 19 (separate) → `/usr/lib64/llvm19/lib/cmake/llvm`

`find_package(LLVM REQUIRED CONFIG PATHS ...)` without `NO_DEFAULT_PATH` searches
system paths before the hint `PATHS`, so CMake picked LLVM 15. Compile used
LLVM 15 headers (old `ReturnInst(ctx, val, Instruction*)` ctor still exists),
but the link line pulled `/usr/lib64/llvm19/lib/libLLVM-19.so` (ctor replaced
by `InsertPosition` in 19) → undefined references.

CI log proof:
- `-- LLVM 15.0.7: /usr/lib64/cmake/llvm`
- link line contains `/usr/lib64/llvm19/lib/libLLVM-19.so`

## Fix
Add `NO_DEFAULT_PATH` to the Linux `find_package(LLVM)` call so the explicit
LLVM 19 `PATHS` win. One-word diff in `CMakeLists.txt`.

## Impact
No behavior change on single-LLVM hosts (x86_64 dev boxes, Ubuntu CI). Fixes
AL2023 Graviton runner where both LLVMs coexist.
