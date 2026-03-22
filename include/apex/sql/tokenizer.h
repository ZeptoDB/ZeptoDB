#pragma once
// ============================================================================
// APEX-DB: SQL Tokenizer
// ============================================================================
// 재귀 하강 파서를 위한 토크나이저
// 지원 SQL 서브셋: SELECT, FROM, WHERE, JOIN, ASOF JOIN, GROUP BY, ORDER BY,
//                  LIMIT, BETWEEN, LIKE 일부, 기본 집계 함수,
//                  윈도우 함수 (OVER, PARTITION, ROWS, PRECEDING, FOLLOWING,
//                               UNBOUNDED, CURRENT, ROW, RANK, DENSE_RANK,
//                               ROW_NUMBER, LAG, LEAD)
// ============================================================================

#include <string>
#include <vector>
#include <stdexcept>

namespace apex::sql {

// ============================================================================
// TokenType: 지원 토큰 목록
// ============================================================================
enum class TokenType {
    // SQL 키워드
    SELECT, FROM, WHERE, JOIN, ASOF, ON, AND, OR, NOT,
    GROUP, BY, ORDER, LIMIT, BETWEEN, HAVING, AS, DISTINCT,
    INNER, LEFT, RIGHT, OUTER, CROSS,
    ASC, DESC, NULLS, FIRST, LAST,

    // 집계 함수 키워드
    SUM, AVG, COUNT, MIN, MAX, VWAP,

    // 윈도우 함수 키워드
    OVER,           // OVER
    PARTITION,      // PARTITION
    ROWS,           // ROWS
    RANGE,          // RANGE
    PRECEDING,      // PRECEDING
    FOLLOWING,      // FOLLOWING
    UNBOUNDED,      // UNBOUNDED
    CURRENT,        // CURRENT
    ROW,            // ROW (CURRENT ROW)
    RANK,           // RANK()
    DENSE_RANK,     // DENSE_RANK()
    ROW_NUMBER,     // ROW_NUMBER()
    LAG,            // LAG()
    LEAD,           // LEAD()

    // 금융 함수 키워드 (kdb+ 스타일)
    XBAR,       // XBAR(col, bucket) — 시간 바 집계
    EMA,        // EMA(col, alpha) OVER (...) — 지수이동평균
    DELTA,      // DELTA(col) OVER (...) — 행간 차이
    RATIO,      // RATIO(col) OVER (...) — 행간 비율
    WINDOW,     // WINDOW JOIN 에 사용

    // 날짜/시간 함수 키워드
    DATE_TRUNC, // DATE_TRUNC('unit', col)
    NOW,        // NOW()
    EPOCH_S,    // EPOCH_S(col) — nanoseconds → seconds
    EPOCH_MS,   // EPOCH_MS(col) — nanoseconds → milliseconds

    // 문자열 연산자
    LIKE,       // col LIKE 'pattern'

    // 집합 연산
    UNION,      // UNION [ALL]
    ALL,        // UNION ALL
    INTERSECT,  // INTERSECT
    EXCEPT,     // EXCEPT

    // 집합 / NULL 연산자
    IN,       // IN (...)
    IS,       // IS [NOT] NULL
    NULL_KW,  // NULL keyword

    // 비교 연산자
    GT,   // >
    LT,   // <
    GE,   // >=
    LE,   // <=
    EQ,   // =
    NE,   // != or <>

    // 산술 연산자
    PLUS,     // +
    MINUS,    // - (이항)
    SLASH,    // /

    // CASE WHEN 키워드
    CASE,     // CASE
    WHEN,     // WHEN
    THEN,     // THEN
    ELSE,     // ELSE
    CASE_END, // END (CASE ... END 의 닫는 키워드)

    // 구분자/특수문자
    COMMA,    // ,
    DOT,      // .
    LPAREN,   // (
    RPAREN,   // )
    STAR,     // *

    // 리터럴
    IDENT,    // 식별자 (테이블명, 컬럼명)
    NUMBER,   // 정수 또는 실수 리터럴
    STRING,   // 'string' 리터럴

    // 특수
    END,      // EOF
};

// ============================================================================
// Token: 단일 토큰
// ============================================================================
struct Token {
    TokenType   type;
    std::string value;   // NUMBER라면 문자열 그대로, IDENT라면 원본 식별자

    Token(TokenType t, std::string v = {})
        : type(t), value(std::move(v)) {}
};

// ============================================================================
// Tokenizer: SQL 문자열 → Token 스트림
// ============================================================================
class Tokenizer {
public:
    /// SQL 문자열을 토큰 벡터로 변환
    /// 에러 시 std::runtime_error 던짐
    std::vector<Token> tokenize(const std::string& sql);

private:
    std::string sql_;
    size_t      pos_  = 0;

    // 유틸리티
    char        peek(size_t offset = 0) const;
    char        advance();
    void        skip_whitespace();
    bool        at_end() const;

    // 토큰 파싱 함수
    Token       read_number();
    Token       read_string();
    Token       read_ident_or_keyword();

    // 키워드 → TokenType 매핑
    static TokenType keyword_type(const std::string& upper);
};

} // namespace apex::sql
