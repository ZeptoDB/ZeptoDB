// ============================================================================
// APEX-DB: SQL Tokenizer Implementation
// ============================================================================

#include "apex/sql/tokenizer.h"
#include <cctype>
#include <algorithm>
#include <stdexcept>

namespace apex::sql {

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
    if (upper == "GROUP")       return TokenType::GROUP;
    if (upper == "BY")          return TokenType::BY;
    if (upper == "ORDER")       return TokenType::ORDER;
    if (upper == "LIMIT")       return TokenType::LIMIT;
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

        // 음수 리터럴 (-123)
        if (c == '-' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            advance(); // '-'
            Token t = read_number();
            t.value = "-" + t.value;
            tokens.push_back(std::move(t));
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

} // namespace apex::sql
