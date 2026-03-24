// ============================================================================
// ZeptoDB: kdb+ q Language Parser
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace zeptodb::migration {

// ============================================================================
// q Language AST (Abstract Syntax Tree)
// ============================================================================

enum class QNodeType {
    SELECT,
    UPDATE,
    DELETE,
    EXEC,
    WHERE,
    BY,
    FROM,
    FBY,           // filter by
    AJ,            // asof join
    WJ,            // window join
    FUNCTION_CALL,
    FUNCTION_DEF,  // {[args] body}
    COLUMN,
    LITERAL,
    BINARY_OP,
    UNARY_OP,
    ASSIGN,        // var: expr
    COND,          // $[cond;true;false]
    LIST,          // (a;b;c) or `a`b`c
    SYMBOL_LIST,
    IN_EXPR,       // x in `a`b`c
    WITHIN_EXPR,   // x within (lo;hi)
    LIKE_EXPR,     // x like "pattern"
    BLOCK,         // multiple statements
    EACH,          // func each list
    OVER,          // func over list (/)
    SCAN,          // func scan list (\)
    CAST,          // "T"$expr
    INDEX          // table[expr] or list[idx]
};

enum class QOperator {
    EQ,            // =
    NE,            // <>
    LT,            // <
    LE,            // <=
    GT,            // >
    GE,            // >=
    AND,
    OR,
    IN
};

struct QNode {
    QNodeType type;
    std::string value;
    std::vector<std::shared_ptr<QNode>> children;

    QNode(QNodeType t, const std::string& v = "")
        : type(t), value(v) {}

    void add_child(std::shared_ptr<QNode> child) {
        children.push_back(child);
    }
};

// ============================================================================
// q Language Lexer
// ============================================================================
enum class QTokenType {
    SELECT,
    FROM,
    WHERE,
    BY,
    FBY,
    AJ,
    WJ,
    EXEC,
    UPDATE,
    DELETE,
    IN,
    NOT,
    WITHIN,
    LIKE,
    EACH,
    IDENTIFIER,
    NUMBER,
    STRING,
    SYMBOL,        // `sym
    DATE,          // 2024.01.01
    TIME,          // 12:30:00
    TIMESPAN,      // 0D12:30:00.000000000
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    LBRACE,        // {
    RBRACE,        // }
    SEMICOLON,
    COMMA,
    COLON,         // :
    DCOLON,        // ::
    EQ,            // =
    LT,            // <
    GT,            // >
    LE,            // <=
    GE,            // >=
    NE,            // <>
    TILDE,         // ~ (match)
    BANG,          // ! (key, dict)
    HASH,          // # (take, count)
    AT,            // @ (apply)
    QUESTION,      // ? (find, rand)
    DOLLAR,        // $ (cast, cond)
    DOT,           // .
    PLUS,
    MINUS,
    STAR,          // *
    PERCENT,       // % (divide in q)
    AND,
    OR,
    BACKTICK,      // `
    END_OF_FILE
};

struct QToken {
    QTokenType type;
    std::string value;
    size_t line;
    size_t column;

    QToken(QTokenType t, const std::string& v = "", size_t l = 0, size_t c = 0)
        : type(t), value(v), line(l), column(c) {}
};

class QLexer {
public:
    explicit QLexer(const std::string& source);

    std::vector<QToken> tokenize();

private:
    std::string source_;
    size_t pos_;
    size_t line_;
    size_t column_;

    char current() const;
    char peek(size_t offset = 1) const;
    void advance();
    void skip_whitespace();
    void skip_comment();

    QToken read_number();
    QToken read_identifier();
    QToken read_string();
    QToken read_symbol();
    QToken read_date();
};

// ============================================================================
// q Language Parser
// ============================================================================
class QParser {
public:
    explicit QParser(const std::vector<QToken>& tokens);

    std::shared_ptr<QNode> parse();

private:
    std::vector<QToken> tokens_;
    size_t pos_;

    const QToken& current() const;
    const QToken& peek(size_t offset = 1) const;
    void advance();
    bool match(QTokenType type);
    bool expect(QTokenType type);

    // Parsing methods
    std::shared_ptr<QNode> parse_select();
    std::shared_ptr<QNode> parse_update();
    std::shared_ptr<QNode> parse_delete();
    std::shared_ptr<QNode> parse_exec();
    std::shared_ptr<QNode> parse_from();
    std::shared_ptr<QNode> parse_where();
    std::shared_ptr<QNode> parse_by();
    std::shared_ptr<QNode> parse_fby();
    std::shared_ptr<QNode> parse_aj();
    std::shared_ptr<QNode> parse_wj();
    std::shared_ptr<QNode> parse_expression();
    std::shared_ptr<QNode> parse_additive();
    std::shared_ptr<QNode> parse_primary();
    std::shared_ptr<QNode> parse_function_call();
    std::shared_ptr<QNode> parse_function_def();
    std::shared_ptr<QNode> parse_cond();
    std::shared_ptr<QNode> parse_list();
    std::shared_ptr<QNode> parse_column();
    std::shared_ptr<QNode> parse_literal();
    std::shared_ptr<QNode> parse_symbol_list();
};

// ============================================================================
// q to SQL Transformer
// ============================================================================
class QToSQLTransformer {
public:
    QToSQLTransformer();

    std::string transform(std::shared_ptr<QNode> q_ast);

private:
    std::string transform_select(std::shared_ptr<QNode> node);
    std::string transform_where(std::shared_ptr<QNode> node);
    std::string transform_by(std::shared_ptr<QNode> node);
    std::string transform_fby(std::shared_ptr<QNode> node);
    std::string transform_aj(std::shared_ptr<QNode> node);
    std::string transform_wj(std::shared_ptr<QNode> node);
    std::string transform_function(std::shared_ptr<QNode> node);
    std::string transform_expression(std::shared_ptr<QNode> node);

    // Function mapping (q → SQL)
    std::unordered_map<std::string, std::string> function_map_;

    void init_function_map();
};

} // namespace zeptodb::migration
