# Layer 3: Vectorized Query Execution Engine & JIT

This document is the design specification for the **Execution Engine** that maximally parallel-processes the vast tick data loaded in memory within a single clock cycle to produce results. It combines ClickHouse's vectorized execution with JIT technology.

## 1. Architecture Diagram

```mermaid
flowchart TD
    SQL_AST["Logical Query Plan\n(Python AST / Query)"] --> Optimizer["C++ Query Optimizer\n(pipeline minimization)"]

    Optimizer --> OperatorTree["Physical Plan\n(DAG Operators)"]

    OperatorTree --> JIT_Path{"Is it a simple expression?"}
    JIT_Path -- "No: Complex conditions (math ops etc)" --> LLVM["LLVM JIT Compiler\nRuntime compilation to machine code"]
    JIT_Path -- "Yes: Simple filter/sum" --> VectorPath["Pre-compiled\nVectorized Execution"]

    subgraph DataFlow["Data Flow"]
        vector_chunk["RDB DataBlock\n(e.g., 8192-row blocks)"]
    end

    LLVM --> Engine[Hardware Intrinsics Core\nSIMD (AVX-512 / ARM SVE)]
    VectorPath --> Engine
    vector_chunk --> Engine

    Engine --> Output["Final aggregate result"]
    Engine -. "Offload intense computation" .-> FPGA["CXL-based FPGA/GPU\n(Options Greeks, etc.)"]
```

## 2. Tech Stack
- **JIT framework:** LLVM / MLIR libraries (for runtime machine code generation from C++ code).
- **SIMD parallel processing (Cross-Platform):** **Google Highway (`hwy`)** library. Write once in C++ and it detects the target hardware, auto-translating to x86 (AVX-512) or ARM Graviton (SVE) instructions at runtime (Auto-Vectorization).
- **Offloading:** OpenCL / CUDA, CXL 3.0-based device-mapped memory.

## 3. Layer Requirements
1. **Cache Locality Guarantee:** Never load data row-by-row (Virtual Function Call overhead). Always fetch column streams (DataBlocks) in blocks of 8,192 or more at once, process within L1/L2 cache, then pass to the next pipeline.
2. **Branch Misprediction Avoidance:** To prevent performance degradation from if/else control flow branches, use SIMD Predication with mask registers to handle all filter conditions.
3. **Zero Virtual Function Calls:** Bake the computation pipeline into a single executable binary via JIT compilation, eliminating runtime performance degradation.

## 4. Detailed Design
- **DataBlock Pipeline:** Operators in the database pass a single DataBlock (column-unit memory fragment) to the next operator when scanning the RDB memory pool. Instead of copying data, only reference counts and position pointers are passed, achieving zero-copy.
- **LLVM JIT compilation target:** For example, when a user queries `WHERE price > 100 AND volume * 10 > 5000`, instead of evaluating each expression individually, LLVM at runtime instantly replaces this with a single optimized internal machine code C++ function compiled as `bool result = (price > 100) & ((volume * 10) > 5000);`.
- **JIT SIMD emit (v3):** `compile_simd()` generates explicit `<4 x i64>` vector IR — vector loads, vector compares, and cttz-based mask extraction — instead of relying on LLVM auto-vectorization. Main loop processes 4 elements per iteration; scalar tail handles remainder.
