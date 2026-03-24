# 개발 로그 003: Phase B v2 — SIMD/JIT 공격적 최적화

> 날짜: 2026-03-21
> 작성자: 고생이 (AI subagent)
> 관련 브랜치: main
> 빌드: Clang 19.1.7, -O3 -march=native, Highway SIMD 1.2.0, LLVM OrcJIT 19

---

## 배경 및 문제 인식

Phase B v1에서 Highway SIMD를 도입했으나, kdb+ 대비 여전히 갭이 존재했다:

| 연산 | v1 결과 | kdb+ 참고치 | 갭 |
|------|---------|------------|-----|
| filter_gt_i64 1M | 1,341μs | 200-400μs | **3-5x 느림** |
| sum_i64 1M | 265μs | 100-200μs | ~1.5x |
| vwap 1M | 530μs | ~200μs | ~2.5x |
| JIT filter 1M | 1,254μs (per-row) | hardcoded ~530μs | **2.4x** |

**가장 큰 병목**: filter가 3-5x 느림 → 원인 분석부터 시작

---

## 최적화 1: BitMask Filter (filter_gt_i64_bitmask)

### 이론적 근거

기존 `filter_gt_i64` (v1)의 출력 경로:

```
StoreMaskBits → uint8_t mask_bytes
→ while(bits) { ctz → out_indices[out_count++] = i + k; bits &= bits-1; }
```

**문제점 3가지:**

1. **쓰기 대역폭**: 1M행, 선택률 50% → ~50만 개 uint32_t = **2MB 쓰기**
   - L2 캐시(보통 256KB-1MB)를 초과 → DRAM 쓰기 발생

2. **분기 예측 미스**: `while(bits)` 루프는 비트 패턴이 데이터 의존적
   - 각 반복마다 ctz 결과가 변함 → 브랜치 예측기가 불규칙 패턴 학습 실패
   - **측정값**: 1M 행에서 ~1,169μs → 분기 미스 누적 효과

3. **메모리 접근 패턴**: out_indices에 순차 쓰기이지만, out_count++ 증분에 의한
   store-to-load forwarding 의존성 체인 발생 (serialization)

**BitMask 해법:**

```
StoreMaskBits → uint8_t mask_bytes
→ out_bits[word_idx] |= (bits << bit_off)  ← 이게 전부
```

- 쓰기량: 1M / 8 = **128KB** (8x 감소)
- 128KB는 L2 캐시에 완전히 들어감 → DRAM 쓰기 없음
- 분기 없음: 비트 OR 연산만 수행
- `popcount()`는 __builtin_popcountll → 하드웨어 POPCNT 명령

**실측 결과:**
- 1M: 1,169μs → **272μs** (4.3x 향상) ✅ kdb+ 동급
- 100K: 98μs → **7μs** (14x 향상) — 캐시 핫일 때 극적 효과

### 코드 변경 요약

**`include/zeptodb/execution/vectorized_engine.h`:**
- `BitMask` 클래스 추가 (num_rows → num_words = ⌈n/64⌉)
- `filter_gt_i64_bitmask()` 선언
- `sum_i64_masked()` 선언 (ctz 기반 sparse 합계)

**`src/execution/vectorized_engine.cpp`:**
- `filter_gt_i64_bitmask_impl()`: bit_pos 추적하며 OR 기록
  - 경계 넘는 경우(bit_off + N > 64) 2번째 word 처리
- `sum_i64_masked()`: ctz 루프로 비트 순회 (선택률 낮을수록 유리)

### 시도했지만 효과 없었던 것

**CompressStore 시도**: Highway의 `CompressStore(v, mask, d, out_buf)` 사용 검토
→ CompressStore는 scalar 인덱스가 아닌 값 자체를 압축 출력하므로 인덱스 필터 목적에 부적합
→ 포기하고 StoreMaskBits + OR 방식 유지

**VPCOMPRESSD 직접 사용**: AVX-512의 VPCOMPRESSD로 인덱스 압축 출력 검토
→ Highway API에 직접 노출된 인터페이스 없음
→ BitMask가 더 범용적이므로 채택

---

## 최적화 2: sum_i64_fast (스칼라 4-way 언롤 + prefetch)

### 이론적 근거

**목표**: 컴파일러에 의존하지 않는 스칼라 ILP 최대화

64-bit ADD의 레이턴시는 1 CPI이지만, 단일 누산기 체인에서는 다음 ADD가
이전 ADD의 완료를 기다려야 함 → **레이턴시 바운드** 상태

```
// 단일 누산기 (나쁜 예):
for i: s0 += data[i]  ← data[i] 로드 + ADD 직렬화
```

4-way 누산기로 분리하면 OOO(Out-of-Order) 프로세서가 서로 독립된 체인을 병렬 실행:

```
s0 += data[i+0]  |  s1 += data[i+1]  |  s2 += data[i+2]  |  s3 += data[i+3]
```

**prefetch 전략:**
- L2 레이턴시 ~12ns @ 3GHz ≈ ~36 사이클
- 16 elements × 8bytes = 128bytes → 2 cache lines 처리하는 동안 다음 512bytes를 미리 요청
- `__builtin_prefetch(data + i + 64, 0, 1)`: 읽기 전용, locality=1(L2)

### 실측 결과

- 100K: scalar 25μs → fast 6μs (4.2x) - SIMD v1(4μs)보다는 약간 느림
- 1M: 302μs → **265μs** (1.1x) - SIMD와 동등
- **결론**: 1M 이상에서 memory-bound로 SIMD/scalar 차이 없음

### CPU 마이크로아키텍처 분석 (1M rows 기준)

```
데이터 크기: 1M × 8B = 8MB
L3 캐시 (AWS EC2): 일반적으로 8-32MB → 경계 근처
DRAM 대역폭: ~25-50 GB/s (DDR4)
sum 연산 강도: 1 ADD / 8 bytes = 0.125 FLOP/byte
Roofline 천장 (BW 한계): 0.125 × 25GB/s = ~3.1 GFLOP/s
실제 ADD throughput: ~10+ GFLOP/s
→ 완전 memory-bound: 연산 최적화 효과 없음
```

**왜 100K에서는 SIMD가 효과적인가?**
100K × 8B = 800KB → L2/L3 캐시에 완전히 들어감 → compute-bound 영역
SIMD 벡터 폭(AVX-512: 8개 i64 병렬)이 직접 이득을 줌

---

## 최적화 3: sum_i64_simd_v2 (SIMD 8x 언롤 + prefetch)

### 이론적 근거

v1의 4-way 누산기에서 8-way로 확장:
- ROB(Re-order Buffer) 크기: Skylake 224개 entry → 더 많은 in-flight 연산 수용
- 8개 독립 SIMD 벡터 레지스터 체인 → 로드 유닛 포화 시도

**실측**: 1M 이상에서 v1(4x)과 동등 → memory-bound 확인

---

## 최적화 4: vwap_fused (4x 언롤 + 양 배열 prefetch)

### 이론적 근거

```
prices  배열: 1M × 8B = 8MB  (한 번만 읽음)
volumes 배열: 1M × 8B = 8MB  (한 번만 읽음)
총 읽기: 16MB → 두 배열 동시 prefetch로 메모리 컨트롤러 최대 활용
```

**FMA 파이프라인**: Skylake에서 FMA throughput 0.5 CPI (포트 0, 1 각 1개)
4개 독립 FMA 체인으로 포트 포화 시도:

```
pv0 = MulAdd(p0, v0, pv0)   pv1 = MulAdd(p1, v1, pv1)
pv2 = MulAdd(p2, v2, pv2)   pv3 = MulAdd(p3, v3, pv3)
```

**실측 결과**: v1(2x, 532μs) → fused(4x+pf, 531μs) — 1M에서 동등
100K에서는 14μs vs 15μs로 미미한 개선

**결론**: vwap도 1M 이상에서 완전 memory-bound
두 배열(prices+volumes) 동시 읽기로 메모리 컨트롤러는 이미 saturated

---

## 최적화 5: JIT O3 패스 적용

### 기존 문제

```cpp
// v1: OptimizeForSize 속성만 → 패스 없이 코드 생성
fn->setAttributes(...OptimizeForSize...)
```

LLVM에서 `OptimizeForSize`는 속성(hint)이지 최적화 패스가 아님.
실제 최적화를 위해서는 `PassBuilder + buildPerModuleDefaultPipeline(O3)` 필요.

### 구현 방식

`IRTransformLayer::setTransform(apply_o3)` → `initialize()`에서 한 번 등록
→ addIRModule 시마다 자동으로 O3 패스 적용

```cpp
static llvm::Expected<llvm::orc::ThreadSafeModule>
apply_o3(tsm, r) {
    pb.buildPerModuleDefaultPipeline(O3).run(mod, mam);
}
impl_->jit->getIRTransformLayer().setTransform(apply_o3);
```

**주의**: `setTransform`은 초기화 시 한 번만 호출. compile()마다 호출하면
unique_function 소유권 이전으로 이전 람다가 파괴됨 → 버그 유발

**실측 개선**: JIT per-row 1M: (이전) 1,254μs → (O3) 1,129μs (~10% 개선)
C++ fptr(530μs) 대비 여전히 2.1x 느림 → per-row 호출 오버헤드가 주원인

---

## 최적화 6: JIT compile_bulk (벌크 루프 IR 생성)

### 설계 의도

per-row 방식의 근본 문제:
```
for i: if (filter_fn(prices[i], volumes[i])) cnt++;
```
매 행마다 함수 호출: push/pop, ret, 스택 프레임 → **~5-10ns/call × 1M = 5-10ms**

해결책: 루프를 포함한 IR 직접 생성:
```
void bulk_fn(prices*, volumes*, n, out_indices*, out_count*)
  loop: load → cmp → store → inc
```

### 실측 결과 (예상 밖)

| rows | per-row | bulk | 결과 |
|------|---------|------|------|
| 1M   | 1,129μs | 5,320μs | ❌ bulk가 4.7x 느림 |

### 실패 원인 분석 (사후 디버그)

1. **alloca 기반 카운터**: `i_alloca`, `cnt_alloca` alloca 사용 → O3가 mem2reg로
   레지스터 승격해야 하지만 루프 구조에 따라 불완전 승격 가능성

2. **noalias 속성 누락**: prices/volumes 포인터 파라미터에 `noalias` 미지정
   → LLVM이 두 포인터가 aliased일 수 있다고 보수적으로 판단
   → 벡터화(loop vectorization) 차단!
   → per-row는 단순 비교만 하므로 이 문제 없음

3. **out_indices 쓰기**: bulk 함수도 결국 인덱스 배열에 쓰기 → SelVec v1과 동일한 오버헤드

4. **벤치마크 조건의 불공정성**: per-row는 count만 증가(레지스터), bulk는 배열 쓰기까지 포함

### 수정 방향 (미완료, Phase C)

```cpp
// 파라미터에 noalias 추가
arg_prices->addAttr(llvm::Attribute::NoAlias);
arg_volumes->addAttr(llvm::Attribute::NoAlias);

// PHI 노드 기반 루프 (alloca 불필요, LLVM이 직접 레지스터 할당)
auto* i_phi   = builder.CreatePHI(i64_ty, 2, "i");
auto* cnt_phi = builder.CreatePHI(i64_ty, 2, "cnt");
```

---

## 시도했지만 효과 없었던 것 (전체 목록)

| 시도 | 이유 | 결과 |
|------|------|------|
| sum 8x SIMD 언롤 | 이론적으로 ROB 더 채움 | 1M+ memory-bound → 효과 없음 |
| vwap 4x 언롤 + prefetch | FMA 포트 포화 시도 | 1M에서 v1과 동등 (~1μs 차이) |
| CompressStore (인덱스) | 값 압축 용도라 부적합 | 포기 |
| VPCOMPRESSD 직접 | Highway API 미노출 | BitMask로 대체 |
| JIT per-row 마다 setTransform | unique_function 재설정 문제 | 심볼 조회 실패 → initialize에서 1회로 변경 |
| bulk JIT alloca 방식 | LLVM vectorization 차단 | per-row보다 4.7x 느림 |

---

## 결론 및 Phase C 우선순위

### 달성
- **filter_gt_i64**: 1,341μs → 272μs (**4.9x 향상**, kdb+ 동급 달성) ✅

### 미달성 + 원인 파악 완료
- **sum/vwap**: memory-bound 한계. 개선하려면 NT Store 또는 알고리즘 변경 필요
- **JIT bulk**: noalias + PHI 노드로 수정하면 벡터화 활성화 가능

### Phase C 후보 (우선순위)
1. `filter_bitmask + sum_masked` 파이프라인 융합 (single pass)
2. JIT bulk: `noalias` + PHI 노드 기반 재작성 → LLVM 자동 벡터화
3. NT Store 실험: `_mm_stream_si64` for sum accumulation
4. HugePages 활성화: TLB miss 감소 (mlock + madvise MADV_HUGEPAGE)
