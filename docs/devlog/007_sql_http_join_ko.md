# APEX-DB Phase 007: SQL 파서 + HTTP API + JOIN 프레임워크

**날짜:** 2026-03-22  
**작업자:** 고생이  
**Phase:** 007 — SQL/HTTP/JOIN

---

## 개요

APEX-DB의 핵심 레이어(E, B, A, D, C)가 완성된 이후, 실제 클라이언트가 사용할 수 있는 인터페이스를 추가했다. 이번 Phase에서는 다음 세 가지를 구현했다:

1. **SQL 파서** — 재귀 하강 파서 (no yacc/bison)
2. **쿼리 실행기** — AST → ApexPipeline API 변환
3. **JOIN 프레임워크** — ASOF JOIN (금융 핵심) + HashJoin 스텁
4. **HTTP API 서버** — ClickHouse 호환 포트 8123

---

## Part 1: SQL 파서

### 설계 원칙
- **완전 자체 구현**: flex/bison/ANTLR 의존성 없음. 순수 C++ 재귀 하강 파서
- **실용적 서브셋**: 금융 DB에서 실제로 쓰이는 SQL만 지원
- **토크나이저 → AST → 실행기** 파이프라인

### 지원 SQL 문법

```sql
-- 기본 SELECT + WHERE
SELECT price, volume FROM trades WHERE symbol = 1 AND price > 15000

-- 집계 함수 (COUNT, SUM, AVG, MIN, MAX, VWAP)
SELECT count(*), sum(volume), avg(price) FROM trades WHERE symbol = 1
SELECT VWAP(price, volume) FROM trades  -- 금융 특화

-- GROUP BY
SELECT symbol, sum(volume) FROM trades GROUP BY symbol

-- ASOF JOIN (금융 핵심)
SELECT t.price, t.volume, q.bid, q.ask
FROM trades t
ASOF JOIN quotes q ON t.symbol = q.symbol AND t.timestamp >= q.timestamp

-- 시간 범위
SELECT * FROM trades WHERE price BETWEEN 15000 AND 16000

-- 표준 JOIN (스텁)
SELECT t.price, q.bid FROM trades t JOIN quotes q ON t.symbol = q.symbol

-- 정렬 + 제한
SELECT price FROM trades ORDER BY price DESC LIMIT 100
```

### 파싱 성능

| 쿼리 유형 | 평균 | P99 |
|-----------|------|-----|
| 단순 SELECT | 1.17μs | 1.23μs |
| 집계 쿼리 | 1.79μs | 1.85μs |
| GROUP BY | 1.39μs | 1.43μs |
| ASOF JOIN | 4.56μs | 4.63μs |
| 복잡한 JOIN | 5.06μs | 5.14μs |

→ **모든 쿼리 50μs 이하** 목표 달성 ✓

---

## Part 2: 쿼리 실행기

### APEX 스키마 특이사항

APEX-DB 파티션은 `symbol` 컬럼이 없다. `SymbolId`는 `PartitionKey.symbol_id`로 인코딩된다.  
따라서 `WHERE symbol = N` 조건은 **컬럼 레벨 평가가 아닌 파티션 레벨 필터링**으로 처리된다.

```cpp
// symbol 조건 감지 → get_partitions_for_symbol() 사용
if (has_where_symbol(stmt, sym_filter, alias)) {
    auto parts = pm.get_partitions_for_symbol(sym_filter);
    // 이 파티션들만 스캔
}
```

### GROUP BY symbol 처리

GROUP BY symbol의 경우, 파티션 키에서 `symbol_id`를 직접 읽는다:

```cpp
bool is_symbol_group = (group_col == "symbol");
int64_t gkey = is_symbol_group 
    ? static_cast<int64_t>(part->key().symbol_id)
    : gdata[idx];
```

### VWAP 집계

SQL에서 `VWAP(price, volume)`을 집계 함수로 지원:

```sql
SELECT VWAP(price, volume) FROM trades WHERE symbol = 1
```

내부적으로 `sum(price * volume) / sum(volume)` 계산.

---

## Part 3: JOIN 프레임워크

### 제네릭 인터페이스

```cpp
class JoinOperator {
public:
    virtual JoinResult execute(
        const ColumnVector& left_key,
        const ColumnVector& right_key,
        const ColumnVector* left_time  = nullptr,  // ASOF용
        const ColumnVector* right_time = nullptr
    ) = 0;
};
```

### ASOF JOIN 알고리즘

**두 포인터 + 이진 탐색** 조합으로 구현:
1. 오른쪽 테이블을 심볼별로 그룹화
2. 각 그룹 내에서 타임스탬프 기준 정렬 (안전장치)
3. 각 왼쪽 행에 대해 `upper_bound`로 O(log m) 매칭

```
왼쪽: trades [t=100, t=200, t=300]  (symbol=1)
오른쪽: quotes [t=50, t=150, t=250] (symbol=1)

ASOF 매칭:
  trade(t=100) → quote(t=50)   ← t=50 ≤ 100 중 최신
  trade(t=200) → quote(t=150)  ← t=150 ≤ 200 중 최신
  trade(t=300) → quote(t=250)  ← t=250 ≤ 300 중 최신
```

### HashJoin 스텁

```cpp
class HashJoinOperator : public JoinOperator {
public:
    JoinResult execute(...) override {
        throw std::runtime_error("HashJoinOperator: not yet implemented");
    }
};
```

향후 Phase에서 해시 테이블 빌드 + 프로브 구현 예정.

---

## Part 4: HTTP API

### ClickHouse 호환 서버

- **포트**: 8123 (ClickHouse 기본 포트)
- **라이브러리**: cpp-httplib v0.18.3 (헤더 전용)
- **Grafana/클라이언트 직접 연결 가능**

### 엔드포인트

```
POST /        — SQL 쿼리 실행 (바디: SQL 문자열)
GET  /        — SQL 쿼리 (query 파라미터)
GET  /ping    — 헬스체크 → "Ok"
GET  /stats   — 파이프라인 통계 (JSON)
```

### 응답 형식

```json
{
    "columns": ["price", "volume"],
    "data": [[15000, 100], [15010, 200]],
    "rows": 2,
    "rows_scanned": 100000,
    "execution_time_us": 52.30
}
```

---

## Part 5: 테스트 결과

```
=== 새로운 테스트 (32개) ===
Tokenizer.*      7/7  PASS
Parser.*        12/12 PASS
AsofJoin.*       4/4  PASS
SqlExecutorTest.* 9/9  PASS

=== 기존 테스트 유지 ===
C++ 유닛 테스트: 76/76 PASS (ClusterNode/TransportSwap은 기존 실패)
Python 테스트:   31/31 PASS
```

---

## Part 6: 벤치마크 결과

### SQL 파싱 속도

| 쿼리 | avg | 목표 |
|------|-----|------|
| 단순 SELECT | 1.17μs | <50μs ✓ |
| 집계 | 1.79μs | <50μs ✓ |
| ASOF JOIN 파싱 | 4.56μs | <50μs ✓ |

### SQL 실행 오버헤드 (vs 직접 C++ API)

| 작업 | SQL | 직접 C++ |
|------|-----|---------|
| VWAP (100K rows) | 112μs | 50μs |
| COUNT | 13μs | 0.12μs |
| SUM (100K rows) | 52μs | N/A |

→ SQL 오버헤드 = 파싱(~2μs) + AST 해석 + 함수 포인터 디스패치

### ASOF JOIN 성능

| 데이터 크기 | 처리 시간 |
|------------|---------|
| N=1,000 | 149μs |
| N=10,000 | 1.5ms |
| N=1,000,000 | 53ms |

---

## 아키텍처 레이어 업데이트

```
Layer 6: HTTP API (apex_server)
    ↓
Layer 5: SQL Parser + Executor (apex_sql)
    ↓
Layer 4: Transpiler (Python DSL, apex_py)
    ↓
Layer 3: Vectorized Engine (apex_execution)
    ↓
Layer 2: Ingestion (apex_ingestion)
    ↓
Layer 1: Storage (apex_storage)
```

---

## 다음 Phase 예정

- **HashJoin 풀 구현** — 해시 테이블 빌드 + 프로브 (SIMD 최적화)
- **쿼리 플래너 고도화** — JOIN 재정렬, 푸시다운 최적화
- **HTTP API 확장** — TSV 출력 모드, 비동기 쿼리
- **ASOF JOIN SIMD 최적화** — 이진 탐색의 벡터화

---

*작성: 고생이 | APEX-DB 개발 로그*
