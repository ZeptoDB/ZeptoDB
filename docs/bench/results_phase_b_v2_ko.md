# Phase B v2 — 최적화 결과 벤치마크
# 실행일: 2026-03-22 KST
# 환경: Intel Xeon 6975P-C, 8 vCPU, 30GB RAM, Clang 19 Release -O3 -march=native

---

## 핵심 성과 요약

| 연산 | v1 (SIMD) | v2 (최적화) | 최종 speedup |
|---|---|---|---|
| filter_gt_i64 1M | 1,350μs | **272μs** | **4.96x 향상** ✅ |
| filter_gt_i64 100K | 114μs | **10μs** | **11.4x 향상** 🚀 |
| sum_i64 1M | 265μs | 264μs | 1.0x (이미 최적) |
| vwap 100K | 19μs | **17μs** | 2.8x (scalar 대비) |
| JIT filter 1M | 3,430μs → **1,292μs** (O3 적용) | 개선됨 | **2.6x 향상** ✅ |

---

## Part 1: SIMD 비교 (v1 기준)

### 100K rows
| 연산 | Scalar | SIMD v1 | speedup |
|---|---|---|---|
| sum_i64 | 25μs | **6μs** | **4.2x** |
| filter_gt_i64 | 330μs | 117μs | 2.8x |
| vwap | 48μs | 20μs | 2.4x |

### 1M rows
| 연산 | Scalar | SIMD v1 | speedup |
|---|---|---|---|
| sum_i64 | 310μs | 265μs | 1.2x |
| filter_gt_i64 | 3,262μs | 1,360μs | 2.4x |
| vwap | 587μs | 529μs | 1.1x |

### 10M rows
| 연산 | Scalar | SIMD v1 | speedup |
|---|---|---|---|
| sum_i64 | 3,034μs | 2,653μs | 1.1x |
| filter_gt_i64 | 32,791μs | 13,566μs | 2.4x |
| vwap | 9,714μs | 5,561μs | 1.7x |

---

## Part 2: BitMask Filter 최적화 (가장 큰 성과)

**전략:** SelectionVector (인덱스 배열 기록) → BitMask (비트 압축)

| rows | v1 SelectionVector | v2 BitMask | speedup |
|---|---|---|---|
| 100K | 114μs | **10μs** | **11.4x** 🚀 |
| 1M | 1,350μs | **272μs** | **4.96x** ✅ |
| 10M | 13,631μs | **2,767μs** | **4.93x** ✅ |

**분석:**
- SelectionVector는 통과 인덱스를 개별 write → 캐시 미스 + 분기 예측 실패
- BitMask는 64행을 uint64 1개로 압축 → 캐시 효율 64배, 쓰기 1/64
- 100K에서 11x: 데이터가 L2 캐시 안에 들어올 때 효과 극대화

**kdb+ 비교 업데이트:**
- filter 1M: 1,350μs → **272μs** (kdb+ ~100-300μs 범위로 진입) ✅

---

## Part 3: sum_i64 최적화 결과

**전략 비교:** Scalar → SIMD v1 (4x unroll) → fast (4-way accumulator) → SIMD v2 (8x + prefetch)

| rows | Scalar | SIMD v1 | fast | SIMD v2 |
|---|---|---|---|---|
| 100K | 25μs | **6μs (4.2x)** | 6μs (4.2x) | 7μs (3.6x) |
| 1M | 296μs | 269μs (1.1x) | 264μs (1.1x) | 267μs (1.1x) |
| 10M | 3,028μs | 2,656μs (1.1x) | 2,658μs (1.1x) | 2,655μs (1.1x) |

**분석:**
- 100K (L2 캐시 범위): 4.2x — SIMD 효과 확실
- 1M/10M: 1.1x — **메모리 대역폭 bound** (모든 방법이 동일 속도)
- 결론: sum은 더 이상 최적화 어려움. 메모리 시스템이 병목

---

## Part 4: VWAP fused 최적화

**전략:** 2-pass (price 스캔, volume 스캔) → 1-pass fused + 4x unroll + prefetch

| rows | Scalar | SIMD v1 | fused(4x+pf) | 비고 |
|---|---|---|---|---|
| 100K | 48μs | 19μs (2.5x) | **17μs (2.8x)** | L2 캐시 내 효과 |
| 1M | 570μs | 530μs (1.1x) | 532μs (1.1x) | 대역폭 bound |
| 10M | 9,153μs | 5,568μs (1.6x) | 5,544μs (1.7x) | SIMD 효과 유지 |

---

## Part 5: JIT 최적화 (O3 적용)

| rows | per-row O3 | bulk O3 | C++ lambda | 비고 |
|---|---|---|---|---|
| 100K | 112μs | 511μs | 13μs | bulk 역효과 |
| 1M | **1,292μs** | 5,317μs | 532μs | per-row O3: 2.6x 향상 |

**분석:**
- per-row O3: 이전 3,430μs → 1,292μs (**2.6x 향상**) ✅
- bulk IR은 역효과 — LLVM이 IR 루프를 최적화 못함
- C++ lambda는 여전히 2.4x 빠름 — JIT에 SIMD emit 필요 (Phase B 잔여)

---

## kdb+ vs APEX-DB 최종 비교 (Phase B v2 기준)

| 연산 | kdb+ 참고치 | APEX-DB v2 | 상태 |
|---|---|---|---|
| 인제스션 | ~2-5M/sec | **5.52M/sec** | ✅ 우위 |
| VWAP 1M | ~200-500μs | **532μs** | ⚠️ 근접 |
| filter 1M (bitmask) | ~100-300μs | **272μs** | ✅ 범위 진입 |
| sum 1M | ~50-150μs | **264μs** | ⚠️ 2x 열세 (대역폭 bound) |
| JIT filter | q 인터프리터 | 1,292μs | ⚠️ 개선 진행 중 |

---

## 결론

**✅ Phase B v2 핵심 성과:**
1. **BitMask filter: 최대 11x 향상** — kdb+ 범위 진입
2. **JIT O3: 2.6x 향상** — 아직 C++ 대비 느리지만 방향성 확인
3. **sum/vwap: 대역폭 bound** — 하드웨어 수준의 한계, 더 최적화 어려움

**다음 단계 (Phase B 잔여):**
1. JIT에 SIMD vector IR emit → C++ lambda 수준 목표
2. sum 멀티컬럼 fusion (price+volume 동시 처리)
3. filter+aggregate 파이프라인 fusion (filter → sum 1-pass)
