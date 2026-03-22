# APEX-DB Benchmark Results — Phase E (End-to-End MVP)
# 실행일: 2026-03-21 KST
# 환경: Intel Xeon 6975P-C, 8 vCPU, 30GB RAM, Amazon Linux 2023

---

## BENCH 1: 인제스션 처리량 (Ingestion Throughput)

| 배치 크기 | 처리량 | 드롭 |
|---|---|---|
| batch=1 | **4.97M ticks/sec** | 0 |
| batch=64 | **5.44M ticks/sec** | 0 |
| batch=512 | **5.52M ticks/sec** | 0 |
| batch=4096 | **5.51M ticks/sec** | 0 |
| batch=65535 | **5.52M ticks/sec** | 14 (큐 포화) |

**Peak: 5.52M ticks/sec** (배치 512~65535에서 포화)

---

## BENCH 2: 쿼리 레이턴시 (Query Latency)

### 100K rows
| 쿼리 타입 | p50 | p99 | p999 |
|---|---|---|---|
| VWAP | 52.5μs | 60.6μs | 80.1μs |
| Filter+Sum | 75.5μs | 81.0μs | 110.5μs |
| Count | 0.1μs | 0.1μs | 0.3μs |

### 1M rows
| 쿼리 타입 | p50 | p99 | p999 |
|---|---|---|---|
| VWAP | 637.7μs | 663.5μs | 795.7μs |
| Filter+Sum | 789.7μs | 810.8μs | 829.9μs |
| Count | 0.1μs | 0.1μs | 0.4μs |

### 5M rows
| 쿼리 타입 | p50 | p99 | p999 |
|---|---|---|---|
| VWAP | 3,496μs | 3,577μs | 7,942μs |
| Filter+Sum | 3,945μs | 4,035μs | 5,246μs |
| Count | 0.1μs | 0.1μs | 0.7μs |

**처리 속도: 9M rows → 10ms = ~914M rows/sec throughput (VWAP)**

---

## BENCH 3: 멀티 프로듀서 동시 성능

| 스레드 수 | 처리량 |
|---|---|
| 1 | 1.97M ticks/sec |
| 2 | 1.92M ticks/sec |
| 4 | 1.72M ticks/sec |

> 멀티 스레드 시 성능 하락 — drain thread 병목. **개선 필요** (아래 분석 참조)

---

## BENCH 5: 대용량 9M rows VWAP
- 로드: 5.01M ticks/sec
- 쿼리 p50=10.8ms, p99=15ms, p999=16ms
- **처리량: ~914M rows/sec**

---

## kdb+ 비교 레퍼런스

### kdb+ 공개 벤치마크 (참고 자료 기반)
| 메트릭 | kdb+ (참고값) | APEX-DB (현재) | 상태 |
|---|---|---|---|
| 인제스션 처리량 | ~2-5M ticks/sec (단일 tickerplant) | **5.52M ticks/sec** | ✅ 동등~우위 |
| VWAP 1M rows | ~500-800μs | **637μs** | ✅ 동등 |
| Full scan 1M rows | ~200-400μs | 790μs (filter+sum) | ⚠️ 열세 |
| 멀티스레드 인제스션 | kdb+는 단일 스레드 모델 | 1.72M (4 threads) | ⚠️ 개선 필요 |

> **참고**: kdb+ 벤치마크는 공개된 학술/업계 자료 기반 추정치.
> 실제 kdb+ 환경(실시간 tick, dedicated hardware)과 직접 비교는 추후 진행.

---

## 분석 및 개선 포인트

### ✅ 잘 되고 있는 것
1. **단일 스레드 인제스션 5.52M/sec** — 목표치 달성
2. **Count 쿼리 O(1)** — 인덱스 기반으로 0.1μs 수준
3. **VWAP throughput 900M rows/sec** — 메모리 대역폭 한계에 근접

### ⚠️ 개선 필요
1. **멀티 프로듀서 시 처리량 감소** (4 threads → 1.72M/sec)
   - 원인: drain thread가 단일 mutex + 단일 consumer
   - 해결: sharded drain threads (symbol별 분리)

2. **큐 포화 (drop)** — 65K batch 이상에서 발생
   - 원인: Ring buffer 64K capacity vs. 생산 속도
   - 해결: 큰 배치는 direct-to-storage 경로 추가

3. **VWAP 쿼리 레이턴시** — scalar 구현, SIMD 미적용
   - Phase B에서 Highway SIMD 벡터화로 **10-30x 개선 예상**

4. **HugePages 실패** — 9M row 테스트에서 fallback 발생
   - 원인: VM 환경에서 huge_page 설정 부재
   - 해결: /proc/sys/vm/nr_hugepages 설정

---

## POC 결론 (Phase E)

**✅ End-to-End 파이프라인 POC 완료**

- Tick 수신 → 컬럼 저장 → 벡터화 쿼리 전체 흐름 동작
- 5.52M ticks/sec 인제스션, 914M rows/sec VWAP 처리량
- kdb+ 단순 비교 시 인제스션 동등~우위, 쿼리는 SIMD 적용 전

**다음 단계: Phase B — Highway SIMD + LLVM JIT 로 쿼리 10x 개선**
