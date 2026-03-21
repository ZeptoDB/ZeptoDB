# APEX-DB Phase B Benchmark — SIMD + JIT
# 실행일: 2026-03-21 KST
# 환경: Intel Xeon 6975P-C, 8 vCPU, 30GB RAM, Clang 19 Release -O3 -march=native

---

## Part 1: SIMD vs Scalar

### 100K rows
| 연산 | Scalar | SIMD | 속도향상 |
|---|---|---|---|
| sum_i64 | 8μs | 10μs | 0.8x ⚠️ |
| filter_gt_i64 | 343μs | 111μs | **3.1x** ✅ |
| vwap | 53μs | 17μs | **3.1x** ✅ |

### 1M rows
| 연산 | Scalar | SIMD | 속도향상 |
|---|---|---|---|
| sum_i64 | 267μs | 264μs | 1.0x — |
| filter_gt_i64 | 3,550μs | 1,358μs | **2.6x** ✅ |
| vwap | 649μs | 532μs | 1.2x |

### 10M rows
| 연산 | Scalar | SIMD | 속도향상 |
|---|---|---|---|
| sum_i64 | 2,655μs | 2,654μs | 1.0x — |
| filter_gt_i64 | 35,355μs | 13,438μs | **2.6x** ✅ |
| vwap | 10,977μs | 5,519μs | **2.0x** ✅ |

---

## Part 2: LLVM JIT 컴파일 필터

**Expression:** `price > 100000 AND volume > 5000`
**컴파일 시간:** 2,686μs (최초 1회)

| rows | JIT 실행 | C++ Lambda | 비율 |
|---|---|---|---|
| 100K | 337μs | 15μs | 0.04x ❌ |
| 1M | 3,430μs | 531μs | 0.15x ❌ |
| 10M | 35,508μs | 5,890μs | 0.17x ❌ |

---

## 분석

### ✅ SIMD 성과
- **filter_gt_i64**: 2.6~3.1x 향상 — 마스크 기반 SIMD predication 효과
- **vwap**: 2x~3x 향상 — 벡터 MulAdd 효과
- **sum_i64**: 개선 없음 — O3 컴파일러가 이미 scalar sum을 auto-vectorize하고 있음

### ⚠️ 개선 필요한 것

**sum_i64 SIMD 효과 없음:**
- 원인: `-O3 -march=native` 컴파일러가 scalar 코드를 이미 AVX2 auto-vectorize
- 실제 sum은 메모리 대역폭 bound — SIMD 이득 제한적
- 해결책: 멀티컬럼 동시 처리(fusion)로 메모리 접근 최소화

**JIT 실행이 C++ lambda보다 느림:**
- 원인: 현재 JIT가 scalar 코드를 emit하고 있음 — SIMD 명령어 미주입
- 또한 JIT 바이너리가 `-march=native` 최적화를 받지 못함
- 해결책:
  1. JIT 코드에 AVX2/AVX-512 intrinsic 직접 emit
  2. `LLVM::TargetMachine::setOptLevel(CodeGenOpt::Aggressive)` 적용
  3. JIT 함수를 SIMD 레인에서 처리하는 벡터 버전으로 재설계

### kdb+ 비교 업데이트

| 메트릭 | kdb+ (q) | APEX-DB Scalar | APEX-DB SIMD | 상태 |
|---|---|---|---|---|
| VWAP 1M | ~500-800μs | 649μs | **532μs** | ✅ |
| Filter 1M | ~200-400μs | 3,550μs | 1,358μs | ⚠️ 개선 필요 |
| sum 1M | ~100-200μs | 267μs | 264μs | ⚠️ 개선 필요 |

> **filter 성능이 kdb+ 대비 열세** — 이유:
> 1. kdb+ q 언어는 컬럼형 벡터 연산에 최적화된 인터프리터
> 2. 우리 filter 구현이 SelectionVector 기록 오버헤드 있음
> 3. 다음 단계: predicate pushdown + bitmasking 방식으로 재구현

---

## 다음 단계 (Phase B 잔여)
1. **JIT SIMD emit** — 컴파일된 필터가 AVX2로 실행되도록
2. **Filter 재구현** — bitmask 기반으로 SelectionVector 오버헤드 제거
3. **sum 멀티컬럼 fusion** — price+volume 동시 처리로 메모리 접근 반감
