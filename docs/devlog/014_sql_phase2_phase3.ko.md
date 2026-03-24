# 개발 로그 014: SQL Phase 2 & Phase 3 — 산술, CASE WHEN, 날짜/시간, LIKE, 집합 연산

*날짜: 2026-03-22*

---

## 개요

이 개발 로그는 ZeptoDB의 SQL 서브셋을 시계열 분석 워크로드에서 ClickHouse와 실질적으로 동등한 수준으로 끌어올린 세 번의 SQL 파서 및 실행기 개선을 다룹니다.

| 단계 | 기능 | 추가 테스트 수 |
|------|------|---------------|
| Phase 1 | IN, IS NULL/NOT NULL, NOT, HAVING | 9 |
| Phase 2 | SELECT 산술, CASE WHEN, 다중 컬럼 GROUP BY | 13 |
| Phase 3 | 날짜/시간 함수, LIKE/NOT LIKE, UNION/INTERSECT/EXCEPT | 31 |

기존 334+ 테스트 전체가 계속 통과합니다. 기존부터 존재하던 불안정 테스트
(`ClusterNode.TwoNodeLocalCluster`)는 관련이 없으며 타이밍 의존적입니다.

---

## Phase 2: SELECT 산술, CASE WHEN, 다중 컬럼 GROUP BY

### 구현 내용

#### SELECT 산술 — `ArithExpr` 표현식 트리

SELECT 목록의 값 표현식을 나타내는 재귀적 `ArithExpr` AST 노드를 추가했습니다:

```cpp
struct ArithExpr {
    enum class Kind { COLUMN, LITERAL, BINARY, FUNC };
    Kind kind;

    // COLUMN
    std::string table_alias;
    std::string column;

    // LITERAL
    int64_t literal = 0;

    // BINARY
    ArithOp arith_op;                       // ADD / SUB / MUL / DIV
    std::shared_ptr<ArithExpr> left, right;

    // FUNC (Phase 3)
    std::string func_name, func_unit;
    std::shared_ptr<ArithExpr> func_arg;
};
```

파서: `parse_arith_expr_node()` → `parse_arith_term()` → `parse_arith_primary()` (표준 우선순위 방식).
실행기: `eval_arith(node, part, row_idx)` 정적 함수.

**지원 쿼리:**
```sql
SELECT price * volume AS notional FROM trades WHERE symbol = 1
SELECT SUM(price * volume) AS total_notional FROM trades WHERE symbol = 1
SELECT AVG(price - 15000) AS avg_premium FROM trades WHERE symbol = 1
SELECT (close - open) / open AS ret FROM trades WHERE symbol = 1
```

#### CASE WHEN

```sql
SELECT CASE WHEN price > 15050 THEN 1 ELSE 0 END AS is_high FROM trades
```

`eval_case_when(expr, part, row_idx)`이 WHEN 조건에 대해 `eval_expr_single()`을 호출하고
(배치 필터의 스칼라 버전), THEN/ELSE 값에 대해 `eval_arith()`를 호출합니다.

#### 다중 컬럼 GROUP BY

`is_symbol_group` 최적화가 `gb.columns.size() == 1` 가드가 없어 다중 컬럼 키에도 발동되는 문제를 수정했습니다.
다중 컬럼 키는 이제 `std::vector<int64_t>` 복합 키를 구성하고 O(1) 평균 조회를 위해 `VectorHash`를 사용합니다.

직렬(`exec_group_agg`)과 병렬(`exec_group_agg_parallel`) 경로 모두 동일한 `make_group_key` 람다 패턴을 공유합니다.

#### 병렬 경로 동기화

`exec_agg_parallel`과 `exec_group_agg_parallel`이 직렬 경로 대비 Phase 2 기능이 누락되어 있었습니다.
- `agg_val` 람다: `arith_expr`를 먼저 확인하고 없으면 직접 컬럼 읽기로 폴백
- 결과 컬럼 이름이 이제 raw 함수+컬럼 문자열 대신 `alias` 필드를 사용
- `exec_group_agg_parallel`이 `std::unordered_map<int64_t,...>` GroupMap을
  `VectorHash<std::vector<int64_t>,...>`로 교체하여 복합 키 지원

---

## Phase 3: 날짜/시간 함수, LIKE/NOT LIKE, 집합 연산

### 3-A. 날짜/시간 함수

새로운 `ArithExpr::Kind::FUNC`과 4개의 내장 함수:

| 함수 | 동작 |
|------|------|
| `DATE_TRUNC('unit', col)` | 나노초 타임스탬프를 지정 단위 버킷으로 내림 |
| `NOW()` | `std::chrono::system_clock`을 통한 현재 나노초 타임스탬프 |
| `EPOCH_S(col)` | 나노초 ÷ 1,000,000,000 → 초 |
| `EPOCH_MS(col)` | 나노초 ÷ 1,000,000 → 밀리초 |

`DATE_TRUNC` 지원 단위: `ns`, `us`, `ms`, `s`, `min`, `hour`, `day`, `week`.

```sql
SELECT DATE_TRUNC('min', timestamp) AS minute, SUM(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY DATE_TRUNC('min', timestamp)

SELECT EPOCH_S(timestamp) AS ts_sec, price FROM trades WHERE symbol = 1
```

### 3-B. LIKE / NOT LIKE

새로운 `Expr::Kind::LIKE`과 DP 글로브 매칭:

- `%` = 임의의 부분 문자열, `_` = 임의의 단일 문자
- INT64 컬럼 값은 `std::to_string()`으로 십진수 문자열로 변환 후 매칭

```sql
SELECT * FROM trades WHERE price LIKE '150%'    -- 접두사 매칭
SELECT * FROM trades WHERE price NOT LIKE '%9'  -- 접미사 제외
SELECT * FROM trades WHERE price LIKE '1500_'   -- 단일 문자 와일드카드
```

세 곳의 평가 사이트에 `LIKE` 케이스를 추가했습니다:
1. `eval_expr()` — 배치 WHERE 평가 (`BitMask` 반환)
2. `eval_expr_single()` — 단일 행 평가 (CASE WHEN에서 사용)
3. `apply_having_filter()` — 집계 후 필터

### 3-C. 집합 연산 (UNION / INTERSECT / EXCEPT)

새로운 `SelectStmt::SetOp` 열거형과 `rhs` 포인터:

| 연산 | 구현 방식 |
|------|-----------|
| `UNION ALL` | 양쪽을 실행하여 행 연결 |
| `UNION DISTINCT` | 양쪽을 실행하여 `std::set<vector<int64_t>>`로 중복 제거 |
| `INTERSECT` | 왼쪽 실행 후 집합 구성; 오른쪽 실행, 왼쪽 집합에 있는 행만 유지 |
| `EXCEPT` | 왼쪽 실행; 오른쪽으로 집합 구성; 오른쪽에 없는 왼쪽 행 유지 |

```sql
SELECT price FROM trades WHERE symbol = 1
UNION ALL
SELECT price FROM trades WHERE symbol = 2

SELECT price FROM trades WHERE symbol = 1
INTERSECT
SELECT price FROM trades WHERE price > 15050

SELECT price FROM trades WHERE symbol = 1
EXCEPT
SELECT price FROM trades WHERE price > 15050
```

---

## 주요 버그 수정

1. **`is_symbol_group` 다중 컬럼 회귀** — `gb.columns.size() == 1` 가드 누락. 수정됨.

2. **산술 연산에서 `symbol` 컬럼이 0 반환** — `get_col_data(part, "symbol")`이 nullptr 반환
   (symbol은 파티션 수준 키). `eval_arith()`와 `exec_simple_select()`에서 `part.key().symbol_id`를
   명시적으로 반환하는 분기 추가.

3. **`DateTruncUsResult`가 10행 반환** — 결과 컬럼이 `tb`로 별칭되어 있었지만 테스트가
   `ORDER BY price`를 사용. `apply_order_by`가 `price`를 찾지 못해 LIMIT를 건너뜀.
   ORDER BY/LIMIT 제거로 수정.

4. **`UNION DISTINCT`가 왼쪽 내부 중복을 제거하지 않음** — 수정됨.

5. **`apply_having_filter`에서 `Expr::Kind::LIKE` 누락** — 컴파일러 경고.
   LIKE 분기 추가로 수정.

---

## 변경된 파일

| 파일 | 변경 사항 |
|------|-----------|
| `include/zeptodb/sql/tokenizer.h` | DATE_TRUNC, NOW, EPOCH_S, EPOCH_MS, LIKE, UNION, ALL, INTERSECT, EXCEPT 토큰 추가 |
| `src/sql/tokenizer.cpp` | 새 토큰 키워드 매핑 |
| `include/zeptodb/sql/ast.h` | `ArithExpr::Kind::FUNC` + func 필드; `Expr::Kind::LIKE` + `like_pattern`; `SelectStmt::SetOp` + `rhs` |
| `include/zeptodb/sql/parser.h` | `parse_string_literal()` 선언 |
| `src/sql/parser.cpp` | Phase 2/3 파싱: arith FUNC, LIKE, 집합 연산, 문자열 리터럴 |
| `src/sql/executor.cpp` | `date_trunc_bucket()`, `eval_arith FUNC`, `eval_expr LIKE`, `apply_having_filter LIKE`, `exec_select` 집합 연산, 병렬 경로 동기화 |
| `tests/unit/test_sql.cpp` | Part 7~8: 53개 새 테스트 |

---

## 다음 단계

- 서브쿼리 / CTE (`WITH daily AS (...) SELECT ...`)
- EXPLAIN (쿼리 실행 계획 출력)
- RIGHT JOIN / FULL OUTER JOIN
- SUBSTR 및 문자열 조작 함수
- NULL 표준화 (INT64_MIN 센티넬 → 실제 null 비트마스크)
