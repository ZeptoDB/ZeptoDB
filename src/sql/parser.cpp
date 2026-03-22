// ============================================================================
// APEX-DB: SQL Parser Implementation
// ============================================================================
// 재귀 하강 파서 — yacc/bison 없이 순수 C++ 구현
// 윈도우 함수 지원: OVER (PARTITION BY ... ORDER BY ... ROWS N PRECEDING)
// ============================================================================

#include "apex/sql/parser.h"
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <climits>

namespace apex::sql {

// ============================================================================
// 토큰 유틸리티
// ============================================================================
const Token& Parser::current() const {
    if (pos_ < tokens_.size()) return tokens_[pos_];
    static Token end{TokenType::END, ""};
    return end;
}

const Token& Parser::peek(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx < tokens_.size()) return tokens_[idx];
    static Token end{TokenType::END, ""};
    return end;
}

bool Parser::at_end() const {
    return pos_ >= tokens_.size() || tokens_[pos_].type == TokenType::END;
}

bool Parser::check(TokenType t) const {
    return !at_end() && current().type == t;
}

bool Parser::match(TokenType t) {
    if (check(t)) { advance(); return true; }
    return false;
}

Token Parser::expect(TokenType t, const char* msg) {
    if (!check(t)) {
        throw std::runtime_error(
            std::string("Parser: expected ") + msg +
            " but got '" + current().value + "'");
    }
    Token tok = current();
    advance();
    return tok;
}

void Parser::advance() {
    if (pos_ < tokens_.size()) ++pos_;
}

// ============================================================================
// 메인 파싱 진입점
// ============================================================================
SelectStmt Parser::parse(const std::string& sql) {
    Tokenizer tok;
    tokens_ = tok.tokenize(sql);
    pos_ = 0;

    // WITH clause: parse CTE definitions before the main SELECT
    std::vector<CTEDef> cte_defs;
    if (check(TokenType::WITH)) {
        cte_defs = parse_cte_list();
    }

    SelectStmt stmt = parse_select();
    stmt.cte_defs = std::move(cte_defs);

    // UNION [ALL] / INTERSECT / EXCEPT chaining
    while (check(TokenType::UNION) || check(TokenType::INTERSECT) || check(TokenType::EXCEPT)) {
        SelectStmt::SetOp op;
        if (match(TokenType::UNION)) {
            op = match(TokenType::ALL)
                ? SelectStmt::SetOp::UNION_ALL
                : SelectStmt::SetOp::UNION_DISTINCT;
        } else if (match(TokenType::INTERSECT)) {
            op = SelectStmt::SetOp::INTERSECT;
        } else {
            advance(); // EXCEPT
            op = SelectStmt::SetOp::EXCEPT;
        }
        stmt.set_op = op;
        stmt.rhs    = std::make_shared<SelectStmt>(parse_select());
    }
    return stmt;
}

// ============================================================================
// SELECT 파싱
// ============================================================================
SelectStmt Parser::parse_select() {
    expect(TokenType::SELECT, "SELECT");

    SelectStmt stmt;

    // DISTINCT
    if (match(TokenType::DISTINCT)) {
        stmt.distinct = true;
    }

    // SELECT 컬럼 목록
    // SELECT *  또는  SELECT col [, col ...]
    if (check(TokenType::STAR)) {
        advance();
        SelectExpr expr;
        expr.is_star = true;
        stmt.columns.push_back(expr);
    } else {
        stmt.columns.push_back(parse_select_expr());
        while (match(TokenType::COMMA)) {
            stmt.columns.push_back(parse_select_expr());
        }
    }

    // FROM
    expect(TokenType::FROM, "FROM");

    if (check(TokenType::LPAREN)) {
        // Subquery: FROM (SELECT ...) AS alias
        advance(); // consume (
        stmt.from_subquery = std::make_shared<SelectStmt>(parse_select());
        expect(TokenType::RPAREN, ")");
        match(TokenType::AS); // optional AS
        stmt.from_alias = expect(TokenType::IDENT, "subquery alias").value;
    } else {
        stmt.from_table = expect(TokenType::IDENT, "table name").value;

        // 테이블 별칭 (AS는 선택적)
        match(TokenType::AS); // 선택적 AS
        if (check(TokenType::IDENT)
            && current().value != "WHERE"
            && current().value != "JOIN"
            && current().value != "GROUP"
            && current().value != "ORDER"
            && current().value != "LIMIT"
            && current().type != TokenType::WHERE
            && current().type != TokenType::JOIN
            && current().type != TokenType::ASOF
            && current().type != TokenType::GROUP
            && current().type != TokenType::ORDER
            && current().type != TokenType::LIMIT
            && current().type != TokenType::INNER
            && current().type != TokenType::LEFT
            && current().type != TokenType::WINDOW
            && current().type != TokenType::UNION
            && current().type != TokenType::INTERSECT
            && current().type != TokenType::EXCEPT) {
            stmt.from_alias = current().value;
            advance();
        }
    }

    // JOIN (ASOF JOIN, INNER JOIN, JOIN, LEFT JOIN, WINDOW JOIN)
    if (check(TokenType::ASOF) || check(TokenType::JOIN)
        || check(TokenType::INNER) || check(TokenType::LEFT)
        || check(TokenType::WINDOW)) {
        stmt.join = parse_join();
    }

    // WHERE
    if (match(TokenType::WHERE)) {
        stmt.where = parse_where();
    }

    // GROUP BY
    if (check(TokenType::GROUP)) {
        advance();
        expect(TokenType::BY, "BY");
        stmt.group_by = parse_group_by();
    }

    // HAVING (GROUP BY 이후 집계 필터)
    if (check(TokenType::HAVING)) {
        advance();
        stmt.having = parse_where();
    }

    // ORDER BY
    if (check(TokenType::ORDER)) {
        advance();
        expect(TokenType::BY, "BY");
        stmt.order_by = parse_order_by();
    }

    // LIMIT
    if (match(TokenType::LIMIT)) {
        stmt.limit = parse_integer_literal();
    }

    return stmt;
}

// ============================================================================
// SELECT 표현식 파싱 (집계 함수, 윈도우 함수, 또는 컬럼)
// ============================================================================
SelectExpr Parser::parse_select_expr() {
    SelectExpr expr;

    // 집계 함수: SUM(col), AVG(col), COUNT(*), VWAP(price, volume) 등
    auto parse_agg = [&](AggFunc func) {
        expr.agg = func;
        expect(TokenType::LPAREN, "(");
        if (func == AggFunc::COUNT && check(TokenType::STAR)) {
            advance();
            expr.column = "*";
        } else if (func == AggFunc::VWAP) {
            // VWAP(price, volume)
            std::string alias1, col1;
            parse_qualified_name(alias1, col1);
            expr.table_alias = alias1;
            expr.column = col1;
            expect(TokenType::COMMA, ",");
            std::string alias2, col2;
            parse_qualified_name(alias2, col2);
            expr.agg_arg2 = col2;
        } else if (func == AggFunc::XBAR) {
            // XBAR(col, bucket_size)
            std::string alias, col;
            parse_qualified_name(alias, col);
            expr.table_alias = alias;
            expr.column = col;
            expect(TokenType::COMMA, ",");
            expr.xbar_bucket = parse_integer_literal();
        } else {
            // SUM/AVG/MIN/MAX/FIRST/LAST: allow arithmetic expression
            // e.g. SUM(price * volume), AVG(price - 15000)
            auto arith = parse_arith_expr_node();
            if (arith->kind == ArithExpr::Kind::COLUMN && !arith->left) {
                expr.table_alias = arith->table_alias;
                expr.column      = arith->column;
            } else {
                expr.arith_expr = std::move(arith);
            }
        }
        expect(TokenType::RPAREN, ")");
    };

    // ── 윈도우 함수 파싱 헬퍼 ──
    // func_name(arg [, offset]) OVER (...)
    auto parse_window_func = [&](WindowFunc wf, bool has_col, bool has_offset) {
        expr.window_func = wf;
        expect(TokenType::LPAREN, "(");
        if (has_col) {
            // col 인자
            std::string alias, col;
            parse_qualified_name(alias, col);
            expr.table_alias = alias;
            expr.column = col;
            if (has_offset && check(TokenType::COMMA)) {
                advance();
                // EMA: alpha (float) 또는 period (integer)
                if (wf == WindowFunc::EMA) {
                    if (check(TokenType::NUMBER)) {
                        std::string s = current().value;
                        advance();
                        if (s.find('.') != std::string::npos) {
                            // float → alpha 직접
                            expr.ema_alpha = std::stod(s);
                            expr.ema_period = 0;
                        } else {
                            // integer → period (alpha = 2/(period+1))
                            expr.ema_period = std::stoll(s);
                            expr.ema_alpha  = 2.0 / (expr.ema_period + 1.0);
                        }
                    }
                } else {
                    expr.window_offset = parse_integer_literal();
                }
            }
        }
        expect(TokenType::RPAREN, ")");
        // OVER (...)
        expect(TokenType::OVER, "OVER");
        expect(TokenType::LPAREN, "(");
        expr.window_spec = parse_window_spec();
        expect(TokenType::RPAREN, ")");
    };

    switch (current().type) {
        // 일반 집계 함수
        case TokenType::SUM:   advance(); parse_agg(AggFunc::SUM);   break;
        case TokenType::AVG:   advance(); parse_agg(AggFunc::AVG);   break;
        case TokenType::COUNT: advance(); parse_agg(AggFunc::COUNT); break;
        case TokenType::MIN:   advance(); parse_agg(AggFunc::MIN);   break;
        case TokenType::MAX:   advance(); parse_agg(AggFunc::MAX);   break;
        case TokenType::VWAP:  advance(); parse_agg(AggFunc::VWAP);  break;

        // kdb+ 스타일 집계 함수
        case TokenType::XBAR:  advance(); parse_agg(AggFunc::XBAR);  break;
        case TokenType::FIRST: {
            // FIRST는 집계 함수로 사용
            advance();
            // FIRST를 집계로 처리 (NULLS FIRST 와 구분 — 함수 호출 형식)
            if (check(TokenType::LPAREN)) {
                parse_agg(AggFunc::FIRST);
            } else {
                // ORDER BY NULLS FIRST 맥락에서는 IDENT로 처리
                expr.column = "FIRST";
            }
            break;
        }
        case TokenType::LAST: {
            advance();
            if (check(TokenType::LPAREN)) {
                parse_agg(AggFunc::LAST);
            } else {
                expr.column = "LAST";
            }
            break;
        }

        // 윈도우 함수 (OVER 절 필수)
        case TokenType::ROW_NUMBER:  advance(); parse_window_func(WindowFunc::ROW_NUMBER, false, false); break;
        case TokenType::RANK:        advance(); parse_window_func(WindowFunc::RANK, false, false);       break;
        case TokenType::DENSE_RANK:  advance(); parse_window_func(WindowFunc::DENSE_RANK, false, false); break;
        case TokenType::LAG:         advance(); parse_window_func(WindowFunc::LAG, true, true);          break;
        case TokenType::LEAD:        advance(); parse_window_func(WindowFunc::LEAD, true, true);         break;
        // 금융 윈도우 함수
        case TokenType::EMA:         advance(); parse_window_func(WindowFunc::EMA, true, true);          break;
        case TokenType::DELTA:       advance(); parse_window_func(WindowFunc::DELTA, true, false);       break;
        case TokenType::RATIO:       advance(); parse_window_func(WindowFunc::RATIO, true, false);       break;

        case TokenType::STAR:
            advance();
            expr.is_star = true;
            break;

        case TokenType::CASE: {
            // CASE WHEN cond THEN val [...] [ELSE val] END
            advance(); // consume CASE
            expr.case_when = parse_case_when_expr();
            break;
        }

        default: {
            // 일반 컬럼: [alias.]col
            // 단, wj_avg(col), wj_sum(col), wj_count(col) 등 처리
            if (check(TokenType::IDENT)) {
                std::string name = current().value;
                // 대문자 변환
                std::string upper = name;
                std::transform(upper.begin(), upper.end(), upper.begin(),
                               [](unsigned char c){ return std::toupper(c); });

                // wj_ 접두사 윈도우 조인 집계 함수
                if (upper.size() > 3 && upper.substr(0, 3) == "WJ_") {
                    advance(); // 함수 이름 소비
                    std::string agg_type = upper.substr(3);
                    if      (agg_type == "AVG")   expr.wj_agg = WJAggFunc::AVG;
                    else if (agg_type == "SUM")   expr.wj_agg = WJAggFunc::SUM;
                    else if (agg_type == "COUNT") expr.wj_agg = WJAggFunc::COUNT;
                    else if (agg_type == "MIN")   expr.wj_agg = WJAggFunc::MIN;
                    else if (agg_type == "MAX")   expr.wj_agg = WJAggFunc::MAX;
                    else throw std::runtime_error("Unknown wj_ aggregate: " + name);
                    expect(TokenType::LPAREN, "(");
                    std::string alias, col;
                    parse_qualified_name(alias, col);
                    expr.table_alias = alias;
                    expr.column = col;
                    expect(TokenType::RPAREN, ")");
                    break;
                }
            }
            // Plain column or arithmetic expression
            // parse_arith_expr_node handles single column ref as trivial case
            auto arith = parse_arith_expr_node();
            if (arith->kind == ArithExpr::Kind::COLUMN && !arith->left) {
                // Simple column ref (no arithmetic)
                expr.table_alias = arith->table_alias;
                if (arith->column == "*") {
                    expr.is_star = true;
                } else {
                    expr.column = arith->column;
                }
            } else {
                // Arithmetic expression: price * volume, price - 15000, etc.
                expr.arith_expr = std::move(arith);
            }
            break;
        }
    }

    // OVER 체크: 일반 집계 함수 뒤에 OVER가 붙으면 윈도우 함수로 변환
    // (SUM(...) OVER (...) 형태)
    if (expr.agg != AggFunc::NONE && check(TokenType::OVER)) {
        // 집계 함수를 윈도우 함수로 변환
        switch (expr.agg) {
            case AggFunc::SUM: expr.window_func = WindowFunc::SUM; break;
            case AggFunc::AVG: expr.window_func = WindowFunc::AVG; break;
            case AggFunc::MIN: expr.window_func = WindowFunc::MIN; break;
            case AggFunc::MAX: expr.window_func = WindowFunc::MAX; break;
            default: break; // COUNT, VWAP 등은 윈도우 변환 미지원
        }
        if (expr.window_func != WindowFunc::NONE) {
            expr.agg = AggFunc::NONE; // 집계에서 윈도우로 전환
            advance(); // OVER 소비
            expect(TokenType::LPAREN, "(");
            expr.window_spec = parse_window_spec();
            expect(TokenType::RPAREN, ")");
        }
    }

    // AS alias — 키워드도 alias로 허용 (delta, ratio, ema, bar 등)
    if (match(TokenType::AS)) {
        // alias는 IDENT 또는 임의의 키워드 (delta, ema, bar 등)
        // 현재 토큰을 alias로 취급
        if (!at_end() && current().type != TokenType::COMMA
            && current().type != TokenType::FROM
            && current().type != TokenType::WHERE
            && current().type != TokenType::GROUP
            && current().type != TokenType::ORDER
            && current().type != TokenType::LIMIT
            && current().type != TokenType::END) {
            expr.alias = current().value;
            advance();
        }
    } else if (check(TokenType::IDENT)
               && current().value != "FROM"
               && current().type != TokenType::FROM) {
        // 암묵적 alias (AS 없이)
        // 여기서는 보수적으로 AS만 처리
    }

    return expr;
}

// ============================================================================
// OVER (...) 내부 파싱
// PARTITION BY col [, col ...] ORDER BY col [ASC|DESC]
// ROWS [UNBOUNDED] N PRECEDING [AND M FOLLOWING]
// ============================================================================
WindowSpec Parser::parse_window_spec() {
    WindowSpec ws;

    // PARTITION BY
    if (check(TokenType::PARTITION)) {
        advance();
        expect(TokenType::BY, "BY");
        std::string alias, col;
        parse_qualified_name(alias, col);
        ws.partition_by_aliases.push_back(alias);
        ws.partition_by_cols.push_back(col);
        while (check(TokenType::COMMA)) {
            advance();
            parse_qualified_name(alias, col);
            ws.partition_by_aliases.push_back(alias);
            ws.partition_by_cols.push_back(col);
        }
    }

    // ORDER BY
    if (check(TokenType::ORDER)) {
        advance();
        expect(TokenType::BY, "BY");
        std::string alias, col;
        parse_qualified_name(alias, col);
        ws.order_by_aliases.push_back(alias);
        ws.order_by_cols.push_back(col);
        bool asc = true;
        if (check(TokenType::DESC)) { advance(); asc = false; }
        else { match(TokenType::ASC); }
        ws.order_by_asc.push_back(asc);

        while (check(TokenType::COMMA)) {
            advance();
            parse_qualified_name(alias, col);
            ws.order_by_aliases.push_back(alias);
            ws.order_by_cols.push_back(col);
            asc = true;
            if (check(TokenType::DESC)) { advance(); asc = false; }
            else { match(TokenType::ASC); }
            ws.order_by_asc.push_back(asc);
        }
    }

    // ROWS / RANGE 프레임
    if (check(TokenType::ROWS) || check(TokenType::RANGE)) {
        advance(); // ROWS or RANGE 소비
        ws.has_frame = true;

        // BETWEEN ... AND ... 형식 또는 단순 N PRECEDING
        if (check(TokenType::BETWEEN)) {
            advance(); // BETWEEN
            // start frame
            if (check(TokenType::UNBOUNDED)) {
                advance();
                expect(TokenType::PRECEDING, "PRECEDING");
                ws.preceding = INT64_MAX;
            } else if (check(TokenType::CURRENT)) {
                advance();
                expect(TokenType::ROW, "ROW");
                ws.preceding = 0;
            } else {
                ws.preceding = parse_integer_literal();
                expect(TokenType::PRECEDING, "PRECEDING");
            }
            expect(TokenType::AND, "AND");
            // end frame
            if (check(TokenType::UNBOUNDED)) {
                advance();
                expect(TokenType::FOLLOWING, "FOLLOWING");
                ws.following = INT64_MAX;
            } else if (check(TokenType::CURRENT)) {
                advance();
                expect(TokenType::ROW, "ROW");
                ws.following = 0;
            } else {
                ws.following = parse_integer_literal();
                expect(TokenType::FOLLOWING, "FOLLOWING");
            }
        } else {
            // 단순 형식: N PRECEDING 또는 UNBOUNDED PRECEDING
            if (check(TokenType::UNBOUNDED)) {
                advance();
                expect(TokenType::PRECEDING, "PRECEDING");
                ws.preceding = INT64_MAX;
                ws.following = 0;
            } else if (check(TokenType::CURRENT)) {
                advance();
                expect(TokenType::ROW, "ROW");
                ws.preceding = 0;
                ws.following = 0;
            } else {
                ws.preceding = parse_integer_literal();
                expect(TokenType::PRECEDING, "PRECEDING");
                ws.following = 0;
            }
        }
    }

    return ws;
}

// ============================================================================
// JOIN 절 파싱
// ============================================================================
JoinClause Parser::parse_join() {
    JoinClause jc;

    // ASOF JOIN
    if (check(TokenType::ASOF)) {
        advance();
        expect(TokenType::JOIN, "JOIN");
        jc.type = JoinClause::Type::ASOF;
    }
    // INNER JOIN 또는 JOIN
    else if (match(TokenType::INNER)) {
        expect(TokenType::JOIN, "JOIN");
        jc.type = JoinClause::Type::INNER;
    }
    // LEFT [OUTER] JOIN
    else if (match(TokenType::LEFT)) {
        match(TokenType::OUTER); // optional
        expect(TokenType::JOIN, "JOIN");
        jc.type = JoinClause::Type::LEFT;
    }
    // WINDOW JOIN — kdb+ wj 스타일
    else if (check(TokenType::WINDOW)) {
        advance();
        expect(TokenType::JOIN, "JOIN");
        jc.type = JoinClause::Type::WINDOW;
    }
    // JOIN (기본 INNER)
    else {
        expect(TokenType::JOIN, "JOIN");
        jc.type = JoinClause::Type::INNER;
    }

    jc.table = expect(TokenType::IDENT, "join table name").value;

    // 테이블 별칭
    match(TokenType::AS);
    if (check(TokenType::IDENT) && current().type != TokenType::ON) {
        jc.alias = current().value;
        advance();
    }

    // ON 절
    expect(TokenType::ON, "ON");

    // ON 조건 파싱
    // WINDOW JOIN 특수 처리: col BETWEEN expr AND expr
    auto parse_join_cond = [&]() -> bool {
        // qualified_name 파싱 시작
        // WINDOW JOIN: col BETWEEN l_expr AND r_expr 처리
        std::string left_alias, left_col;

        // 먼저 테이블.컬럼 파싱
        parse_qualified_name(left_alias, left_col);

        // BETWEEN 체크 (WINDOW JOIN 시간 조건)
        if (check(TokenType::BETWEEN)) {
            advance(); // BETWEEN 소비
            // 왼쪽 경계: expr (가능하면 col - N 또는 col + N)
            // 간단히: IDENT [DOT IDENT] [MINUS/PLUS NUMBER]
            std::string ref_alias, ref_col;
            parse_qualified_name(ref_alias, ref_col);
            int64_t offset_before = 0;
            bool subtract_before = false;
            if (check(TokenType::MINUS)) {
                advance();
                offset_before = parse_integer_literal();
                subtract_before = true;
            } else if (check(TokenType::PLUS)) {
                advance();
                offset_before = parse_integer_literal();
                subtract_before = false;
            }
            expect(TokenType::AND, "AND");
            // 오른쪽 경계: col [+/- N]
            std::string ref_alias2, ref_col2;
            parse_qualified_name(ref_alias2, ref_col2);
            int64_t offset_after = 0;
            bool add_after = true;
            if (check(TokenType::MINUS)) {
                advance();
                offset_after = parse_integer_literal();
                add_after = false;
            } else if (check(TokenType::PLUS)) {
                advance();
                offset_after = parse_integer_literal();
                add_after = true;
            }

            // WINDOW JOIN 파라미터 채우기
            jc.wj_right_time_col = left_col;
            jc.wj_left_time_col = ref_col; // t.timestamp 등
            // wj_window_before: t.ts - X → X
            jc.wj_window_before = subtract_before ? offset_before : 0;
            // wj_window_after: t.ts + X → X
            jc.wj_window_after = add_after ? offset_after : 0;
            return true; // BETWEEN 처리됨 — JoinCondition은 따로 추가 안 함
        }

        // 일반 비교 조건
        JoinCondition cond;
        cond.left_alias = left_alias;
        cond.left_col   = left_col;
        cond.op = parse_compare_op();
        parse_qualified_name(cond.right_alias, cond.right_col);
        jc.on_conditions.push_back(std::move(cond));
        return false;
    };

    parse_join_cond();
    while (match(TokenType::AND)) {
        parse_join_cond();
    }

    return jc;
}

// ============================================================================
// WHERE 절 파싱
// ============================================================================
WhereClause Parser::parse_where() {
    WhereClause wc;
    wc.expr = parse_expr();
    return wc;
}

// ============================================================================
// 표현식 파싱 (OR 레벨)
// ============================================================================
std::shared_ptr<Expr> Parser::parse_expr() {
    auto left = parse_and_expr();
    while (match(TokenType::OR)) {
        auto right = parse_and_expr();
        auto node = std::make_shared<Expr>();
        node->kind  = Expr::Kind::OR;
        node->left  = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }
    return left;
}

// ============================================================================
// AND 레벨
// ============================================================================
std::shared_ptr<Expr> Parser::parse_and_expr() {
    auto left = parse_primary_expr();
    while (match(TokenType::AND)) {
        auto right = parse_primary_expr();
        auto node = std::make_shared<Expr>();
        node->kind  = Expr::Kind::AND;
        node->left  = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }
    return left;
}

// ============================================================================
// 기본 조건 표현식
// ============================================================================
std::shared_ptr<Expr> Parser::parse_primary_expr() {
    // NOT expr
    if (match(TokenType::NOT)) {
        auto sub = parse_primary_expr();
        auto node = std::make_shared<Expr>();
        node->kind  = Expr::Kind::NOT;
        node->left  = std::move(sub);
        return node;
    }

    // 괄호
    if (match(TokenType::LPAREN)) {
        auto sub = parse_expr();
        expect(TokenType::RPAREN, ")");
        return sub;
    }

    // col [op val | BETWEEN lo AND hi | IN (...) | IS [NOT] NULL]
    auto node = std::make_shared<Expr>();
    parse_qualified_name(node->table_alias, node->column);

    // BETWEEN
    if (match(TokenType::BETWEEN)) {
        node->kind = Expr::Kind::BETWEEN;
        node->lo   = parse_integer_literal();
        expect(TokenType::AND, "AND");
        node->hi   = parse_integer_literal();
        return node;
    }

    // IN (v1, v2, ...)
    if (match(TokenType::IN)) {
        node->kind = Expr::Kind::IN;
        expect(TokenType::LPAREN, "(");
        // Parse comma-separated integer/float literals
        if (check(TokenType::NUMBER)) {
            std::string s = current().value;
            advance();
            node->in_values.push_back(
                s.find('.') != std::string::npos
                    ? static_cast<int64_t>(std::stod(s))
                    : std::stoll(s));
        } else if (check(TokenType::STRING)) {
            // String literals: attempt numeric parse; keep 0 if not numeric
            std::string s = current().value;
            advance();
            try { node->in_values.push_back(std::stoll(s)); }
            catch (...) { node->in_values.push_back(0); }
        }
        while (match(TokenType::COMMA)) {
            if (check(TokenType::NUMBER)) {
                std::string s = current().value;
                advance();
                node->in_values.push_back(
                    s.find('.') != std::string::npos
                        ? static_cast<int64_t>(std::stod(s))
                        : std::stoll(s));
            } else if (check(TokenType::STRING)) {
                std::string s = current().value;
                advance();
                try { node->in_values.push_back(std::stoll(s)); }
                catch (...) { node->in_values.push_back(0); }
            } else {
                break;
            }
        }
        expect(TokenType::RPAREN, ")");
        return node;
    }

    // IS [NOT] NULL
    if (match(TokenType::IS)) {
        node->kind = Expr::Kind::IS_NULL;
        if (match(TokenType::NOT)) {
            node->negated = true;
        }
        expect(TokenType::NULL_KW, "NULL");
        return node;
    }

    // [NOT] LIKE 'pattern'
    if (check(TokenType::LIKE)) {
        advance();
        node->kind         = Expr::Kind::LIKE;
        node->negated      = false;
        node->like_pattern = parse_string_literal();
        return node;
    }
    if (check(TokenType::NOT) && peek().type == TokenType::LIKE) {
        advance(); // consume NOT
        advance(); // consume LIKE
        node->kind         = Expr::Kind::LIKE;
        node->negated      = true;
        node->like_pattern = parse_string_literal();
        return node;
    }

    // 비교 연산자
    node->kind = Expr::Kind::COMPARE;
    node->op   = parse_compare_op();

    // 오른쪽 값 (숫자 또는 문자열)
    if (check(TokenType::NUMBER)) {
        std::string s = current().value;
        advance();
        // 실수 여부 체크
        if (s.find('.') != std::string::npos) {
            node->value_f = std::stod(s);
            node->is_float = true;
            node->value = static_cast<int64_t>(node->value_f);
        } else {
            node->value = std::stoll(s);
        }
    } else if (check(TokenType::STRING)) {
        // 문자열 → 숫자 변환 시도 (심볼 ID 등)
        std::string s = current().value;
        advance();
        try { node->value = std::stoll(s); }
        catch (...) { node->value = 0; }
    } else if (check(TokenType::IDENT) || check(TokenType::STAR)) {
        // 다른 컬럼과 비교 (조인 조건 등 — WHERE에서는 드물지만)
        node->value = 0;
        advance();
    } else {
        throw std::runtime_error(
            "Parser: expected value in WHERE condition, got '"
            + current().value + "'");
    }

    return node;
}

// ============================================================================
// GROUP BY 파싱
// ============================================================================
GroupByClause Parser::parse_group_by() {
    GroupByClause gc;

    // 단일 GROUP BY 항목 파싱 람다
    auto parse_one = [&]() {
        if (check(TokenType::XBAR)) {
            // XBAR(col, bucket) in GROUP BY
            advance(); // XBAR 소비
            expect(TokenType::LPAREN, "(");
            std::string alias, col;
            parse_qualified_name(alias, col);
            expect(TokenType::COMMA, ",");
            int64_t bucket = parse_integer_literal();
            expect(TokenType::RPAREN, ")");
            gc.aliases.push_back(alias);
            gc.columns.push_back(col);
            gc.xbar_buckets.push_back(bucket);
        } else {
            std::string alias, col;
            parse_qualified_name(alias, col);
            gc.aliases.push_back(alias);
            gc.columns.push_back(col);
            gc.xbar_buckets.push_back(0); // 0 = 일반 컬럼
        }
    };

    parse_one();
    while (match(TokenType::COMMA)) {
        parse_one();
    }
    return gc;
}

// ============================================================================
// ORDER BY 파싱
// ============================================================================
OrderByClause Parser::parse_order_by() {
    OrderByClause oc;

    auto parse_item = [&]() {
        OrderByItem item;
        parse_qualified_name(item.table_alias, item.column);
        if (match(TokenType::DESC)) {
            item.asc = false;
        } else {
            match(TokenType::ASC); // 선택적
        }
        oc.items.push_back(std::move(item));
    };

    parse_item();
    while (match(TokenType::COMMA)) {
        parse_item();
    }
    return oc;
}

// ============================================================================
// 비교 연산자 파싱
// ============================================================================
CompareOp Parser::parse_compare_op() {
    switch (current().type) {
        case TokenType::EQ: advance(); return CompareOp::EQ;
        case TokenType::NE: advance(); return CompareOp::NE;
        case TokenType::GT: advance(); return CompareOp::GT;
        case TokenType::LT: advance(); return CompareOp::LT;
        case TokenType::GE: advance(); return CompareOp::GE;
        case TokenType::LE: advance(); return CompareOp::LE;
        default:
            throw std::runtime_error(
                "Parser: expected comparison operator, got '"
                + current().value + "'");
    }
}

// ============================================================================
// 한정된 이름 파싱: [alias.]col 또는 col
// ============================================================================
std::string Parser::parse_qualified_name(std::string& out_alias,
                                          std::string& out_col) {
    out_alias.clear();
    // IDENT [DOT IDENT]
    if (check(TokenType::IDENT)) {
        std::string first = current().value;
        advance();
        if (match(TokenType::DOT)) {
            // alias.col
            out_alias = first;
            if (check(TokenType::STAR)) {
                out_col = "*";
                advance();
            } else {
                out_col = expect(TokenType::IDENT, "column name").value;
            }
        } else {
            out_col = first;
        }
    } else if (check(TokenType::STAR)) {
        out_col = "*";
        advance();
    } else {
        throw std::runtime_error(
            "Parser: expected identifier, got '" + current().value + "'");
    }
    return out_col;
}

// ============================================================================
// 정수 리터럴 파싱
// ============================================================================
int64_t Parser::parse_integer_literal() {
    if (check(TokenType::NUMBER)) {
        std::string s = current().value;
        advance();
        return std::stoll(s);
    }
    throw std::runtime_error(
        "Parser: expected integer literal, got '" + current().value + "'");
}

// ============================================================================
// 문자열 리터럴 파싱: 'value' → "value" (quotes stripped)
// ============================================================================
std::string Parser::parse_string_literal() {
    if (check(TokenType::STRING)) {
        std::string s = current().value;
        advance();
        return s;
    }
    throw std::runtime_error(
        "Parser: expected string literal, got '" + current().value + "'");
}

// ============================================================================
// Arithmetic expression parsing (value expressions in SELECT list)
// Precedence: primary > term (* /) > expr (+ -)
// ============================================================================

// Primary: column ref, integer literal, parenthesized expression, or function call
std::shared_ptr<ArithExpr> Parser::parse_arith_primary() {
    auto node = std::make_shared<ArithExpr>();

    if (match(TokenType::LPAREN)) {
        node = parse_arith_expr_node();
        expect(TokenType::RPAREN, ")");
        return node;
    }

    if (check(TokenType::NUMBER)) {
        node->kind = ArithExpr::Kind::LITERAL;
        std::string s = current().value;
        advance();
        node->literal = s.find('.') != std::string::npos
            ? static_cast<int64_t>(std::stod(s))
            : std::stoll(s);
        return node;
    }

    // DATE_TRUNC('unit', expr)
    if (check(TokenType::DATE_TRUNC)) {
        advance();
        expect(TokenType::LPAREN, "(");
        node->kind      = ArithExpr::Kind::FUNC;
        node->func_name = "date_trunc";
        node->func_unit = parse_string_literal();
        expect(TokenType::COMMA, ",");
        node->func_arg  = parse_arith_expr_node();
        expect(TokenType::RPAREN, ")");
        return node;
    }

    // NOW()
    if (check(TokenType::NOW)) {
        advance();
        expect(TokenType::LPAREN, "(");
        expect(TokenType::RPAREN, ")");
        node->kind      = ArithExpr::Kind::FUNC;
        node->func_name = "now";
        return node;
    }

    // EPOCH_S(expr)
    if (check(TokenType::EPOCH_S)) {
        advance();
        expect(TokenType::LPAREN, "(");
        node->kind      = ArithExpr::Kind::FUNC;
        node->func_name = "epoch_s";
        node->func_arg  = parse_arith_expr_node();
        expect(TokenType::RPAREN, ")");
        return node;
    }

    // EPOCH_MS(expr)
    if (check(TokenType::EPOCH_MS)) {
        advance();
        expect(TokenType::LPAREN, "(");
        node->kind      = ArithExpr::Kind::FUNC;
        node->func_name = "epoch_ms";
        node->func_arg  = parse_arith_expr_node();
        expect(TokenType::RPAREN, ")");
        return node;
    }

    // Column reference: [alias.]col
    node->kind = ArithExpr::Kind::COLUMN;
    parse_qualified_name(node->table_alias, node->column);
    return node;
}

// Term: primary (* primary | / primary)*
std::shared_ptr<ArithExpr> Parser::parse_arith_term() {
    auto left = parse_arith_primary();
    while (check(TokenType::STAR) || check(TokenType::SLASH)) {
        ArithOp op = check(TokenType::STAR) ? ArithOp::MUL : ArithOp::DIV;
        advance();
        auto right = parse_arith_primary();
        auto node = std::make_shared<ArithExpr>();
        node->kind = ArithExpr::Kind::BINARY;
        node->arith_op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }
    return left;
}

// Expr: term (+ term | - term)*
std::shared_ptr<ArithExpr> Parser::parse_arith_expr_node() {
    auto left = parse_arith_term();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        ArithOp op = check(TokenType::PLUS) ? ArithOp::ADD : ArithOp::SUB;
        advance();
        auto right = parse_arith_term();
        auto node = std::make_shared<ArithExpr>();
        node->kind = ArithExpr::Kind::BINARY;
        node->arith_op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }
    return left;
}

// ============================================================================
// CASE WHEN cond THEN val [WHEN cond THEN val ...] [ELSE val] END
// ============================================================================
std::shared_ptr<CaseWhenExpr> Parser::parse_case_when_expr() {
    auto cwe = std::make_shared<CaseWhenExpr>();

    while (check(TokenType::WHEN)) {
        advance(); // consume WHEN
        CaseWhenBranch branch;
        branch.when_cond = parse_expr();         // full condition expression
        expect(TokenType::THEN, "THEN");
        branch.then_val = parse_arith_expr_node(); // value expression
        cwe->branches.push_back(std::move(branch));
    }

    if (match(TokenType::ELSE)) {
        cwe->else_val = parse_arith_expr_node();
    }

    expect(TokenType::CASE_END, "END");
    return cwe;
}

// ============================================================================
// CTE list parsing: WITH name AS (SELECT ...) [, name AS (SELECT ...)]
// ============================================================================
std::vector<CTEDef> Parser::parse_cte_list() {
    advance(); // consume WITH

    std::vector<CTEDef> defs;
    do {
        CTEDef cte;
        cte.name = expect(TokenType::IDENT, "CTE name").value;
        expect(TokenType::AS, "AS");
        expect(TokenType::LPAREN, "(");
        cte.stmt = std::make_shared<SelectStmt>(parse_select());
        expect(TokenType::RPAREN, ")");
        defs.push_back(std::move(cte));
    } while (match(TokenType::COMMA));

    return defs;
}

} // namespace apex::sql
