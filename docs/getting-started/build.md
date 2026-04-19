# Build from Source

```bash
git clone https://github.com/zeptodb/zeptodb.git
cd zeptodb

mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)
```

## Run Tests

```bash
# C++ unit tests (1300+), single process / serial
./tests/zepto_tests

# Parallel run via ctest — one process per test, scales with cores:
ctest -j$(nproc) --output-on-failure

# Python tests (208)
python3 -m pytest ../tests/test_python.py -v
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` / `Debug` / `RelWithDebInfo` |
| `ZEPTO_ENABLE_JIT` | `ON` | LLVM JIT compilation |
| `ZEPTO_ENABLE_SIMD` | `ON` | Highway SIMD vectorization |
| `ZEPTO_ENABLE_UCX` | `ON` | UCX/RDMA transport |

## ARM Graviton

Verified on aarch64 (Amazon Linux 2023, Clang 19.1.7) — 766/766 tests passing.

```bash
# Same build commands work on Graviton
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)
```
