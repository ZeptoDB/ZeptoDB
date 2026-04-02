// ============================================================================
// ZeptoDB: SQL Tokenizer Implementation
// ============================================================================

#include "zeptodb/sql/tokenizer.h"
#include <cctype>
#include <algorithm>
#include <stdexcept>

namespace zeptodb::sql {

// ============================================================================
// 키워드 매핑 테이블
// ============================================================================
TokenType Tokenizer::keyword_type(const std::string& upper) {
    if (upper == "SELECT")      return TokenType::SELECT;
    if (upper == "FROM")        return TokenType::FROM;
    if (upper == "WHERE")       return TokenType::WHERE;
    if (upper == "JOIN")        return TokenType::JOIN;
    if (upper == "ASOF")        return TokenType::ASOF;
    if (upper == "ON")          return TokenType::ON;
    if (upper == "AND")         return TokenType::AND;
    if (upper == "OR")          return TokenType::OR;
    if (upper == "NOT")         return TokenType::NOT;
    if (upper == "IN")          return TokenType::IN;
    if (upper == "IS")          return TokenType::IS;
    if (upper == "NULL")        return TokenType::NULL_KW;
    if (upper == "GROUP")       return TokenType::GROUP;
    if (upper == "BY")          return TokenType::BY;
    if (upper == "ORDER")       return TokenType::ORDER;
    if (upper == "LIMIT")       return TokenType::LIMIT;
    if (upper == "OFFSET")      return TokenType::OFFSET;
    if (upper == "BETWEEN")     return TokenType::BETWEEN;
    if (upper == "HAVING")      return TokenType::HAVING;
    if (upper == "AS")          return TokenType::AS;
    if (upper == "DISTINCT")    return TokenType::DISTINCT;
    if (upper == "INNER")       return TokenType::INNER;
    if (upper == "LEFT")        return TokenType::LEFT;
    if (upper == "RIGHT")       return TokenType::RIGHT;
    if (upper == "OUTER")       return TokenType::OUTER;
    if (upper == "CROSS")       return TokenType::CROSS;
    if (upper == "ASC")         return TokenType::ASC;
    if (upper == "DESC")        return TokenType::DESC;
    if (upper == "NULLS")       return TokenType::NULLS;
    if (upper == "FIRST")       return TokenType::FIRST;
    if (upper == "LAST")        return TokenType::LAST;
    // 집계 함수
    if (upper == "SUM")         return TokenType::SUM;
    if (upper == "AVG")         return TokenType::AVG;
    if (upper == "COUNT")       return TokenType::COUNT;
    if (upper == "MIN")         return TokenType::MIN;
    if (upper == "MAX")         return TokenType::MAX;
    if (upper == "VWAP")        return TokenType::VWAP;
    if (upper == "STDDEV")      return TokenType::STDDEV;
    if (upper == "VARIANCE")    return TokenType::VARIANCE;
    if (upper == "MEDIAN")      return TokenType::MEDIAN;
    if (upper == "PERCENTILE" || upper == "PERCENTILE_CONT")
                                return TokenType::PERCENTILE;
    // 윈도우 함수 키워드
    if (upper == "OVER")        return TokenType::OVER;
    if (upper == "PARTITION")   return TokenType::PARTITION;
    if (upper == "ROWS")        return TokenType::ROWS;
    if (upper == "RANGE")       return TokenType::RANGE;
    if (upper == "PRECEDING")   return TokenType::PRECEDING;
    if (upper == "FOLLOWING")   return TokenType::FOLLOWING;
    if (upper == "UNBOUNDED")   return TokenType::UNBOUNDED;
    if (upper == "CURRENT")     return TokenType::CURRENT;
    if (upper == "ROW")         return TokenType::ROW;
    if (upper == "RANK")        return TokenType::RANK;
    if (upper == "DENSE_RANK")  return TokenType::DENSE_RANK;
    if (upper == "ROW_NUMBER")  return TokenType::ROW_NUMBER;
    if (upper == "LAG")         return TokenType::LAG;
    if (upper == "LEAD")        return TokenType::LEAD;
    // 금융 함수 키워드
    if (upper == "XBAR")        return TokenType::XBAR;
    if (upper == "EMA")         return TokenType::EMA;
    if (upper == "DELTA")       return TokenType::DELTA;
    if (upper == "RATIO")       return TokenType::RATIO;
    if (upper == "WINDOW")      return TokenType::WINDOW;
    // CASE WHEN 키워드
    if (upper == "CASE")        return TokenType::CASE;
    if (upper == "WHEN")        return TokenType::WHEN;
    if (upper == "THEN")        return TokenType::THEN;
    if (upper == "ELSE")        return TokenType::ELSE;
    if (upper == "END")         return TokenType::CASE_END;
    // 날짜/시간 함수
    if (upper == "DATE_TRUNC")  return TokenType::DATE_TRUNC;
    if (upper == "NOW")         return TokenType::NOW;
    if (upper == "EPOCH_S")     return TokenType::EPOCH_S;
    if (upper == "EPOCH_MS")    return TokenType::EPOCH_MS;
    // 문자열 연산자
    if (upper == "LIKE")        return TokenType::LIKE;
    if (upper == "SUBSTR")      return TokenType::SUBSTR;
    // kdb+ 조인 키워드
    if (upper == "FULL")        return TokenType::FULL;
    if (upper == "PLUS")        return TokenType::PLUS_JOIN;
    if (upper == "AJ0")         return TokenType::AJ0;
    // Query plan
    if (upper == "EXPLAIN")     return TokenType::EXPLAIN;
    if (upper == "INTERVAL")    return TokenType::INTERVAL;
    if (upper == "SAMPLE")      return TokenType::SAMPLE;
    // CTE / subquery
    if (upper == "WITH")        return TokenType::WITH;
    // DML
    if (upper == "INSERT")      return TokenType::INSERT;
    if (upper == "INTO")        return TokenType::INTO;
    if (upper == "VALUES")      return TokenType::VALUES;
    if (upper == "UPDATE")      return TokenType::UPDATE;
    if (upper == "SET")         return TokenType::SET;
    if (upper == "DELETE")      return TokenType::DELETE_KW;
    if (upper == "MATERIALIZED") return TokenType::MATERIALIZED;
    if (upper == "VIEW")        return TokenType::VIEW;
    // 집합 연산
    if (upper == "UNION")       return TokenType::UNION;
    if (upper == "ALL")         return TokenType::ALL;
    if (upper == "INTERSECT")   return TokenType::INTERSECT;
    if (upper == "EXCEPT")      return TokenType::EXCEPT;
    // 일반 식별자
    return TokenType::IDENT;
}

// ============================================================================
// 유틸리티
// ============================================================================
char Tokenizer::peek(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx >= sql_.size()) return '\0';
    return sql_[idx];
}

char Tokenizer::advance() {
    return sql_[pos_++];
}

bool Tokenizer::at_end() const {
    return pos_ >= sql_.size();
}

void Tokenizer::skip_whitespace() {
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) {
        advance();
    }
}

// ============================================================================
// 숫자 리터럴 파싱
// ============================================================================
Token Tokenizer::read_number() {
    std::string num;
    while (!at_end() && (std::isdigit(static_cast<unsigned char>(peek()))
                         || peek() == '.')) {
        num += advance();
    }
    return Token{TokenType::NUMBER, std::move(num)};
}

// ============================================================================
// 문자열 리터럴 파싱 (단순 '...' 지원)
// ============================================================================
Token Tokenizer::read_string() {
    advance(); // 여는 '
    std::string s;
    while (!at_end() && peek() != '\'') {
        s += advance();
    }
    if (at_end()) {
        throw std::runtime_error("Tokenizer: unterminated string literal");
    }
    advance(); // 닫는 '
    return Token{TokenType::STRING, std::move(s)};
}

// ============================================================================
// 식별자 / 키워드 파싱
// DENSE_RANK, ROW_NUMBER 같은 underscore 포함 식별자 지원
// ============================================================================
Token Tokenizer::read_ident_or_keyword() {
    std::string ident;
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek()))
                         || peek() == '_')) {
        ident += advance();
    }
    // 대문자 변환하여 키워드 체크
    std::string upper = ident;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    TokenType t = keyword_type(upper);
    if (t == TokenType::IDENT) {
        return Token{TokenType::IDENT, std::move(ident)};
    }
    return Token{t, std::move(ident)};
}

// ============================================================================
// 메인 토크나이저
// ============================================================================
std::vector<Token> Tokenizer::tokenize(const std::string& sql) {
    sql_ = sql;
    pos_ = 0;
    std::vector<Token> tokens;
    tokens.reserve(64);

    while (true) {
        skip_whitespace();
        if (at_end()) break;

        char c = peek();

        // 주석 (-- 스타일)
        if (c == '-' && peek(1) == '-') {
            while (!at_end() && peek() != '\n') advance();
            continue;
        }

        // 숫자
        if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(read_number());
            continue;
        }

        // '-' 처리: 항상 MINUS 토큰으로 처리
        // 음수 리터럴 파싱은 파서 레벨에서 MINUS + NUMBER로 처리됨
        if (c == '-' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            // 직전 토큰에 따라 결정:
            // - 직전이 RPAREN/NUMBER/IDENT이면 이항 MINUS
            // - 그 외 (시작, 괄호 안, 연산자 뒤)이면 음수 리터럴
            bool is_binary = false;
            if (!tokens.empty()) {
                auto tt = tokens.back().type;
                if (tt == TokenType::RPAREN || tt == TokenType::NUMBER ||
                    tt == TokenType::IDENT) {
                    is_binary = true;
                }
            }
            if (is_binary) {
                advance(); // '-' 소비
                tokens.push_back({TokenType::MINUS, "-"});
            } else {
                // 음수 리터럴: '-' + digits → 단일 NUMBER 토큰
                advance(); // '-'
                Token t = read_number();
                t.value = "-" + t.value;
                tokens.push_back(std::move(t));
            }
            continue;
        }

        // 문자열 리터럴
        if (c == '\'') {
            tokens.push_back(read_string());
            continue;
        }

        // 식별자 또는 키워드 (underscore 포함: DENSE_RANK, ROW_NUMBER 등)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(read_ident_or_keyword());
            continue;
        }

        // backtick 식별자
        if (c == '`') {
            advance();
            std::string ident;
            while (!at_end() && peek() != '`') ident += advance();
            if (!at_end()) advance();
            tokens.push_back(Token{TokenType::IDENT, std::move(ident)});
            continue;
        }

        // 연산자 및 구분자
        advance(); // c 소비
        switch (c) {
            case ',': tokens.push_back({TokenType::COMMA, ","}); break;
            case '.': tokens.push_back({TokenType::DOT,   "."}); break;
            case '(': tokens.push_back({TokenType::LPAREN,"("}); break;
            case ')': tokens.push_back({TokenType::RPAREN,")"}); break;
            case '*': tokens.push_back({TokenType::STAR,  "*"}); break;
            case '+': tokens.push_back({TokenType::PLUS,  "+"}); break;
            case '-': tokens.push_back({TokenType::MINUS, "-"}); break;
            case '/': tokens.push_back({TokenType::SLASH, "/"}); break;
            case '=': tokens.push_back({TokenType::EQ,    "="}); break;
            case '>':
                if (!at_end() && peek() == '=') {
                    advance();
                    tokens.push_back({TokenType::GE, ">="});
                } else {
                    tokens.push_back({TokenType::GT, ">"});
                }
                break;
            case '<':
                if (!at_end() && peek() == '=') {
                    advance();
                    tokens.push_back({TokenType::LE, "<="});
                } else if (!at_end() && peek() == '>') {
                    advance();
                    tokens.push_back({TokenType::NE, "<>"});
                } else {
                    tokens.push_back({TokenType::LT, "<"});
                }
                break;
            case '!':
                if (!at_end() && peek() == '=') {
                    advance();
                    tokens.push_back({TokenType::NE, "!="});
                } else {
                    throw std::runtime_error(
                        std::string("Tokenizer: unexpected character '!'"
                                    " at position ") + std::to_string(pos_));
                }
                break;
            default:
                throw std::runtime_error(
                    std::string("Tokenizer: unexpected character '")
                    + c + "' at position " + std::to_string(pos_));
        }
    }

    tokens.push_back({TokenType::END, ""});
    return tokens;
}

} // namespace zeptodb::sql
