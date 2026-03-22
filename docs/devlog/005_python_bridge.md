# 005 — Phase D: Python Transpiler Bridge

**Date:** 2026-03-22
**Branch:** phase-d/python-bridge

---

## Overview

Phase D builds the bridge layer allowing quant researchers to manipulate APEX-DB directly from Python. All three core requirements specified in the design document (`layer4_transpiler_client.md`) were achieved:

1. **Zero-copy**: Expose C++ RDB memory as numpy arrays without copying
2. **Lazy Evaluation**: `.collect()` paradigm in the style of Polars
3. **Python to C++ Binding**: pybind11-based native module

---

## Implementation

### Part 1: pybind11 Binding (`src/transpiler/python_binding.cpp`)

**Choice: pybind11 v3.0.2 (over nanobind)**
Chose pybind11 to avoid nanobind's complex CMake integration and compatibility issues with the Python 3.9 / Amazon Linux 2023 environment. More mature and better documented — a safer practical choice.

**Core implementation: Zero-copy `get_column()`**

```cpp
// numpy directly references the raw ptr managed by RDB ArenaAllocator
py::capsule base(raw, [](void*) { /* no ownership */ });
return py::array_t<int64_t>(
    { static_cast<py::ssize_t>(nrows) },
    { sizeof(int64_t) },
    raw,
    base
);
```

Registered a no-op destructor in `py::capsule` to prevent numpy from freeing the memory. Actual memory ownership remains with `ApexPipeline` -> `ArenaAllocator`. Zero-copy is verified by `OWNDATA=False` on the result array.

**Resolving the drain() race condition**
A race in `ColumnVector::append()` occurred when the background drain thread and `drain_sync()` ran concurrently. Fix:

```cpp
// Poll until ticks_stored counter catches up to ticks_ingested
while (...) {
    if (stored >= target) break;
    sleep(1ms);
}
```

Completely removed direct `drain_sync()` calls and delegated entirely to the background thread.

---

### Part 2: Lazy DSL (`src/transpiler/apex_py/dsl.py`)

Reproduced the Polars `.lazy()` -> `.collect()` paradigm in pure Python.

**Expression tree structure:**

```
DataFrame(db, symbol=1)
  └── FilteredFrame (df[df['price'] > 15000])
        └── FilteredColumn (['volume'])
              └── _FilterSumNode -> filter_sum(symbol, 'volume', 15000)
                    └── LazyResult (not executed until collect())
```

**LazyResult caching:**

```python
result = df[df['price'] > 15000]['volume'].sum()
v1 = result.collect()  # C++ execution
v2 = result.collect()  # cache hit, no re-execution
```

**Supported operations:**
- `df.vwap()` -> `query_vwap()`
- `df.count()` -> `query_count()`
- `df[condition]['col'].sum()` -> `query_filter_sum()`
- `df['col'].collect()` -> `get_column()` (zero-copy numpy)

---

### Part 3: CMake Integration

The key was adding `CMAKE_POSITION_INDEPENDENT_CODE ON`. Static libraries were compiled without `-fPIC`, causing `R_X86_64_32S relocation` errors during `.so` linking. Fixed with a global PIC setting.

```cmake
# Apply -fPIC to all static libraries for Python .so linking
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
```

pybind11 CMake dir is dynamically discovered via `python3 -m pybind11 --cmakedir`.

---

### Part 4: Test Results

```
31 passed in 14.58s
```

All 31 Python tests passed. C++ tests 29/29 also maintained.

**Test coverage:**
- `TestBasicPipeline` (8): single ingest, VWAP, filter_sum, count
- `TestBatchIngest` (4): list/numpy batch, error handling
- `TestZeroCopy` (6): numpy view verification, OWNDATA=False check
- `TestStats` (3): stats dict structure
- `TestLazyDSL` (8): full DSL chain, caching, lazy values
- `TestMultiSymbol` (2): symbol isolation

---

### Part 5: Benchmark Results (Polars v1.36.1 vs APEX-DB, N=100K rows)

| Query | APEX | Polars Lazy | Speedup |
|-------|------|-------------|---------|
| VWAP | **56.9us** | 228.7us | **4.0x** |
| Filter+Sum | **66.9us** | 98.8us | **1.5x** |
| COUNT | **716ns** | 26.3us | **36.7x** |
| get_column | **522ns** | 760ns (Series) | **1.5x** |
| DSL chain | **66.1us** | 96.8us | **1.5x** |

**Notable results:**
- COUNT is **37x** faster than Polars: only needs to aggregate partition row counts — no actual scan
- VWAP **4x** faster: power of SIMD 8-way unroll + `__int128` integer accumulator
- Zero-copy `get_column()`: numpy OWNDATA=False confirmed, zero actual memory copies

**Polars Eager vs Lazy:**
Polars Eager VWAP (82.2us) is faster than Lazy (228.7us). On small datasets, Lazy's query planning overhead is actually a disadvantage.

---

## Troubleshooting Notes

### 1. `array::c_contiguous` removed in pybind11 3.0
The `py::array::c_contiguous` constant from pybind11 2.x was removed in 3.0.
Fixed by using only `py::array::forcecast`.

### 2. Link error with static libraries missing `-fPIC`
```
relocation R_X86_64_32S against `.rodata.str1.1' can not be used when making a shared object
```
Fixed with `set(CMAKE_POSITION_INDEPENDENT_CODE ON)` global setting.

### 3. drain() race condition (background thread vs drain_sync)
`drain_sync()` and background thread both calling `ColumnVector::append()` concurrently caused data loss (stored N-1 or N-2 instead of N).
Removed `drain_sync()`. Replaced with polling for `ticks_stored >= ticks_ingested` condition.

### 4. Arena exhausted (benchmark N=100K)
32MB arena per partition; continuous storage of 100K rows triggered out-of-memory log due to ColumnVector initial capacity expansion strategy.
Actual data stored correctly (no fallback on append failure). Workaround in benchmark: 50K chunk ingest. In production, the arena size must be set large enough.

---

## File List

```
src/transpiler/
  python_binding.cpp       # pybind11 C++ module (apex.so)
  apex_py/
    dsl.py                 # Polars-style Lazy DSL

tests/
  test_python.py           # Python binding tests (31 tests)
  bench/
    bench_python.py        # Polars vs APEX benchmark

docs/devlog/
  005_python_bridge.md     # this file

CMakeLists.txt             # pybind11 integration, -fPIC global setting
```

---

## Next Steps (Phase D follow-up)

- **FlatBuffers AST serialization**: Serialize complex query trees to C++ for transmission
- **Arrow C-Data Interface**: Convert `get_column()` return value directly to Polars DataFrame
- **Async Pipeline**: `asyncio` integration, `await db.drain()`
- **Arena auto-sizing**: Dynamic expansion based on partition data size prediction
