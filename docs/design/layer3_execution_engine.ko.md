# Layer 3: Vectorized Query Execution Engine & JIT

본 문서는 인메모리에 로드된 방대한 틱 데이터를 단일 클럭 사이클 내에 최대치로 병렬 처리해 결과를 계산하는 **실행 엔진(Execution Engine)** 설계서입니다. ClickHouse의 벡터 연력과 JIT 기술을 결합했습니다.

## 1. 아키텍처 다이어그램 (Architecture Diagram)

```mermaid
flowchart TD
    SQL_AST["Logical Query Plan\n(Python AST / Query)"] --> Optimizer["C++ Query Optimizer\n(파이프라인 최소화)"]

    Optimizer --> OperatorTree["Physical Plan\n(DAG Operators)"]

    OperatorTree --> JIT_Path{"단순 식인가요?"}
    JIT_Path -- "No: 복잡한 조건(수학연산 등)" --> LLVM["LLVM JIT Compiler\n기계어로 런타임 컴파일 적용"]
    JIT_Path -- "Yes: 단순 필터링/합계" --> VectorPath["Pre-compiled\nVectorized Execution"]

    subgraph DataFlow["Data Flow"]
        vector_chunk["RDB DataBlock\n(예: 8192 rows 블록)"]
    end

    LLVM --> Engine[Hardware Intrinsics Core\nSIMD (AVX-512 / ARM SVE)]
    VectorPath --> Engine
    vector_chunk --> Engine

    Engine --> Output["최종 집계 결과"]
    Engine -. "초고강도 연산 분배" .-> FPGA["CXL 기반 FPGA/GPU\n(Options Greeks 등)"]
```

## 2. 사용될 기술 스택 (Tech Stack)
- **JIT 프레임워크:** LLVM / MLIR 라이브러리 (C++ 코드 상에서 런타임 기계어 생성용).
- **SIMD 병렬 처리 (Cross-Platform):** **Google Highway (`hwy`)** 라이브러리. 단일 C++ 코드로 작성하면 타겟 하드웨어를 감지하여 x86(AVX-512) 또는 ARM Graviton(SVE) 명령어로 런타임에 자동번역(Auto-Vectorization)하는 최신 프레임워크.
- **오프로딩:** OpenCL / CUDA, CXL 3.0 기반 디바이스 매핑 메모리.

## 3. 핵심 요구사항 (Layer Requirements)
1. **Cache Locality 보장:** 절대 데이터 로드를 한 행(Row)씩 가져와서 처리(Virtual Function Call 오버헤드)하지 마십시오. 무조건 8,192 단위 이상의 블록 단위 컬럼 스트림(DataBlock)을 한 번에 가져와 L1/L2 캐시 내에서 처리 후 다음 파이프라인으로 넘겨야 합니다.
2. **분기(Branch) 예측 실패 회피:** if/else 제어 흐름 분기로 나타나는 성능 저하를 막기 위해, 마스크(Mask) 레지스터 기반의 SIMD Predication 기법으로 모든 필터 조건을 처리할 것.
3. **가상 함수 호출(Virtual Func Call) 제로화:** 연산 파이프라인을 JIT 컴파일 등을 이용해 단일 실행 바이너리로 구워버려 런타임 성능 저하 요소를 없앤다.

## 4. 구체적 설계 (Detailed Design)
- **DataBlock Pipeline:** 데이터베이스 내의 연산자(Operator)는 RDB 메모리 풀을 스캔할 때 하나의 DataBlock(컬럼 단위 메모리 조각)을 다음 연산자로 패스합니다. 데이터를 복사하는 게 아니라, 참조 카운트와 위치 포인터만을 포함해 넘겨 무복사를 실현합니다.
- **LLVM JIT 컴파일 적용 부위:** 예를 들어 사용자가 `WHERE price > 100 AND volume * 10 > 5000`을 질의했을 때, 이 표현식을 개별적으로 평가하는 게 아니라. 런타임에 LLVM을 통해 `bool result = (price > 100) & ((volume * 10) > 5000);` 컴파일된 한 개의 최적화된 내부 기계어 C++ 함수로 즉석 치환해버립니다.
