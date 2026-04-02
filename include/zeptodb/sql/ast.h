#pragma once
// ============================================================================
// ZeptoDB: SQL AST (Abstract Syntax Tree)
// ============================================================================
// 파서가 생성하는 AST 노드 정의
// 지원 SQL:
//   SELECT [DISTINCT] col_expr [, col_expr ...] [AS alias]
//   FROM table [alias]
//   [ASOF | INNER | LEFT | RIGHT] JOIN table [alias] ON cond [AND cond ...]
//   [WHERE cond [AND|OR cond ...]]
//   [GROUP BY col [, col ...]]
//   [ORDER BY col [ASC|DESC] [, ...]]
//   [LIMIT n]
//   윈도우 함수: func() OVER (PARTITION BY ... ORDER BY ... ROWS N PRECEDING)
// ============================================================================

#include "zeptodb/storage/column_store.h"  // ColumnType
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace zeptodb::sql {

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
    STDDEV,     // STDDEV(col) — population standard deviation
    VARIANCE,   // VARIANCE(col) — population variance
    MEDIAN,     // MEDIAN(col) — 50th percentile
    PERCENTILE, // PERCENTILE(col, N) — Nth percentile (0-100)
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
struct SelectStmt;

// ============================================================================
// CTEDef: single WITH clause entry — "name AS (SELECT ...)"
// ============================================================================
struct CTEDef {
    std::string                 name;
    std::shared_ptr<SelectStmt> stmt;
};

// ============================================================================
// SelectExpr: SELECT 목록의 단일 항목
// ============================================================================
struct SelectExpr {
    AggFunc     agg    = AggFunc::NONE;
    bool        agg_distinct = false;  // COUNT(DISTINCT col)
    std::string table_alias;   // 없으면 ""
    std::string column;        // "*" 이면 전체, "1" 이면 COUNT(*)
    std::string agg_arg2;      // VWAP(price, volume)의 두 번째 인자
    std::string alias;         // AS alias
    bool        is_star = false; // SELECT *

    // XBAR: xbar(col, bucket_size) — 두 번째 인자 (버킷 크기)
    int64_t     xbar_bucket = 0;

    // PERCENTILE(col, N) — N is 0-100
    int64_t     percentile_value = 50;

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

    // FUNC: date/time function call or string function
    // func_name: "date_trunc" | "now" | "epoch_s" | "epoch_ms" | "substr"
    // func_unit: unit string for date_trunc ("ns","us","ms","s","min","hour","day","week")
    // func_arg:  argument expression (nullptr for NOW())
    // func_arg2: second argument expression (for SUBSTR length)
    std::string func_name;
    std::string func_unit;
    std::shared_ptr<ArithExpr> func_arg;
    std::shared_ptr<ArithExpr> func_arg2;
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
        SCALAR_SUBQUERY, // col op (SELECT ...) — uncorrelated scalar subquery
        IN_SUBQUERY,     // col IN (SELECT ...) — uncorrelated IN subquery
    };

    Kind kind = Kind::COMPARE;

    // COMPARE 필드
    std::string table_alias;
    std::string column;
    CompareOp   op = CompareOp::EQ;
    int64_t     value = 0;         // 숫자 리터럴 (정수 기준)
    double      value_f = 0.0;     // 실수 리터럴
    bool        is_float = false;
    std::string value_str;         // 문자열 리터럴
    bool        is_string = false;
    std::shared_ptr<ArithExpr> value_expr; // RHS expression (NOW() - INTERVAL '5 min')

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

    // Scalar / IN subquery (uncorrelated)
    std::shared_ptr<SelectStmt> subquery;
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
    enum class Type { INNER, ASOF, LEFT, RIGHT, FULL, WINDOW, UNION_JOIN, PLUS, AJ0 };

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
    // DATE_TRUNC: GROUP BY DATE_TRUNC('unit', col) 지원
    std::vector<int64_t>     date_trunc_buckets; // 0이면 미사용, >0이면 DATE_TRUNC bucket (ns)
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
    // WITH clause CTE definitions (executed before the main SELECT)
    std::vector<CTEDef>         cte_defs;

    bool                        explain  = false; // EXPLAIN prefix — return plan text
    bool                        distinct = false;
    std::vector<SelectExpr>     columns;       // SELECT 목록
    std::string                 from_table;    // FROM 테이블명 (empty if from_subquery is set)
    std::string                 from_alias;    // 테이블 별칭 (없으면 "")
    std::shared_ptr<SelectStmt> from_subquery; // FROM (SELECT ...) AS alias
    std::optional<JoinClause>   join;          // JOIN 절
    std::optional<WhereClause>  where;         // WHERE 절
    std::optional<GroupByClause> group_by;     // GROUP BY 절
    std::optional<OrderByClause> order_by;     // ORDER BY 절
    std::optional<int64_t>      limit;         // LIMIT n
    std::optional<int64_t>      offset;        // OFFSET n
    std::optional<double>       sample_rate;   // SAMPLE 0.1 (fraction 0..1)
    std::optional<WhereClause>  having;        // HAVING 절 (GROUP BY 이후 집계 필터)

    // Set operations: UNION [ALL] / INTERSECT / EXCEPT
    enum class SetOp { NONE, UNION_ALL, UNION_DISTINCT, INTERSECT, EXCEPT };
    SetOp                       set_op = SetOp::NONE;
    std::shared_ptr<SelectStmt> rhs;           // right-hand side of set operation
};

// ============================================================================
// DDL: Column definition in CREATE TABLE / ALTER TABLE ADD COLUMN
// ============================================================================
struct DdlColumnDef {
    std::string column;    // column name
    std::string type_str;  // "INT64" | "INT32" | "FLOAT64" | "FLOAT32"
                           // | "TIMESTAMP" | "SYMBOL" | "BOOL"
};

// ============================================================================
// DDL: CREATE TABLE [IF NOT EXISTS] name (col type [, col type ...])
// ============================================================================
struct CreateTableStmt {
    std::string               table_name;
    std::vector<DdlColumnDef> columns;
    bool                      if_not_exists = false;
};

// ============================================================================
// DDL: DROP TABLE [IF EXISTS] name
// ============================================================================
struct DropTableStmt {
    std::string table_name;
    bool        if_exists = false;
};

// ============================================================================
// DDL: ALTER TABLE name ...
//   ADD COLUMN col type
//   DROP COLUMN col
//   SET TTL n DAYS | HOURS
// ============================================================================
struct AlterTableStmt {
    enum class Action { ADD_COLUMN, DROP_COLUMN, SET_TTL, SET_ATTRIBUTE, SET_STORAGE_POLICY };

    std::string  table_name;
    Action       action = Action::ADD_COLUMN;

    DdlColumnDef col_def;    // ADD COLUMN
    std::string  col_name;   // DROP COLUMN / SET_ATTRIBUTE target column
    int64_t      ttl_value = 0;   // SET TTL n
    std::string  ttl_unit;        // "DAYS" | "HOURS"
    std::string  attr_type;       // "sorted" | "grouped" | "parted" (SET_ATTRIBUTE)

    // SET STORAGE POLICY
    int64_t      warm_after_ns = 0;
    int64_t      cold_after_ns = 0;
    int64_t      drop_after_ns = 0;
};

// ============================================================================
// DML: INSERT INTO table [(col, ...)] VALUES (v, ...) [, (v, ...) ...]
// ============================================================================
struct InsertValue {
    int64_t i = 0;
    double  f = 0.0;
    std::string s;
    enum Type : uint8_t { INT, FLOAT, STRING } type = INT;
    InsertValue() = default;
    InsertValue(int64_t v) : i(v) {}
    InsertValue(double v) : f(v), type(FLOAT) {}
    InsertValue(std::string v) : s(std::move(v)), type(STRING) {}
};

struct InsertStmt {
    std::string              table_name;
    std::vector<std::string> columns;              // optional column list
    std::vector<std::vector<InsertValue>> value_rows;
};

// ============================================================================
// DML: UPDATE table SET col = val [, col = val ...] [WHERE ...]
// ============================================================================
struct UpdateAssign {
    std::string column;
    int64_t     value = 0;
};

struct UpdateStmt {
    std::string                table_name;
    std::vector<UpdateAssign>  assignments;
    std::optional<WhereClause> where;
};

// ============================================================================
// DML: DELETE FROM table [WHERE ...]
// ============================================================================
struct DeleteStmt {
    std::string                table_name;
    std::optional<WhereClause> where;
};

// ============================================================================
// DDL: CREATE MATERIALIZED VIEW name AS SELECT ...
// ============================================================================
struct CreateMVStmt {
    std::string view_name;
    std::shared_ptr<SelectStmt> query;  // the defining SELECT
};

// ============================================================================
// DDL: DROP MATERIALIZED VIEW name
// ============================================================================
struct DropMVStmt {
    std::string view_name;
};

// ============================================================================
// ParsedStatement: top-level statement — SELECT, DDL, or DML
// ============================================================================
struct ParsedStatement {
    enum class Kind { SELECT, CREATE_TABLE, DROP_TABLE, ALTER_TABLE, INSERT, UPDATE, DELETE,
                      CREATE_MV, DROP_MV, SHOW_TABLES, DESCRIBE_TABLE };

    Kind kind = Kind::SELECT;
    std::optional<SelectStmt>       select;
    std::optional<CreateTableStmt>  create_table;
    std::optional<DropTableStmt>    drop_table;
    std::optional<AlterTableStmt>   alter_table;
    std::optional<InsertStmt>       insert;
    std::optional<UpdateStmt>       update;
    std::optional<DeleteStmt>       del;
    std::optional<CreateMVStmt>     create_mv;
    std::optional<DropMVStmt>       drop_mv;
    std::string                     describe_table_name; // DESCRIBE <table>
};

} // namespace zeptodb::sql
