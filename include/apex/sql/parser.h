#pragma once
// ============================================================================
// APEX-DB: SQL Parser
// ============================================================================
// 재귀 하강 파서 (no yacc/bison, no external deps)
// 지원 SQL 서브셋 → SelectStmt AST 생성
// ============================================================================

#include "apex/sql/tokenizer.h"
#include "apex/sql/ast.h"
#include <vector>
#include <string>

namespace apex::sql {

// ============================================================================
// Parser: Token 스트림 → SelectStmt AST
// ============================================================================
class Parser {
public:
    /// SQL 문자열을 파싱하여 SelectStmt 반환
    /// 에러 시 std::runtime_error 던짐
    SelectStmt parse(const std::string& sql);

private:
    std::vector<Token> tokens_;
    size_t             pos_ = 0;

    // ====== 토큰 유틸 ======
    const Token& current() const;
    const Token& peek(size_t offset = 1) const;
    bool         at_end() const;
    bool         check(TokenType t) const;
    bool         match(TokenType t);
    Token        expect(TokenType t, const char* msg);
    void         advance();

    // ====== 파싱 함수 ======
    SelectStmt      parse_select();
    SelectExpr      parse_select_expr();
    JoinClause      parse_join();
    WhereClause     parse_where();
    std::shared_ptr<Expr> parse_expr();
    std::shared_ptr<Expr> parse_and_expr();
    std::shared_ptr<Expr> parse_primary_expr();
    GroupByClause   parse_group_by();
    OrderByClause   parse_order_by();
    WindowSpec      parse_window_spec();  // OVER (...) 파싱

    // ====== 산술 표현식 파싱 (SELECT 컬럼 값식) ======
    std::shared_ptr<ArithExpr> parse_arith_expr_node(); // + and -
    std::shared_ptr<ArithExpr> parse_arith_term();      // * and /
    std::shared_ptr<ArithExpr> parse_arith_primary();   // column, literal, (expr)

    // ====== CASE WHEN 파싱 ======
    std::shared_ptr<CaseWhenExpr> parse_case_when_expr();

    // ====== 헬퍼 ======
    CompareOp       parse_compare_op();
    std::string     parse_qualified_name(std::string& out_alias, std::string& out_col);
    int64_t         parse_integer_literal();
    std::string     parse_string_literal();   // 'unit' → "unit" without quotes
};

} // namespace apex::sql
