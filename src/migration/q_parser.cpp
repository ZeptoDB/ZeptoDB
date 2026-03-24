// ============================================================================
// ZeptoDB: kdb+ q Parser Implementation (Extended)
// ============================================================================
#include "zeptodb/migration/q_parser.h"
#include <stdexcept>
#include <unordered_map>

namespace zeptodb::migration {

QParser::QParser(const std::vector<QToken>& tokens)
    : tokens_(tokens), pos_(0) {}

const QToken& QParser::current() const {
    if (pos_ >= tokens_.size()) return tokens_.back();
    return tokens_[pos_];
}

const QToken& QParser::peek(size_t offset) const {
    if (pos_ + offset >= tokens_.size()) return tokens_.back();
    return tokens_[pos_ + offset];
}

void QParser::advance() {
    if (pos_ < tokens_.size()) ++pos_;
}

bool QParser::match(QTokenType type) {
    if (current().type == type) { advance(); return true; }
    return false;
}

bool QParser::expect(QTokenType type) {
    if (!match(type)) throw std::runtime_error("Expected token type");
    return true;
}

// ============================================================================
// Known q functions for prefix/infix detection
// ============================================================================
static bool is_q_function(const std::string& name) {
    static const std::unordered_map<std::string, bool> funcs = {
        {"sum",1},{"avg",1},{"min",1},{"max",1},{"count",1},
        {"first",1},{"last",1},{"wavg",1},{"xbar",1},{"ema",1},
        {"mavg",1},{"msum",1},{"mmin",1},{"mmax",1},
        {"deltas",1},{"ratios",1},{"sums",1},{"prds",1},
        {"med",1},{"var",1},{"dev",1},{"sdev",1},
        {"prev",1},{"next",1},{"neg",1},{"abs",1},{"sqrt",1},
        {"floor",1},{"ceiling",1},{"reciprocal",1},
        {"string",1},{"type",1},{"null",1},{"fills",1},
        {"distinct",1},{"asc",1},{"desc",1},{"rank",1},
        {"til",1},{"reverse",1},{"raze",1},{"enlist",1},
        {"flip",1},{"cols",1},{"keys",1},{"value",1},
        {"log",1},{"exp",1},{"signum",1},
    };
    return funcs.count(name) > 0;
}

// ============================================================================
// Top-level parse
// ============================================================================
std::shared_ptr<QNode> QParser::parse() {
    if (current().type == QTokenType::END_OF_FILE)
        throw std::runtime_error("Empty query");

    switch (current().type) {
        case QTokenType::SELECT: return parse_select();
        case QTokenType::UPDATE: return parse_update();
        case QTokenType::DELETE: return parse_delete();
        case QTokenType::EXEC:   return parse_exec();
        case QTokenType::AJ: {
            auto node = std::make_shared<QNode>(QNodeType::SELECT);
            node->add_child(parse_aj());
            return node;
        }
        case QTokenType::LBRACE: return parse_function_def();
        default: break;
    }

    // Try expression (assignment, function call, etc.)
    auto expr = parse_expression();
    if (expr) return expr;

    throw std::runtime_error("Unsupported q statement");
}

// ============================================================================
// select <cols> by <groups> from <table> where <cond1>, <cond2>
// ============================================================================
std::shared_ptr<QNode> QParser::parse_select() {
    auto node = std::make_shared<QNode>(QNodeType::SELECT);
    expect(QTokenType::SELECT);

    // Columns
    if (current().type != QTokenType::FROM &&
        current().type != QTokenType::BY) {
        while (current().type != QTokenType::FROM &&
               current().type != QTokenType::BY &&
               current().type != QTokenType::END_OF_FILE) {
            node->add_child(parse_expression());
            if (!match(QTokenType::COMMA)) break;
        }
    }

    // BY (before FROM in q)
    if (current().type == QTokenType::BY)
        node->add_child(parse_by());

    // FROM
    if (current().type == QTokenType::FROM)
        node->add_child(parse_from());

    // WHERE — supports multiple comma-separated conditions
    if (current().type == QTokenType::WHERE)
        node->add_child(parse_where());

    return node;
}

// ============================================================================
// update <col>:<expr> from <table> where <cond>
// ============================================================================
std::shared_ptr<QNode> QParser::parse_update() {
    auto node = std::make_shared<QNode>(QNodeType::UPDATE);
    expect(QTokenType::UPDATE);

    // Assignment list: col:expr, col:expr
    while (current().type != QTokenType::FROM &&
           current().type != QTokenType::BY &&
           current().type != QTokenType::END_OF_FILE) {
        node->add_child(parse_expression());
        if (!match(QTokenType::COMMA)) break;
    }

    if (current().type == QTokenType::BY)
        node->add_child(parse_by());
    if (current().type == QTokenType::FROM)
        node->add_child(parse_from());
    if (current().type == QTokenType::WHERE)
        node->add_child(parse_where());

    return node;
}

// ============================================================================
// delete <cols> from <table> where <cond>
// ============================================================================
std::shared_ptr<QNode> QParser::parse_delete() {
    auto node = std::make_shared<QNode>(QNodeType::DELETE);
    expect(QTokenType::DELETE);

    // Optional column list
    while (current().type != QTokenType::FROM &&
           current().type != QTokenType::END_OF_FILE) {
        node->add_child(parse_expression());
        if (!match(QTokenType::COMMA)) break;
    }

    if (current().type == QTokenType::FROM)
        node->add_child(parse_from());
    if (current().type == QTokenType::WHERE)
        node->add_child(parse_where());

    return node;
}

// ============================================================================
// exec <cols> from <table> where <cond>
// ============================================================================
std::shared_ptr<QNode> QParser::parse_exec() {
    auto node = std::make_shared<QNode>(QNodeType::EXEC);
    expect(QTokenType::EXEC);

    while (current().type != QTokenType::FROM &&
           current().type != QTokenType::BY &&
           current().type != QTokenType::END_OF_FILE) {
        node->add_child(parse_expression());
        if (!match(QTokenType::COMMA)) break;
    }

    if (current().type == QTokenType::BY)
        node->add_child(parse_by());
    if (current().type == QTokenType::FROM)
        node->add_child(parse_from());
    if (current().type == QTokenType::WHERE)
        node->add_child(parse_where());

    return node;
}

// ============================================================================
// FROM
// ============================================================================
std::shared_ptr<QNode> QParser::parse_from() {
    auto node = std::make_shared<QNode>(QNodeType::FROM);
    expect(QTokenType::FROM);

    if (current().type == QTokenType::IDENTIFIER ||
        current().type == QTokenType::SYMBOL) {
        node->value = current().value;
        advance();
    } else {
        throw std::runtime_error("Expected table name after FROM");
    }
    return node;
}

// ============================================================================
// WHERE — multiple comma-separated conditions
// ============================================================================
std::shared_ptr<QNode> QParser::parse_where() {
    auto node = std::make_shared<QNode>(QNodeType::WHERE);
    expect(QTokenType::WHERE);

    node->add_child(parse_expression());

    // Multiple conditions: where cond1, cond2
    while (match(QTokenType::COMMA)) {
        node->add_child(parse_expression());
    }

    if (current().type == QTokenType::FBY)
        node->add_child(parse_fby());

    return node;
}

// ============================================================================
// BY
// ============================================================================
std::shared_ptr<QNode> QParser::parse_by() {
    auto node = std::make_shared<QNode>(QNodeType::BY);
    expect(QTokenType::BY);

    while (current().type != QTokenType::FROM &&
           current().type != QTokenType::END_OF_FILE) {
        if (current().type == QTokenType::IDENTIFIER ||
            current().type == QTokenType::SYMBOL) {
            node->add_child(std::make_shared<QNode>(QNodeType::COLUMN, current().value));
            advance();
        } else {
            // Could be expression like xbar[5;time]
            node->add_child(parse_expression());
        }
        if (!match(QTokenType::COMMA)) break;
    }
    return node;
}

// ============================================================================
// FBY
// ============================================================================
std::shared_ptr<QNode> QParser::parse_fby() {
    auto node = std::make_shared<QNode>(QNodeType::FBY);
    expect(QTokenType::FBY);
    if (current().type == QTokenType::IDENTIFIER ||
        current().type == QTokenType::SYMBOL) {
        node->value = current().value;
        advance();
    }
    return node;
}

// ============================================================================
// aj[`cols;t1;t2]
// ============================================================================
std::shared_ptr<QNode> QParser::parse_aj() {
    auto node = std::make_shared<QNode>(QNodeType::AJ);
    expect(QTokenType::AJ);
    expect(QTokenType::LBRACKET);

    node->add_child(parse_symbol_list());
    expect(QTokenType::SEMICOLON);

    if (current().type == QTokenType::IDENTIFIER) {
        node->add_child(std::make_shared<QNode>(QNodeType::FROM, current().value));
        advance();
    }
    expect(QTokenType::SEMICOLON);

    if (current().type == QTokenType::IDENTIFIER) {
        node->add_child(std::make_shared<QNode>(QNodeType::FROM, current().value));
        advance();
    }
    expect(QTokenType::RBRACKET);
    return node;
}

// ============================================================================
// wj[w;`cols;t1;(t2;(agg;`col)...)]
// ============================================================================
std::shared_ptr<QNode> QParser::parse_wj() {
    auto node = std::make_shared<QNode>(QNodeType::WJ);
    expect(QTokenType::WJ);
    expect(QTokenType::LBRACKET);

    // Window spec
    node->add_child(parse_expression());
    expect(QTokenType::SEMICOLON);

    // Join columns
    node->add_child(parse_symbol_list());
    expect(QTokenType::SEMICOLON);

    // Table 1
    if (current().type == QTokenType::IDENTIFIER) {
        node->add_child(std::make_shared<QNode>(QNodeType::FROM, current().value));
        advance();
    }
    expect(QTokenType::SEMICOLON);

    // (table2;(agg;`col)...) — parse as list
    node->add_child(parse_list());

    expect(QTokenType::RBRACKET);
    return node;
}

// ============================================================================
// {[args] body} — function definition
// ============================================================================
std::shared_ptr<QNode> QParser::parse_function_def() {
    auto node = std::make_shared<QNode>(QNodeType::FUNCTION_DEF);
    expect(QTokenType::LBRACE);

    // Optional parameter list: [x;y;z]
    if (current().type == QTokenType::LBRACKET) {
        advance();
        while (current().type != QTokenType::RBRACKET &&
               current().type != QTokenType::END_OF_FILE) {
            if (current().type == QTokenType::IDENTIFIER) {
                node->add_child(std::make_shared<QNode>(QNodeType::COLUMN, current().value));
                advance();
            }
            match(QTokenType::SEMICOLON);
        }
        expect(QTokenType::RBRACKET);
    }

    // Body: statements separated by semicolons until }
    auto body = std::make_shared<QNode>(QNodeType::BLOCK);
    while (current().type != QTokenType::RBRACE &&
           current().type != QTokenType::END_OF_FILE) {
        body->add_child(parse_expression());
        match(QTokenType::SEMICOLON);
    }
    node->add_child(body);

    expect(QTokenType::RBRACE);
    return node;
}

// ============================================================================
// $[cond;true_expr;false_expr]
// ============================================================================
std::shared_ptr<QNode> QParser::parse_cond() {
    auto node = std::make_shared<QNode>(QNodeType::COND);
    expect(QTokenType::DOLLAR);
    expect(QTokenType::LBRACKET);

    node->add_child(parse_expression()); // condition
    expect(QTokenType::SEMICOLON);
    node->add_child(parse_expression()); // true branch
    expect(QTokenType::SEMICOLON);
    node->add_child(parse_expression()); // false branch

    expect(QTokenType::RBRACKET);
    return node;
}

// ============================================================================
// (expr;expr;...) — list/tuple
// ============================================================================
std::shared_ptr<QNode> QParser::parse_list() {
    auto node = std::make_shared<QNode>(QNodeType::LIST);
    expect(QTokenType::LPAREN);

    while (current().type != QTokenType::RPAREN &&
           current().type != QTokenType::END_OF_FILE) {
        node->add_child(parse_expression());
        if (!match(QTokenType::SEMICOLON)) break;
    }
    expect(QTokenType::RPAREN);
    return node;
}

// ============================================================================
// Expression — handles comparison, in, within, like, and/or, assignment
// ============================================================================
std::shared_ptr<QNode> QParser::parse_expression() {
    // Function definition
    if (current().type == QTokenType::LBRACE)
        return parse_function_def();

    // $[cond;t;f]
    if (current().type == QTokenType::DOLLAR && peek().type == QTokenType::LBRACKET)
        return parse_cond();

    // Bracket function call: func[args]
    if (current().type == QTokenType::IDENTIFIER && peek().type == QTokenType::LBRACKET)
        return parse_function_call();

    // q-style prefix/infix function
    if (current().type == QTokenType::IDENTIFIER) {
        std::string name = current().value;

        // Infix: <arg1> <func> <arg2> (e.g. size wavg price)
        if (peek().type == QTokenType::IDENTIFIER && is_q_function(peek().value)) {
            auto arg1 = parse_primary();
            std::string func_name = current().value;
            advance();
            auto func_node = std::make_shared<QNode>(QNodeType::FUNCTION_CALL, func_name);
            func_node->add_child(arg1);
            func_node->add_child(parse_additive());
            return func_node;
        }

        // Prefix: <func> <arg> (e.g. sum size, count i, neg price)
        if (is_q_function(name)) {
            // But only if next token looks like an argument
            auto next = peek().type;
            if (next == QTokenType::IDENTIFIER || next == QTokenType::NUMBER ||
                next == QTokenType::LPAREN || next == QTokenType::SYMBOL) {
                advance();
                auto func_node = std::make_shared<QNode>(QNodeType::FUNCTION_CALL, name);
                func_node->add_child(parse_additive());

                // Check for 'each': sum each list
                if (current().type == QTokenType::EACH) {
                    advance();
                    auto each_node = std::make_shared<QNode>(QNodeType::EACH);
                    each_node->add_child(func_node);
                    each_node->add_child(parse_additive());
                    return each_node;
                }
                return func_node;
            }
        }
    }

    // Additive expression (handles +, -, *, %)
    auto left = parse_additive();

    // Assignment: identifier : expression
    if (current().type == QTokenType::COLON && left->type == QNodeType::COLUMN) {
        advance();
        auto assign = std::make_shared<QNode>(QNodeType::ASSIGN, left->value);
        assign->add_child(parse_expression());
        return assign;
    }

    // Comparison operators
    if (current().type == QTokenType::EQ || current().type == QTokenType::LT ||
        current().type == QTokenType::GT || current().type == QTokenType::LE ||
        current().type == QTokenType::GE || current().type == QTokenType::NE ||
        current().type == QTokenType::TILDE) {
        std::string op = current().value;
        advance();
        auto op_node = std::make_shared<QNode>(QNodeType::BINARY_OP, op);
        op_node->add_child(left);
        op_node->add_child(parse_additive());
        return op_node;
    }

    // x in `a`b`c
    if (current().type == QTokenType::IN) {
        advance();
        auto in_node = std::make_shared<QNode>(QNodeType::IN_EXPR);
        in_node->add_child(left);
        // Collect symbol list or expression
        if (current().type == QTokenType::SYMBOL) {
            in_node->add_child(parse_symbol_list());
        } else {
            in_node->add_child(parse_additive());
        }
        return in_node;
    }

    // x within (lo;hi)
    if (current().type == QTokenType::WITHIN) {
        advance();
        auto within_node = std::make_shared<QNode>(QNodeType::WITHIN_EXPR);
        within_node->add_child(left);
        within_node->add_child(parse_list());
        return within_node;
    }

    // x like "pattern"
    if (current().type == QTokenType::LIKE) {
        advance();
        auto like_node = std::make_shared<QNode>(QNodeType::LIKE_EXPR);
        like_node->add_child(left);
        like_node->add_child(parse_primary());
        return like_node;
    }

    // and / or
    if (current().type == QTokenType::AND || current().type == QTokenType::OR) {
        std::string op = current().value;
        advance();
        auto op_node = std::make_shared<QNode>(QNodeType::BINARY_OP, op);
        op_node->add_child(left);
        op_node->add_child(parse_expression());
        return op_node;
    }

    return left;
}

// ============================================================================
// Additive: handles +, -, *, % (q divide)
// ============================================================================
std::shared_ptr<QNode> QParser::parse_additive() {
    auto left = parse_primary();

    while (current().type == QTokenType::PLUS || current().type == QTokenType::MINUS ||
           current().type == QTokenType::STAR || current().type == QTokenType::PERCENT) {
        std::string op = current().value;
        if (op == "%") op = "/";  // q % → Python /
        advance();
        auto op_node = std::make_shared<QNode>(QNodeType::BINARY_OP, op);
        op_node->add_child(left);
        op_node->add_child(parse_primary());
        left = op_node;
    }

    // Dot accessor: expr.field (e.g. time.minute)
    if (current().type == QTokenType::IDENTIFIER &&
        left->type == QNodeType::COLUMN &&
        !left->value.empty()) {
        // Check if this looks like a dot access that was split
        // Actually in q, time.minute is lexed as one identifier
    }

    return left;
}

// ============================================================================
// Primary: atoms, parens, lists, symbols
// ============================================================================
std::shared_ptr<QNode> QParser::parse_primary() {
    // Parenthesized expression or list
    if (current().type == QTokenType::LPAREN) {
        // Check if it's a list (contains semicolons)
        // Peek ahead to decide
        size_t saved = pos_;
        advance(); // skip (
        int depth = 1;
        bool has_semi = false;
        while (depth > 0 && pos_ < tokens_.size()) {
            if (current().type == QTokenType::LPAREN) depth++;
            else if (current().type == QTokenType::RPAREN) depth--;
            else if (current().type == QTokenType::SEMICOLON && depth == 1) has_semi = true;
            if (depth > 0) advance();
        }
        pos_ = saved; // restore

        if (has_semi) {
            return parse_list();
        } else {
            advance(); // skip (
            auto expr = parse_expression();
            expect(QTokenType::RPAREN);
            return expr;
        }
    }

    // Function definition
    if (current().type == QTokenType::LBRACE)
        return parse_function_def();

    // Bracket function call
    if (current().type == QTokenType::IDENTIFIER && peek().type == QTokenType::LBRACKET)
        return parse_function_call();

    // Symbol literal
    if (current().type == QTokenType::SYMBOL)
        return parse_literal();

    // Number
    if (current().type == QTokenType::NUMBER)
        return parse_literal();

    // String
    if (current().type == QTokenType::STRING)
        return parse_literal();

    // Date
    if (current().type == QTokenType::DATE)
        return parse_literal();

    // Identifier (column or variable)
    if (current().type == QTokenType::IDENTIFIER) {
        auto col = parse_column();
        // Index access: col[expr]
        if (current().type == QTokenType::LBRACKET) {
            advance();
            auto idx = std::make_shared<QNode>(QNodeType::INDEX, col->value);
            idx->add_child(parse_expression());
            expect(QTokenType::RBRACKET);
            return idx;
        }
        return col;
    }

    // NOT prefix
    if (current().type == QTokenType::NOT) {
        advance();
        auto unary = std::make_shared<QNode>(QNodeType::UNARY_OP, "not");
        unary->add_child(parse_primary());
        return unary;
    }

    // Negative number already handled in lexer, but unary minus on expr
    if (current().type == QTokenType::MINUS) {
        advance();
        auto unary = std::make_shared<QNode>(QNodeType::UNARY_OP, "-");
        unary->add_child(parse_primary());
        return unary;
    }

    // Fallback: return empty column
    auto node = std::make_shared<QNode>(QNodeType::COLUMN, current().value);
    advance();
    return node;
}

// ============================================================================
// func[arg1;arg2;...]
// ============================================================================
std::shared_ptr<QNode> QParser::parse_function_call() {
    auto node = std::make_shared<QNode>(QNodeType::FUNCTION_CALL, current().value);
    advance(); // function name
    expect(QTokenType::LBRACKET);

    while (current().type != QTokenType::RBRACKET &&
           current().type != QTokenType::END_OF_FILE) {
        node->add_child(parse_expression());
        if (!match(QTokenType::SEMICOLON) && !match(QTokenType::COMMA)) break;
    }
    expect(QTokenType::RBRACKET);
    return node;
}

// ============================================================================
// Atoms
// ============================================================================
std::shared_ptr<QNode> QParser::parse_column() {
    auto node = std::make_shared<QNode>(QNodeType::COLUMN, current().value);
    advance();
    return node;
}

std::shared_ptr<QNode> QParser::parse_literal() {
    auto node = std::make_shared<QNode>(QNodeType::LITERAL, current().value);
    advance();
    return node;
}

std::shared_ptr<QNode> QParser::parse_symbol_list() {
    auto node = std::make_shared<QNode>(QNodeType::SYMBOL_LIST);
    while (current().type == QTokenType::SYMBOL) {
        node->add_child(std::make_shared<QNode>(QNodeType::COLUMN, current().value));
        advance();
    }
    return node;
}

} // namespace zeptodb::migration
