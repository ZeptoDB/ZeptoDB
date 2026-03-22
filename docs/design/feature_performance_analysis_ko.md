# APEX-DB 기능 확장 성능 분석
# 각 기능 추가 시 경쟁 대비 성능 우위 가능성 평가

---

## 분석 프레임워크

각 기능을 3가지 축으로 평가:
- **구조적 우위**: APEX-DB 아키텍처가 근본적으로 유리한가?
- **성능 유지**: 기능 추가해도 핫 패스 성능이 안 떨어지는가?
- **경쟁 갭**: 기존 솔루션 대비 의미 있는 차이를 낼 수 있는가?

---

## 1. SQL 파서 + 실행

### 경쟁 구조 분석
| DB | SQL 실행 방식 | 쿼리 레이턴시 |
|---|---|---|
| ClickHouse | 벡터화 인터프리터 (Block 파이프라인) | ms~sec |
| DuckDB | 벡터화 + push-based pipeline | ms |
| PostgreSQL | Volcano 모델 (row-by-row) | ms~sec |
| **APEX-DB** | **SIMD 벡터화 + LLVM JIT** | **μs** |

### 성능 우위 가능성: ✅ 높음

**이유:**
1. ClickHouse의 Block 방식: `IColumn::filter` → 새 컬럼 생성 (immutable, 복사 발생)
   APEX-DB: BitMask → 원본 데이터 zero-copy, 복사 없음
2. ClickHouse: 벡터화 OR JIT (둘 중 하나)
   APEX-DB: **벡터화 + JIT 동시** (Highway SIMD + LLVM)
3. ClickHouse: 디스크 기반, 콜드 쿼리 시 I/O 대기
   APEX-DB: 인메모리 기본, 콜드도 mmap

**위험 요소:**
- SQL 파서 자체의 파싱 오버헤드 (수 μs)
- 복잡한 쿼리 플래닝 오버헤드
- **대응:** 간단한 쿼리는 fast-path (파서 스킵), 복잡한 것만 full planner

**결론:** SQL 추가해도 핫 패스(데이터 처리)는 기존 SIMD 엔진 그대로.
파서 오버헤드는 ms 미만으로 유지 가능. **성능 우위 유지 가능.**

---

## 2. GROUP BY 집계 엔진

### 경쟁 구조 분석
| DB | GROUP BY 방식 | 1억 행 GROUP BY |
|---|---|---|
| ClickHouse | Hash table + 2-level aggregation | ~1-3초 |
| DuckDB | 파티셔닝 기반 parallel hash agg | ~2-5초 |
| kdb+ | 벡터 q 언어 `select by` | ~0.5-2초 |

### 성능 우위 가능성: ⚠️ 중간~높음

**우리가 유리한 점:**
1. 데이터가 이미 Symbol별 파티셔닝 → GROUP BY symbol은 **O(1) 라우팅**
2. 인메모리 → 해시테이블이 L2 캐시에 들어감
3. SIMD aggregate (sum, avg, min, max) 이미 구현됨

**불리한 점:**
1. High-cardinality GROUP BY (예: user_id 수백만 종류): 해시테이블 커짐
2. ClickHouse 2-level aggregation이 매우 잘 최적화되어 있음
3. DuckDB의 파티셔닝 기반 병렬 처리도 강력

**대응 전략:**
- 금융 데이터 특성: cardinality 낮음 (symbol 수천 개) → 해시테이블 작음 → **무조건 유리**
- 범용 OLAP: 높은 cardinality → 2-level aggregation 필요
- 우리 파티션 구조가 자연스럽게 1차 파티셔닝 역할

**결론:** 금융/IoT (low cardinality) → **확실한 우위**
범용 OLAP (high cardinality) → **동등~약간 우위** (인메모리 이점)

---

## 3. HTTP API (REST/Wire Protocol)

### 성능 영향 분석

```
현재:   Python → pybind11 → C++ 엔진 (in-process, ~0 오버헤드)
HTTP:   Client → HTTP → JSON/Binary 파싱 → C++ 엔진 → 직렬화 → 응답
```

### 성능 우위 가능성: ✅ 유지 가능

**HTTP 오버헤드:**
- TCP 연결: ~50-100μs (keep-alive로 1회만)
- JSON 파싱: ~10-50μs (작은 쿼리)
- 결과 직렬화: 데이터 양에 비례

**하지만:**
- ClickHouse HTTP API도 동일한 오버헤드를 가짐
- 핵심 차이는 **엔진 실행 시간**: CH ms vs APEX μs
- HTTP 오버헤드 50μs + 엔진 50μs = 100μs 총 레이턴시
- CH HTTP 오버헤드 50μs + 엔진 500ms = 500ms 총 레이턴시

**결론:** HTTP 추가해도 ClickHouse 대비 **100~1000x 빠름 유지**

**추가로:** Native binary protocol (ClickHouse wire format) 지원하면
JSON 오버헤드도 제거 가능 → Grafana가 네이티브로 연결

---

## 4. 시간 범위 인덱스 (Time Range Index)

### 경쟁 구조 분석
| DB | 시간 인덱스 | 범위 쿼리 효율 |
|---|---|---|
| ClickHouse | MergeTree 파티션 pruning | 좋음 (파티션 스킵) |
| TimescaleDB | hypertable chunk pruning | 좋음 |
| QuestDB | 시간 순서 보장 + 바이너리 서치 | 매우 좋음 |
| kdb+ | 파티션(날짜) + 정렬 가정 | 매우 좋음 |

### 성능 우위 가능성: ✅ 높음

**이유:**
1. 우리 데이터는 **시간 순서로 append-only** → 바이너리 서치 O(log n)
2. 파티션이 이미 **Hour 단위** → 시간 범위 = 파티션 pruning
3. 인메모리 → 바이너리 서치가 ns 단위

**구현 비용:** 낮음 — 이미 Timestamp 컬럼이 정렬되어 있으므로
`std::lower_bound` + `std::upper_bound`면 끝

**결론:** 거의 공짜로 추가 가능, 성능 우위 유지

---

## 5. JOIN 연산

### 경쟁 구조 분석
| DB | JOIN 방식 | 성능 |
|---|---|---|
| ClickHouse | Hash join (메모리에 우측 테이블 전체 로드) | 중간 |
| DuckDB | Hash join + sort-merge | 좋음 |
| kdb+ | aj (asof join — 시계열 특화) | 매우 좋음 |

### 성능 우위 가능성: ⚠️ 중간

**유리:**
- `asof join` (시계열 특화): 시간 기준 가장 가까운 행 매칭 → 정렬된 데이터에서 O(n)
- 우리 데이터 이미 시간순 → asof join 자연스러움
- 인메모리 → 해시테이블 빌드 빠름

**불리:**
- 범용 JOIN (arbitrary key): ClickHouse, DuckDB가 수십 년 최적화
- 우리가 처음부터 만들면 초기 품질 열세

**전략:**
- **시계열 JOIN (asof, LATEST BY)만 우선** 구현 → kdb+ aj 수준 목표
- 범용 JOIN은 후순위 (또는 DuckDB 임베딩으로 위임)

**결론:** 시계열 JOIN → **우위 가능**. 범용 JOIN → **단기 열세, 장기 동등**

---

## 6. 레플리케이션 (고가용성)

### 성능 영향 분석

**동기 복제:** 매 write마다 N개 노드 확인 → 레이턴시 증가
**비동기 복제:** write는 빠르지만 데이터 유실 가능

### 성능 우위 가능성: ✅ 유지 가능 (비동기)

**전략:**
- HFT 모드: WAL 기반 비동기 복제 (write 성능 영향 없음)
- OLAP 모드: 준동기 (1개 replica 확인 후 응답)
- ClickHouse도 비동기 복제 → 동일 조건

**결론:** 아키텍처적 불이익 없음

---

## 종합 평가표

| 기능 | 성능 우위 유지? | 구현 난이도 | 시장 임팩트 | 추천 우선순위 |
|---|---|---|---|---|
| SQL 파서 | ✅ 높음 | ⭐⭐⭐ | 🔴 필수 | **1위** |
| HTTP API | ✅ 유지 | ⭐⭐ | 🔴 필수 | **2위** |
| 시간 범위 인덱스 | ✅ 높음 (공짜) | ⭐ | 🟠 높음 | **3위** |
| GROUP BY | ⚠️ 중~높 | ⭐⭐⭐ | 🟠 높음 | **4위** |
| 시계열 JOIN (asof) | ⚠️ 중간 | ⭐⭐ | 🟡 중간 | **5위** |
| 범용 JOIN | ⚠️ 중간 | ⭐⭐⭐⭐ | 🟡 중간 | **6위** |
| 레플리케이션 | ✅ 유지 | ⭐⭐⭐ | 🟡 추후 | **7위** |

---

## 핵심 결론

### 추가해도 확실히 빠른 것 (구조적 우위)
1. **SQL + 벡터화**: ClickHouse Block 복사 vs 우리 BitMask zero-copy
2. **HTTP API**: 오버헤드 동일, 엔진 차이가 지배적
3. **시간 인덱스**: 이미 정렬된 데이터, 추가 비용 ~0

### 조건부로 빠른 것 (도메인 의존)
4. **GROUP BY**: low cardinality(금융) → 확실한 우위 / high cardinality → 동등
5. **시계열 JOIN**: 금융 asof join → 우위 / 범용 → 불리

### 성능과 무관한 것 (필요하지만 차이 없음)
6. **레플리케이션**: 모든 DB가 동일한 trade-off

---

## 전략적 추천

**Phase 1 (바로 시작):** SQL 파서 + HTTP API + 시간 인덱스
→ ClickHouse 사용자가 바로 전환 가능, 성능 우위 확실

**Phase 2:** GROUP BY + asof JOIN
→ 분석 쿼리 완성도 높임

**Phase 3:** 범용 JOIN + 레플리케이션
→ 엔터프라이즈 기능
