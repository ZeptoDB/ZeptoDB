// ============================================================================
// APEX-DB: kdb+ q Parser Implementation
// ============================================================================
#include "apex/migration/q_parser.h"
#include <stdexcept>

namespace apex::migration {

QParser::QParser(const std::vector<QToken>& tokens)
    : tokens_(tokens)
    , pos_(0)
{}

const QToken& QParser::current() const {
    if (pos_ >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[pos_];
}

const QToken& QParser::peek(size_t offset) const {
    if (pos_ + offset >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[pos_ + offset];
}

void QParser::advance() {
    if (pos_ < tokens_.size()) {
        ++pos_;
    }
}

bool QParser::match(QTokenType type) {
    if (current().type == type) {
        advance();
        return true;
    }
    return false;
}

bool QParser::expect(QTokenType type) {
    if (!match(type)) {
        throw std::runtime_error("Expected token type");
    }
    return true;
}

std::shared_ptr<QNode> QParser::parse() {
    // q 쿼리는 보통 select로 시작
    if (current().type == QTokenType::SELECT) {
        return parse_select();
    }

    throw std::runtime_error("Unsupported q statement");
}

std::shared_ptr<QNode> QParser::parse_select() {
    auto select_node = std::make_shared<QNode>(QNodeType::SELECT);

    expect(QTokenType::SELECT);

    // Column list 또는 표현식
    // 간단화: 전체 선택 또는 컬럼 리스트
    if (current().type == QTokenType::FROM) {
        // select from trades (전체 선택)
    } else {
        // select col1, col2 from trades
        while (current().type != QTokenType::FROM &&
               current().type != QTokenType::BY &&
               current().type != QTokenType::END_OF_FILE) {
            select_node->add_child(parse_expression());

            if (match(QTokenType::COMMA)) {
                continue;
            } else {
                break;
            }
        }
    }

    // FROM 절
    if (current().type == QTokenType::FROM) {
        select_node->add_child(parse_from());
    }

    // WHERE 절
    if (current().type == QTokenType::WHERE) {
        select_node->add_child(parse_where());
    }

    // BY 절 (GROUP BY)
    if (current().type == QTokenType::BY) {
        select_node->add_child(parse_by());
    }

    return select_node;
}

std::shared_ptr<QNode> QParser::parse_from() {
    auto from_node = std::make_shared<QNode>(QNodeType::FROM);

    expect(QTokenType::FROM);

    // 테이블 이름
    if (current().type == QTokenType::IDENTIFIER ||
        current().type == QTokenType::SYMBOL) {
        from_node->value = current().value;
        advance();
    }

    return from_node;
}

std::shared_ptr<QNode> QParser::parse_where() {
    auto where_node = std::make_shared<QNode>(QNodeType::WHERE);

    expect(QTokenType::WHERE);

    // WHERE 조건식
    where_node->add_child(parse_expression());

    // fby가 있으면 처리
    if (current().type == QTokenType::FBY) {
        where_node->add_child(parse_fby());
    }

    return where_node;
}

std::shared_ptr<QNode> QParser::parse_by() {
    auto by_node = std::make_shared<QNode>(QNodeType::BY);

    expect(QTokenType::BY);

    // BY 컬럼 리스트
    while (current().type != QTokenType::END_OF_FILE) {
        if (current().type == QTokenType::IDENTIFIER ||
            current().type == QTokenType::SYMBOL) {
            auto col = std::make_shared<QNode>(QNodeType::COLUMN, current().value);
            by_node->add_child(col);
            advance();
        }

        if (match(QTokenType::COMMA)) {
            continue;
        } else {
            break;
        }
    }

    return by_node;
}

std::shared_ptr<QNode> QParser::parse_fby() {
    auto fby_node = std::make_shared<QNode>(QNodeType::FBY);

    expect(QTokenType::FBY);

    // fby 컬럼
    if (current().type == QTokenType::IDENTIFIER ||
        current().type == QTokenType::SYMBOL) {
        fby_node->value = current().value;
        advance();
    }

    return fby_node;
}

std::shared_ptr<QNode> QParser::parse_aj() {
    auto aj_node = std::make_shared<QNode>(QNodeType::AJ);

    expect(QTokenType::AJ);
    expect(QTokenType::LBRACKET);

    // aj[`time`sym;table1;table2]
    // 간단화: 심볼 리스트, 테이블1, 테이블2
    aj_node->add_child(parse_symbol_list());
    expect(QTokenType::SEMICOLON);

    // table1
    if (current().type == QTokenType::IDENTIFIER) {
        auto table1 = std::make_shared<QNode>(QNodeType::FROM, current().value);
        aj_node->add_child(table1);
        advance();
    }
    expect(QTokenType::SEMICOLON);

    // table2
    if (current().type == QTokenType::IDENTIFIER) {
        auto table2 = std::make_shared<QNode>(QNodeType::FROM, current().value);
        aj_node->add_child(table2);
        advance();
    }

    expect(QTokenType::RBRACKET);

    return aj_node;
}

std::shared_ptr<QNode> QParser::parse_wj() {
    auto wj_node = std::make_shared<QNode>(QNodeType::WJ);

    expect(QTokenType::WJ);
    expect(QTokenType::LBRACKET);

    // wj[w;`sym`time;table1;(table2;(max;`bid);(min;`ask))]
    // 간단화: 구현 생략 (복잡함)

    throw std::runtime_error("wj parsing not yet implemented");
}

std::shared_ptr<QNode> QParser::parse_expression() {
    // 간단한 표현식: 컬럼, 리터럴, 함수 호출

    // 함수 호출: func[args]
    if (current().type == QTokenType::IDENTIFIER && peek().type == QTokenType::LBRACKET) {
        return parse_function_call();
    }

    // 컬럼
    if (current().type == QTokenType::IDENTIFIER ||
        current().type == QTokenType::SYMBOL) {
        return parse_column();
    }

    // 리터럴
    if (current().type == QTokenType::NUMBER ||
        current().type == QTokenType::STRING ||
        current().type == QTokenType::DATE) {
        return parse_literal();
    }

    // 이항 연산자
    auto left = parse_column();

    if (current().type == QTokenType::EQ ||
        current().type == QTokenType::LT ||
        current().type == QTokenType::GT ||
        current().type == QTokenType::LE ||
        current().type == QTokenType::GE ||
        current().type == QTokenType::NE) {

        auto op_node = std::make_shared<QNode>(QNodeType::BINARY_OP, current().value);
        advance();

        op_node->add_child(left);
        op_node->add_child(parse_literal());

        return op_node;
    }

    return left;
}

std::shared_ptr<QNode> QParser::parse_function_call() {
    auto func_node = std::make_shared<QNode>(QNodeType::FUNCTION_CALL, current().value);

    advance(); // function name
    expect(QTokenType::LBRACKET);

    // Arguments
    while (current().type != QTokenType::RBRACKET &&
           current().type != QTokenType::END_OF_FILE) {
        func_node->add_child(parse_expression());

        if (match(QTokenType::SEMICOLON) || match(QTokenType::COMMA)) {
            continue;
        } else {
            break;
        }
    }

    expect(QTokenType::RBRACKET);

    return func_node;
}

std::shared_ptr<QNode> QParser::parse_column() {
    auto col_node = std::make_shared<QNode>(QNodeType::COLUMN, current().value);
    advance();
    return col_node;
}

std::shared_ptr<QNode> QParser::parse_literal() {
    std::string value = current().value;
    advance();

    return std::make_shared<QNode>(QNodeType::LITERAL, value);
}

std::shared_ptr<QNode> QParser::parse_symbol_list() {
    auto list_node = std::make_shared<QNode>(QNodeType::SYMBOL_LIST);

    // `time`sym 형식
    while (current().type == QTokenType::SYMBOL) {
        auto sym = std::make_shared<QNode>(QNodeType::COLUMN, current().value);
        list_node->add_child(sym);
        advance();
    }

    return list_node;
}

} // namespace apex::migration
