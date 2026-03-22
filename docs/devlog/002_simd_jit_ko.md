# Devlog 002: Highway SIMD + LLVM JIT Integration

**Date:** 2026-03-21
**Phase:** B (SIMD + JIT)
**Author:** APEX-DB 개발팀

---

## 목표
Phase E의 scalar 구현을 Highway SIMD와 LLVM JIT으로 교체하여 벡터 연산 성능 향상.

## 구현 결정사항

### Highway SIMD (v1.2.0)

**Multi-target dispatch 패턴 사용:**
- `foreach_target.h` + `HWY_BEFORE_NAMESPACE` / `HWY_AFTER_NAMESPACE` 매크로
- SSE4, AVX2, AVX-512 등 여러 타겟으로 자동 재컴파일
- 런타임에 `HWY_DYNAMIC_DISPATCH`로 CPU 최적 타겟 선택
- 현재 서버: AVX-512 (ice-lake-server급, AVX512F/BW/DQ/VL/VNNI)

**구현된 SIMD 연산:**

1. **`sum_i64`**: 4개 accumulator로 언롤 + `ReduceSum`. 독립적인 accumulator가 파이프라인 의존성을 제거하여 ILP 극대화.

2. **`filter_gt_i64`**: `Gt()` → mask → `StoreMaskBits()` → `__builtin_ctzll()` 비트 순회. 기존 scalar `if` 분기 대신 mask-based predication으로 branch misprediction 제거.

3. **`vwap`**: `ConvertTo(i64→f64)` + `MulAdd(price, volume, acc)` 파이프라인. 2개 accumulator pair로 언롤. 기존 `__int128` scalar 대비 f64 FMA가 throughput 유리.

**SelectionVector 변경:**
- `set_size(n)` 메서드 추가 — SIMD filter가 indices 배열에 직접 기록 후 크기만 설정

### LLVM JIT Engine (OrcJIT v2)

**아키텍처:**
```
Expression String → Parser (재귀하강) → AST → LLVM IR → OrcJIT → Native Code
```

**파서 문법:**
```
expr    := or_expr
or_expr := and_expr ('OR' and_expr)*
and_expr := compare ('AND' compare)*
compare := COLUMN ['*' INT] CMP_OP INT
COLUMN  := 'price' | 'volume'
CMP_OP  := '>' | '>=' | '<' | '<=' | '==' | '!='
```

**설계 결정:**
- 외부 파서 라이브러리 불사용 (Flex/Bison 등) — 단순한 표현식에 과도
- `LLJIT` (OrcJIT v2) 사용: `ExecutionEngine` 대비 더 현대적, thread-safe
- 각 compile()마다 유니크 함수명 (`apex_filter_0`, `apex_filter_1`, ...) 생성
- 최적화 패스 미적용 (향후 `PassBuilder`로 추가 가능)
- 반환 타입: `bool (*)(int64_t price, int64_t volume)` 함수 포인터 — virtual call 불필요

### 빌드 이슈

**libstdc++ 버전 충돌 해결:**
- LLVM 19 .so가 `GLIBCXX_3.4.30` 필요 (libstdc++ 6.0.30+)
- Clang 19가 GCC 11 toolchain의 libstdc++ 6.0.29를 기본으로 사용
- 해결: GCC 11의 libstdc++.so symlink를 시스템 libstdc++ 6.0.33으로 교체
- LLVM을 shared library (`libLLVM-19.so`)로 링크 (static .a는 같은 문제 발생)

## 벤치마크 결과

### SIMD (100K rows, L1/L2 cache-hot)
| Operation | Scalar | SIMD | Speedup |
|-----------|--------|------|---------|
| sum_i64 | 25μs | 6μs | **4.2x** |
| filter_gt | 307μs | 117μs | **2.6x** |
| vwap | 51μs | 20μs | **2.5x** |

### SIMD (10M rows, memory-bound)
| Operation | Scalar | SIMD | Speedup |
|-----------|--------|------|---------|
| sum_i64 | 3065μs | 2656μs | 1.2x |
| filter_gt | 32457μs | 13951μs | **2.3x** |
| vwap | 9811μs | 5587μs | **1.8x** |

### JIT (10M rows)
- Compile time: ~2.6ms
- JIT vs C++ function pointer: 0.41x (JIT 2.4x 느림)
- JIT 정확성: ✅ C++ lambda와 결과 일치

### 분석
- **캐시 내 데이터 (100K, ~800KB)**: SIMD 효과 극대화 (2.5-4.2x). 이는 DataBlock 파이프라인 (8192 rows/block) 시나리오에 정확히 해당.
- **메모리 바운드 (10M, ~80MB)**: 순수 읽기(sum)은 bandwidth 제한, 연산 집중(filter/vwap)은 여전히 1.8-2.3x 개선.
- **JIT**: 현재 최적화 패스 없이도 정확한 코드 생성. `-O2` 급 PassBuilder 추가 시 C++ 동등 성능 기대.

## 다음 단계 (Phase C/D)

1. **JIT 최적화**: PassBuilder로 InstCombine + SROA + GVN 등 적용 → C++ 동등 성능 달성
2. **SIMD filter → gather sum 파이프라인**: filter 결과를 sum에 직접 연결하는 fused operator
3. **DataBlock 통합**: 8192-row block 단위 파이프라인에서 SIMD 실행 (캐시 상주 크기)
4. **JIT 표현식 캐싱**: LRU 캐시로 자주 사용하는 필터의 재컴파일 방지
5. **Columnar batch JIT**: 행 단위 함수 호출 → 벡터 단위 IR 생성 (SIMD JIT)
