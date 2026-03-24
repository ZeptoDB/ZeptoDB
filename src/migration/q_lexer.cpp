// ============================================================================
// ZeptoDB: kdb+ q Lexer Implementation
// ============================================================================
#include "zeptodb/migration/q_parser.h"
#include <cctype>
#include <stdexcept>

namespace zeptodb::migration {

QLexer::QLexer(const std::string& source)
    : source_(source)
    , pos_(0)
    , line_(1)
    , column_(1)
{}

char QLexer::current() const {
    if (pos_ >= source_.length()) return '\0';
    return source_[pos_];
}

char QLexer::peek(size_t offset) const {
    if (pos_ + offset >= source_.length()) return '\0';
    return source_[pos_ + offset];
}

void QLexer::advance() {
    if (pos_ < source_.length()) {
        if (source_[pos_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++pos_;
    }
}

void QLexer::skip_whitespace() {
    while (std::isspace(current())) {
        advance();
    }
}

void QLexer::skip_comment() {
    // q 주석: / 로 시작
    if (current() == '/' && peek() != ' ') {
        while (current() != '\n' && current() != '\0') {
            advance();
        }
    }
}

QToken QLexer::read_number() {
    std::string num;
    size_t start_line = line_;
    size_t start_col = column_;

    while (std::isdigit(current()) || current() == '.') {
        num += current();
        advance();
    }

    return QToken(QTokenType::NUMBER, num, start_line, start_col);
}

QToken QLexer::read_identifier() {
    std::string id;
    size_t start_line = line_;
    size_t start_col = column_;

    while (std::isalnum(current()) || current() == '_') {
        id += current();
        advance();
    }

    // Keywords
    if (id == "select") return QToken(QTokenType::SELECT, id, start_line, start_col);
    if (id == "from") return QToken(QTokenType::FROM, id, start_line, start_col);
    if (id == "where") return QToken(QTokenType::WHERE, id, start_line, start_col);
    if (id == "by") return QToken(QTokenType::BY, id, start_line, start_col);
    if (id == "fby") return QToken(QTokenType::FBY, id, start_line, start_col);
    if (id == "aj") return QToken(QTokenType::AJ, id, start_line, start_col);
    if (id == "wj") return QToken(QTokenType::WJ, id, start_line, start_col);
    if (id == "exec") return QToken(QTokenType::EXEC, id, start_line, start_col);
    if (id == "update") return QToken(QTokenType::UPDATE, id, start_line, start_col);
    if (id == "delete") return QToken(QTokenType::DELETE, id, start_line, start_col);
    if (id == "and") return QToken(QTokenType::AND, id, start_line, start_col);
    if (id == "or") return QToken(QTokenType::OR, id, start_line, start_col);
    if (id == "in") return QToken(QTokenType::IN, id, start_line, start_col);
    if (id == "not") return QToken(QTokenType::NOT, id, start_line, start_col);
    if (id == "within") return QToken(QTokenType::WITHIN, id, start_line, start_col);
    if (id == "like") return QToken(QTokenType::LIKE, id, start_line, start_col);
    if (id == "each") return QToken(QTokenType::EACH, id, start_line, start_col);

    return QToken(QTokenType::IDENTIFIER, id, start_line, start_col);
}

QToken QLexer::read_string() {
    std::string str;
    size_t start_line = line_;
    size_t start_col = column_;

    advance(); // skip "

    while (current() != '"' && current() != '\0') {
        if (current() == '\\') {
            advance();
            if (current() == 'n') str += '\n';
            else if (current() == 't') str += '\t';
            else if (current() == '"') str += '"';
            else str += current();
        } else {
            str += current();
        }
        advance();
    }

    if (current() == '"') advance(); // skip closing "

    return QToken(QTokenType::STRING, str, start_line, start_col);
}

QToken QLexer::read_symbol() {
    std::string sym;
    size_t start_line = line_;
    size_t start_col = column_;

    advance(); // skip `

    while (std::isalnum(current()) || current() == '_' || current() == '.') {
        sym += current();
        advance();
    }

    return QToken(QTokenType::SYMBOL, sym, start_line, start_col);
}

QToken QLexer::read_date() {
    std::string date;
    size_t start_line = line_;
    size_t start_col = column_;

    // YYYY.MM.DD 형식
    while (std::isdigit(current()) || current() == '.') {
        date += current();
        advance();
    }

    return QToken(QTokenType::DATE, date, start_line, start_col);
}

std::vector<QToken> QLexer::tokenize() {
    std::vector<QToken> tokens;

    while (current() != '\0') {
        skip_whitespace();
        skip_comment();

        if (current() == '\0') break;

        size_t start_line = line_;
        size_t start_col = column_;

        // Numbers and dates
        if (std::isdigit(current())) {
            // 날짜 형식 체크: YYYY.MM.DD
            if (peek() >= '0' && peek() <= '9' &&
                peek(2) == '.' && peek(3) >= '0' && peek(3) <= '9') {
                tokens.push_back(read_date());
            } else {
                tokens.push_back(read_number());
            }
            continue;
        }

        // Identifiers and keywords
        if (std::isalpha(current()) || current() == '_') {
            tokens.push_back(read_identifier());
            continue;
        }

        // Strings
        if (current() == '"') {
            tokens.push_back(read_string());
            continue;
        }

        // Symbols
        if (current() == '`') {
            tokens.push_back(read_symbol());
            continue;
        }

        // Operators and punctuation
        switch (current()) {
            case '(':
                tokens.push_back(QToken(QTokenType::LPAREN, "(", start_line, start_col));
                advance();
                break;
            case ')':
                tokens.push_back(QToken(QTokenType::RPAREN, ")", start_line, start_col));
                advance();
                break;
            case '[':
                tokens.push_back(QToken(QTokenType::LBRACKET, "[", start_line, start_col));
                advance();
                break;
            case ']':
                tokens.push_back(QToken(QTokenType::RBRACKET, "]", start_line, start_col));
                advance();
                break;
            case '{':
                tokens.push_back(QToken(QTokenType::LBRACE, "{", start_line, start_col));
                advance();
                break;
            case '}':
                tokens.push_back(QToken(QTokenType::RBRACE, "}", start_line, start_col));
                advance();
                break;
            case ';':
                tokens.push_back(QToken(QTokenType::SEMICOLON, ";", start_line, start_col));
                advance();
                break;
            case ',':
                tokens.push_back(QToken(QTokenType::COMMA, ",", start_line, start_col));
                advance();
                break;
            case ':':
                if (peek() == ':') {
                    tokens.push_back(QToken(QTokenType::DCOLON, "::", start_line, start_col));
                    advance(); advance();
                } else {
                    tokens.push_back(QToken(QTokenType::COLON, ":", start_line, start_col));
                    advance();
                }
                break;
            case '=':
                tokens.push_back(QToken(QTokenType::EQ, "=", start_line, start_col));
                advance();
                break;
            case '+':
                tokens.push_back(QToken(QTokenType::PLUS, "+", start_line, start_col));
                advance();
                break;
            case '-':
                // Negative number: - followed by digit with no preceding identifier/number
                if (std::isdigit(peek()) &&
                    (tokens.empty() ||
                     tokens.back().type == QTokenType::LPAREN ||
                     tokens.back().type == QTokenType::LBRACKET ||
                     tokens.back().type == QTokenType::SEMICOLON ||
                     tokens.back().type == QTokenType::COMMA ||
                     tokens.back().type == QTokenType::COLON ||
                     tokens.back().type == QTokenType::EQ ||
                     tokens.back().type == QTokenType::LT ||
                     tokens.back().type == QTokenType::GT ||
                     tokens.back().type == QTokenType::PLUS ||
                     tokens.back().type == QTokenType::MINUS ||
                     tokens.back().type == QTokenType::STAR ||
                     tokens.back().type == QTokenType::PERCENT)) {
                    advance(); // skip '-'
                    auto num_tok = read_number();
                    num_tok.value = "-" + num_tok.value;
                    tokens.push_back(num_tok);
                } else {
                    tokens.push_back(QToken(QTokenType::MINUS, "-", start_line, start_col));
                    advance();
                }
                break;
            case '*':
                tokens.push_back(QToken(QTokenType::STAR, "*", start_line, start_col));
                advance();
                break;
            case '%':
                tokens.push_back(QToken(QTokenType::PERCENT, "%", start_line, start_col));
                advance();
                break;
            case '~':
                tokens.push_back(QToken(QTokenType::TILDE, "~", start_line, start_col));
                advance();
                break;
            case '!':
                tokens.push_back(QToken(QTokenType::BANG, "!", start_line, start_col));
                advance();
                break;
            case '#':
                tokens.push_back(QToken(QTokenType::HASH, "#", start_line, start_col));
                advance();
                break;
            case '@':
                tokens.push_back(QToken(QTokenType::AT, "@", start_line, start_col));
                advance();
                break;
            case '?':
                tokens.push_back(QToken(QTokenType::QUESTION, "?", start_line, start_col));
                advance();
                break;
            case '$':
                tokens.push_back(QToken(QTokenType::DOLLAR, "$", start_line, start_col));
                advance();
                break;
            case '.':
                // Could be .z.d, .z.t etc — read as identifier
                {
                    std::string dotid;
                    dotid += current();
                    advance();
                    while (std::isalnum(current()) || current() == '.' || current() == '_') {
                        dotid += current();
                        advance();
                    }
                    tokens.push_back(QToken(QTokenType::IDENTIFIER, dotid, start_line, start_col));
                }
                break;
            case '<':
                if (peek() == '=') {
                    tokens.push_back(QToken(QTokenType::LE, "<=", start_line, start_col));
                    advance();
                    advance();
                } else if (peek() == '>') {
                    tokens.push_back(QToken(QTokenType::NE, "<>", start_line, start_col));
                    advance();
                    advance();
                } else {
                    tokens.push_back(QToken(QTokenType::LT, "<", start_line, start_col));
                    advance();
                }
                break;
            case '>':
                if (peek() == '=') {
                    tokens.push_back(QToken(QTokenType::GE, ">=", start_line, start_col));
                    advance();
                    advance();
                } else {
                    tokens.push_back(QToken(QTokenType::GT, ">", start_line, start_col));
                    advance();
                }
                break;
            default:
                // Skip unknown characters instead of throwing
                advance();
                break;
        }
    }

    tokens.push_back(QToken(QTokenType::END_OF_FILE, "", line_, column_));
    return tokens;
}

} // namespace zeptodb::migration
