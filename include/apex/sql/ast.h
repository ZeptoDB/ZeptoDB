#pragma once
// ============================================================================
// APEX-DB: SQL AST (Abstract Syntax Tree)
// ============================================================================
// 파서가 생성하는 AST 노드 정의
// 지원 SQL:
//   SELECT [DISTINCT] col_expr [, col_expr ...] [AS alias]
//   FROM table [alias]
//   [ASOF | INNER | LEFT] JOIN table [alias] ON cond [AND cond ...]
//   [WHERE cond [AND|OR cond ...]]
//   [GROUP BY col [, col ...]]
//   [ORDER BY col [ASC|DESC] [, ...]]
//   [LIMIT n]
//   윈도우 함수: func() OVER (PARTITION BY ... ORDER BY ... ROWS N PRECEDING)
// ============================================================================

#include "apex/storage/column_store.h"  // ColumnType
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace apex::sql {

// ============================================================================
// AggFunc: 집계 함수 종류
// ============================================================================
enum class AggFunc {
    NONE,   // 집계 없음 (일반 컬럼)
    COUNT,  // COUNT(*) or COUNT(col)
    SUM,
    AVG,
    MIN,
    MAX,
    VWAP,   // VWAP(price, volume) — 금융 특화
    FIRST,  // FIRST(col) — kdb+ first(): 그룹의 첫 번째 값 (OHLC용 OPEN)
    LAST,   // LAST(col)  — kdb+ last():  그룹의 마지막 값 (OHLC용 CLOSE)
    XBAR,   // XBAR(col, bucket) — 시간 버킷 플로어 (GROUP BY 키로도 사용)
};

// ============================================================================
// WindowFunc: 윈도우 함수 종류
// ============================================================================
enum class WindowFunc {
    NONE,
    ROW_NUMBER,
    RANK,
    DENSE_RANK,
    SUM,
    AVG,
    MIN,
    MAX,
    LAG,
    LEAD,
    EMA,    // EMA(col, alpha_or_period) — 지수이동평균
    DELTA,  // DELTA(col) — 행간 차이 (x[i] - x[i-1])
    RATIO,  // RATIO(col) — 행간 비율 (x[i] / x[i-1])
};

// ============================================================================
// WindowSpec: OVER (...) 절 내용
// ============================================================================
struct WindowSpec {
    // PARTITION BY col [, col ...]
    std::vector<std::string> partition_by_aliases;
    std::vector<std::string> partition_by_cols;

    // ORDER BY col [ASC|DESC]
    std::vector<std::string> order_by_aliases;
    std::vector<std::string> order_by_cols;
    std::vector<bool>        order_by_asc;

    // ROWS BETWEEN N PRECEDING AND M FOLLOWING
    // ROWS N PRECEDING  → preceding=N, following=0
    // ROWS UNBOUNDED PRECEDING → preceding=INT64_MAX, following=0
    bool    has_frame   = false;
    int64_t preceding   = INT64_MAX;  // UNBOUNDED = INT64_MAX
    int64_t following   = 0;          // CURRENT ROW = 0
};

// ============================================================================
// WJAggFunc: WINDOW JOIN 내에서 사용되는 집계 함수
// ============================================================================
enum class WJAggFunc {
    NONE,
    AVG,
    SUM,
    COUNT,
    MIN,
    MAX,
};

// Forward declarations for types referenced before their full definition.
struct ArithExpr;
struct CaseWhenExpr;
struct Expr;

// ============================================================================
// SelectExpr: SELECT 목록의 단일 항목
// ============================================================================
struct SelectExpr {
    AggFunc     agg    = AggFunc::NONE;
    std::string table_alias;   // 없으면 ""
    std::string column;        // "*" 이면 전체, "1" 이면 COUNT(*)
    std::string agg_arg2;      // VWAP(price, volume)의 두 번째 인자
    std::string alias;         // AS alias
    bool        is_star = false; // SELECT *

    // XBAR: xbar(col, bucket_size) — 두 번째 인자 (버킷 크기)
    int64_t     xbar_bucket = 0;

    // WINDOW JOIN 집계: wj_avg(q.col) 등
    WJAggFunc   wj_agg = WJAggFunc::NONE;

    // 윈도우 함수 (OVER 절이 있으면 window_func != NONE)
    WindowFunc  window_func    = WindowFunc::NONE;
    int64_t     window_offset  = 1;     // LAG/LEAD offset
    int64_t     window_default = 0;     // LAG/LEAD default value
    double      ema_alpha      = 0.0;   // EMA alpha 파라미터 (0이면 period 기반)
    int64_t     ema_period     = 0;     // EMA period (alpha = 2/(period+1))
    std::optional<WindowSpec> window_spec;  // OVER (...)

    // Arithmetic expression: price * volume AS notional, etc.
    // Non-null overrides column/agg for value computation.
    std::shared_ptr<ArithExpr>   arith_expr;

    // CASE WHEN expression (non-null overrides all above for value computation)
    std::shared_ptr<CaseWhenExpr> case_when;
};

// ============================================================================
// ArithOp: Arithmetic operator
// ============================================================================
enum class ArithOp { ADD, SUB, MUL, DIV };

// ============================================================================
// ArithExpr: Value expression node — column ref, integer literal, or binary op
// Supports: price * volume, price - 15000, (close - open) / open, etc.
// ============================================================================
struct ArithExpr {
    enum class Kind { COLUMN, LITERAL, BINARY, FUNC };
    Kind kind = Kind::COLUMN;

    // COLUMN: qualified column reference
    std::string table_alias;
    std::string column;

    // LITERAL: integer constant
    int64_t literal = 0;

    // BINARY: left op right
    ArithOp arith_op = ArithOp::MUL;
    std::shared_ptr<ArithExpr> left;
    std::shared_ptr<ArithExpr> right;

    // FUNC: date/time function call
    // func_name: "date_trunc" | "now" | "epoch_s" | "epoch_ms"
    // func_unit: unit string for date_trunc ("ns","us","ms","s","min","hour","day","week")
    // func_arg:  argument expression (nullptr for NOW())
    std::string func_name;
    std::string func_unit;
    std::shared_ptr<ArithExpr> func_arg;
};

// ============================================================================
// CaseWhenExpr: CASE WHEN cond THEN val [...] [ELSE val] END
// ============================================================================
struct CaseWhenBranch {
    std::shared_ptr<Expr>      when_cond; // WHEN condition (uses Expr tree)
    std::shared_ptr<ArithExpr> then_val;  // THEN value expression
};

struct CaseWhenExpr {
    std::vector<CaseWhenBranch> branches;
    std::shared_ptr<ArithExpr>  else_val; // ELSE value (nullptr → 0)
};

// ============================================================================
// CompareOp: 비교 연산자
// ============================================================================
enum class CompareOp { EQ, NE, GT, LT, GE, LE };

// ============================================================================
// Expr: WHERE 조건의 단일 표현식
// ============================================================================
struct Expr {
    enum class Kind {
        COMPARE,     // col op val
        BETWEEN,     // col BETWEEN lo AND hi
        AND,         // left AND right
        OR,          // left OR right
        NOT,         // NOT expr  (left = subexpr)
        IN,          // col IN (v1, v2, ...)
        IS_NULL,     // col IS [NOT] NULL  (negated=true → IS NOT NULL)
        LIKE,        // col LIKE 'pattern'  (negated=true → NOT LIKE)
    };

    Kind kind = Kind::COMPARE;

    // COMPARE 필드
    std::string table_alias;
    std::string column;
    CompareOp   op = CompareOp::EQ;
    int64_t     value = 0;         // 숫자 리터럴 (정수 기준)
    double      value_f = 0.0;     // 실수 리터럴
    bool        is_float = false;

    // BETWEEN 필드
    int64_t     lo = 0;
    int64_t     hi = 0;

    // IN 필드: col IN (v1, v2, ...)
    std::vector<int64_t> in_values;

    // LIKE 필드: col LIKE 'pattern'
    std::string like_pattern;

    // IS_NULL / NOT / LIKE: negated=true → IS NOT NULL / NOT LIKE
    bool negated = false;

    // AND / OR / NOT 결합
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;
};

// ============================================================================
// WhereClause
// ============================================================================
struct WhereClause {
    std::shared_ptr<Expr> expr;
};

// ============================================================================
// JoinCondition: ON 절의 단일 조건
// ============================================================================
struct JoinCondition {
    // t.symbol = q.symbol 또는 t.timestamp >= q.timestamp
    std::string left_alias;
    std::string left_col;
    CompareOp   op = CompareOp::EQ;
    std::string right_alias;
    std::string right_col;
};

// ============================================================================
// JoinClause
// ============================================================================
struct JoinClause {
    enum class Type { INNER, ASOF, LEFT, WINDOW };

    Type                        type = Type::INNER;
    std::string                 table;
    std::string                 alias;
    std::vector<JoinCondition>  on_conditions;

    // WINDOW JOIN 전용: 시간 윈도우 집계
    // SELECT wj_avg(q.bid) AS avg_bid — SELECT 목록에서 wj_ 집계로 처리
    // ON t.symbol = q.symbol AND q.timestamp BETWEEN t.ts - W AND t.ts + W
    std::string  wj_left_time_col;    // 왼쪽 타임스탬프 컬럼 (예: timestamp)
    std::string  wj_right_time_col;   // 오른쪽 타임스탬프 컬럼
    int64_t      wj_window_before = 0; // 윈도우 크기: t.ts - wj_window_before
    int64_t      wj_window_after  = 0; // 윈도우 크기: t.ts + wj_window_after
};

// ============================================================================
// GroupByClause
// ============================================================================
struct GroupByClause {
    std::vector<std::string> columns;  // table_alias.col 또는 col
    std::vector<std::string> aliases;  // 대응되는 테이블 alias (없으면 "")
    // XBAR: GROUP BY xbar(col, bucket) 지원
    std::vector<int64_t>     xbar_buckets;  // 0이면 일반 컬럼, >0이면 xbar
};

// ============================================================================
// OrderByClause
// ============================================================================
struct OrderByItem {
    std::string table_alias;
    std::string column;
    bool        asc = true;
};

struct OrderByClause {
    std::vector<OrderByItem> items;
};

// ============================================================================
// SelectStmt: 최상위 SELECT 문
// ============================================================================
struct SelectStmt {
    bool                        distinct = false;
    std::vector<SelectExpr>     columns;       // SELECT 목록
    std::string                 from_table;    // FROM 테이블명
    std::string                 from_alias;    // 테이블 별칭 (없으면 "")
    std::optional<JoinClause>   join;          // JOIN 절
    std::optional<WhereClause>  where;         // WHERE 절
    std::optional<GroupByClause> group_by;     // GROUP BY 절
    std::optional<OrderByClause> order_by;     // ORDER BY 절
    std::optional<int64_t>      limit;         // LIMIT n
    std::optional<WhereClause>  having;        // HAVING 절 (GROUP BY 이후 집계 필터)

    // Set operations: UNION [ALL] / INTERSECT / EXCEPT
    enum class SetOp { NONE, UNION_ALL, UNION_DISTINCT, INTERSECT, EXCEPT };
    SetOp                       set_op = SetOp::NONE;
    std::shared_ptr<SelectStmt> rhs;           // right-hand side of set operation
};

} // namespace apex::sql
