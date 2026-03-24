// ============================================================================
// ZeptoDB: q to Python Transformer Implementation (Extended)
// ============================================================================
#include "zeptodb/migration/q_to_python.h"
#include <sstream>
#include <algorithm>

namespace zeptodb::migration {

QToPythonTransformer::QToPythonTransformer() {
    init_func_map();
}

void QToPythonTransformer::init_func_map() {
    func_map_["sum"]    = "sum";
    func_map_["avg"]    = "mean";
    func_map_["min"]    = "min";
    func_map_["max"]    = "max";
    func_map_["count"]  = "count";
    func_map_["first"]  = "first";
    func_map_["last"]   = "last";
    func_map_["wavg"]   = "vwap";
    func_map_["xbar"]   = "xbar";
    func_map_["ema"]    = "ema";
    func_map_["mavg"]   = "rolling_mean";
    func_map_["msum"]   = "rolling_sum";
    func_map_["mmin"]   = "rolling_min";
    func_map_["mmax"]   = "rolling_max";
    func_map_["deltas"] = "delta";
    func_map_["ratios"] = "ratio";
    func_map_["sums"]   = "cumsum";
    func_map_["prds"]   = "cumprod";
    func_map_["prev"]   = "shift";
    func_map_["next"]   = "shift_neg";
    func_map_["med"]    = "median";
    func_map_["var"]    = "var";
    func_map_["dev"]    = "std";
    func_map_["sdev"]   = "std";
    func_map_["abs"]    = "abs";
    func_map_["sqrt"]   = "sqrt";
    func_map_["floor"]  = "floor";
    func_map_["ceiling"]= "ceil";
    func_map_["neg"]    = "neg";
    func_map_["log"]    = "log";
    func_map_["exp"]    = "exp";
    func_map_["distinct"]= "unique";
    func_map_["asc"]    = "sort";
    func_map_["desc"]   = "sort_desc";
    func_map_["reverse"]= "reverse";
    func_map_["til"]    = "range";
    func_map_["string"] = "str";
    func_map_["fills"]  = "ffill";
    func_map_["null"]   = "is_null";
    func_map_["rank"]   = "rank";
    func_map_["raze"]   = "flatten";
    func_map_["enlist"] = "list";
    func_map_["type"]   = "type";
    func_map_["signum"] = "sign";
    func_map_["reciprocal"] = "reciprocal";
}

std::string QToPythonTransformer::ind() const {
    return std::string(indent_ * 4, ' ');
}

// ============================================================================
// Multi-line script transform
// ============================================================================
std::string QToPythonTransformer::transform_script(const std::string& q_script) {
    std::ostringstream out;
    out << "import zepto_py as apex\n";
    out << "from zepto_py.dsl import DataFrame\n\n";
    out << "db = zeptodb.connect()\n\n";

    std::istringstream stream(q_script);
    std::string line;

    while (std::getline(stream, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Comments
        if (line[0] == '/') {
            std::string comment = line.substr(1);
            size_t cs = comment.find_first_not_of(" \t");
            if (cs != std::string::npos) comment = comment.substr(cs);
            out << "# " << comment << "\n";
            continue;
        }

        // q system commands (\l, \d, \p, etc.)
        if (line[0] == '\\') {
            out << "# TODO: " << line << "\n";
            continue;
        }
        if (line.empty()) { out << "\n"; continue; }

        // Variable assignment: varname: <q expression>
        std::string var_name;
        std::string q_expr = line;
        auto colon_pos = line.find(':');
        if (colon_pos != std::string::npos && colon_pos > 0 &&
            line[colon_pos - 1] != '<' && line[colon_pos - 1] != '>' &&
            (colon_pos + 1 >= line.size() || line[colon_pos + 1] != ':')) {
            std::string lhs = line.substr(0, colon_pos);
            size_t end = lhs.find_last_not_of(" \t");
            if (end != std::string::npos) lhs = lhs.substr(0, end + 1);
            bool is_ident = !lhs.empty() && (std::isalpha(lhs[0]) || lhs[0] == '_');
            for (size_t i = 1; is_ident && i < lhs.size(); ++i)
                is_ident = std::isalnum(lhs[i]) || lhs[i] == '_';

            if (is_ident) {
                var_name = lhs;
                q_expr = line.substr(colon_pos + 1);
                start = q_expr.find_first_not_of(" \t");
                if (start != std::string::npos) q_expr = q_expr.substr(start);
            }
        }

        try {
            QLexer lexer(q_expr);
            auto tokens = lexer.tokenize();
            QParser parser(tokens);
            auto ast = parser.parse();
            std::string py = transform(ast);

            if (!var_name.empty())
                out << var_name << " = " << py << "\n";
            else
                out << py << "\n";
        } catch (const std::exception&) {
            out << "# TODO: " << line << "\n";
        }
    }

    return out.str();
}

// ============================================================================
// Single AST transform
// ============================================================================
std::string QToPythonTransformer::transform(std::shared_ptr<QNode> q_ast) {
    if (!q_ast) return "";

    switch (q_ast->type) {
        case QNodeType::SELECT: return transform_select(q_ast);

        case QNodeType::UPDATE: {
            // update col:expr from table where cond
            std::ostringstream py;
            std::string table;
            std::shared_ptr<QNode> where_node;
            std::vector<std::shared_ptr<QNode>> assigns;
            for (const auto& c : q_ast->children) {
                if (c->type == QNodeType::FROM) table = c->value;
                else if (c->type == QNodeType::WHERE) where_node = c;
                else assigns.push_back(c);
            }
            if (table.empty()) table = "trades";
            py << "DataFrame(db, \"" << table << "\")";
            if (where_node) py << ".filter(" << transform_where(where_node) << ")";
            py << ".with_columns(";
            bool first = true;
            for (const auto& a : assigns) {
                if (!first) py << ", ";
                if (a->type == QNodeType::ASSIGN) {
                    py << a->value << "=" << transform_expression(a->children[0]);
                } else {
                    py << transform_expression(a);
                }
                first = false;
            }
            py << ")";
            return py.str();
        }

        case QNodeType::DELETE: {
            std::ostringstream py;
            std::string table;
            std::shared_ptr<QNode> where_node;
            for (const auto& c : q_ast->children) {
                if (c->type == QNodeType::FROM) table = c->value;
                else if (c->type == QNodeType::WHERE) where_node = c;
            }
            if (table.empty()) table = "trades";
            py << "DataFrame(db, \"" << table << "\")";
            if (where_node) py << ".filter(~(" << transform_where(where_node) << "))";
            return py.str();
        }

        case QNodeType::EXEC: {
            auto copy = std::make_shared<QNode>(QNodeType::SELECT);
            copy->children = q_ast->children;
            return transform_select(copy) + ".to_series()";
        }

        case QNodeType::FUNCTION_DEF: {
            std::ostringstream py;
            // Collect params
            std::vector<std::string> params;
            std::shared_ptr<QNode> body;
            for (const auto& c : q_ast->children) {
                if (c->type == QNodeType::COLUMN) params.push_back(c->value);
                else if (c->type == QNodeType::BLOCK) body = c;
            }
            py << "lambda";
            if (!params.empty()) {
                py << " ";
                bool first = true;
                for (const auto& p : params) {
                    if (!first) py << ", ";
                    py << p;
                    first = false;
                }
            }
            py << ": ";
            if (body && !body->children.empty()) {
                py << transform(body->children.back());
            }
            return py.str();
        }

        case QNodeType::ASSIGN: {
            if (!q_ast->children.empty())
                return q_ast->value + " = " + transform(q_ast->children[0]);
            return "";
        }

        default:
            return transform_expression(q_ast);
    }
}

// ============================================================================
// SELECT
// ============================================================================
std::string QToPythonTransformer::transform_select(std::shared_ptr<QNode> node) {
    std::ostringstream py;

    std::string table;
    std::shared_ptr<QNode> where_node, by_node, aj_node;
    std::vector<std::shared_ptr<QNode>> select_exprs;

    for (const auto& child : node->children) {
        switch (child->type) {
            case QNodeType::FROM:  table = child->value; break;
            case QNodeType::WHERE: where_node = child; break;
            case QNodeType::BY:    by_node = child; break;
            case QNodeType::AJ:    aj_node = child; break;
            default:               select_exprs.push_back(child); break;
        }
    }

    if (aj_node) return transform_aj(aj_node);

    if (table.empty()) table = "trades";
    py << "DataFrame(db, \"" << table << "\")";

    if (where_node)
        py << ".filter(" << transform_where(where_node) << ")";

    if (by_node) {
        py << ".group_by(";
        bool first = true;
        for (const auto& col : by_node->children) {
            if (!first) py << ", ";
            if (col->type == QNodeType::COLUMN)
                py << "\"" << col->value << "\"";
            else
                py << transform_expression(col);
            first = false;
        }
        py << ")";

        if (!select_exprs.empty()) {
            py << ".agg(";
            bool first_agg = true;
            for (const auto& expr : select_exprs) {
                if (!first_agg) py << ", ";
                if (expr->type == QNodeType::FUNCTION_CALL && !expr->children.empty()) {
                    std::string func = expr->value;
                    auto it = func_map_.find(func);
                    std::string py_func = (it != func_map_.end()) ? it->second : func;
                    std::string col = transform_expression(expr->children[0]);
                    py << col << "_" << py_func << "=(\"" << col << "\", \"" << py_func << "\")";
                } else if (expr->type == QNodeType::ASSIGN && !expr->children.empty()) {
                    py << expr->value << "=" << transform_expression(expr->children[0]);
                } else {
                    py << transform_expression(expr);
                }
                first_agg = false;
            }
            py << ")";
        }
    } else if (!select_exprs.empty()) {
        py << ".select(";
        bool first = true;
        for (const auto& expr : select_exprs) {
            if (!first) py << ", ";
            if (expr->type == QNodeType::FUNCTION_CALL)
                py << transform_expression(expr);
            else if (expr->type == QNodeType::COLUMN)
                py << "\"" << expr->value << "\"";
            else
                py << transform_expression(expr);
            first = false;
        }
        py << ")";
    }

    return py.str();
}

std::string QToPythonTransformer::transform_where(std::shared_ptr<QNode> node) {
    if (node->children.empty()) return "";

    // Multiple conditions → joined with &
    if (node->children.size() == 1)
        return transform_expression(node->children[0]);

    std::ostringstream py;
    bool first = true;
    for (const auto& child : node->children) {
        if (child->type == QNodeType::FBY) continue;
        if (!first) py << " & ";
        py << "(" << transform_expression(child) << ")";
        first = false;
    }
    return py.str();
}

std::string QToPythonTransformer::transform_aj(std::shared_ptr<QNode> node) {
    if (node->children.size() < 3) return "# aj parsing error";
    std::ostringstream py;
    auto join_cols = node->children[0];
    auto table1 = node->children[1];
    auto table2 = node->children[2];

    py << "DataFrame(db, \"" << table1->value << "\").asof_join("
       << "DataFrame(db, \"" << table2->value << "\"), on=[";
    bool first = true;
    for (const auto& col : join_cols->children) {
        if (!first) py << ", ";
        py << "\"" << col->value << "\"";
        first = false;
    }
    py << "])";
    return py.str();
}

// ============================================================================
// Function transform
// ============================================================================
std::string QToPythonTransformer::transform_function(std::shared_ptr<QNode> node) {
    std::string func = node->value;
    auto it = func_map_.find(func);
    std::string py_func = (it != func_map_.end()) ? it->second : func;

    // wavg: size wavg price → df.vwap("price", "size")
    if (func == "wavg" && node->children.size() == 2)
        return "df.vwap(\"" + transform_expression(node->children[1]) +
               "\", \"" + transform_expression(node->children[0]) + "\")";

    // xbar: xbar[300;timestamp] → df["timestamp"].xbar(300)
    if (func == "xbar" && node->children.size() == 2)
        return "df[\"" + transform_expression(node->children[1]) +
               "\"].xbar(" + transform_expression(node->children[0]) + ")";

    // ema: ema[20;price] → df["price"].ema(20)
    if (func == "ema" && node->children.size() == 2)
        return "df[\"" + transform_expression(node->children[1]) +
               "\"].ema(" + transform_expression(node->children[0]) + ")";

    // mavg/msum/mmin/mmax: mavg[20;price] → df["price"].rolling_mean(20)
    if ((func == "mavg" || func == "msum" || func == "mmin" || func == "mmax") &&
        node->children.size() == 2)
        return "df[\"" + transform_expression(node->children[1]) +
               "\"]." + py_func + "(" + transform_expression(node->children[0]) + ")";

    // prev/next: prev price → df["price"].shift(1)
    if (func == "prev" && node->children.size() == 1)
        return "df[\"" + transform_expression(node->children[0]) + "\"].shift(1)";
    if (func == "next" && node->children.size() == 1)
        return "df[\"" + transform_expression(node->children[0]) + "\"].shift(-1)";

    // til: til 10 → list(range(10))
    if (func == "til" && node->children.size() == 1)
        return "list(range(" + transform_expression(node->children[0]) + "))";

    // neg: neg price → -df["price"]
    if (func == "neg" && node->children.size() == 1)
        return "-df[\"" + transform_expression(node->children[0]) + "\"]";

    // Prefix aggregation: sum size → df["size"].sum()
    if (node->children.size() == 1) {
        std::string col = transform_expression(node->children[0]);
        return "df[\"" + col + "\"]." + py_func + "()";
    }

    // Generic
    std::ostringstream py;
    py << py_func << "(";
    bool first = true;
    for (const auto& arg : node->children) {
        if (!first) py << ", ";
        py << transform_expression(arg);
        first = false;
    }
    py << ")";
    return py.str();
}

// ============================================================================
// Expression transform
// ============================================================================
std::string QToPythonTransformer::transform_expression(std::shared_ptr<QNode> node) {
    if (!node) return "";

    switch (node->type) {
        case QNodeType::COLUMN:
            return node->value;

        case QNodeType::LITERAL: {
            bool numeric = !node->value.empty();
            for (char c : node->value)
                if (!std::isdigit(c) && c != '.' && c != '-') { numeric = false; break; }
            if (numeric) return node->value;
            return "\"" + node->value + "\"";
        }

        case QNodeType::FUNCTION_CALL:
            return transform_function(node);

        case QNodeType::BINARY_OP: {
            if (node->children.size() < 2) return "";
            std::string left = transform_expression(node->children[0]);
            std::string right = transform_expression(node->children[1]);
            std::string op = node->value;
            if (op == "=") op = "==";
            else if (op == "and") op = "&";
            else if (op == "or") op = "|";
            // If left looks like a column name (no quotes, no brackets)
            if (node->children[0]->type == QNodeType::COLUMN)
                return "df[\"" + left + "\"] " + op + " " + right;
            return left + " " + op + " " + right;
        }

        case QNodeType::UNARY_OP: {
            if (node->children.empty()) return "";
            if (node->value == "not")
                return "~(" + transform_expression(node->children[0]) + ")";
            return node->value + transform_expression(node->children[0]);
        }

        case QNodeType::IN_EXPR: {
            if (node->children.size() < 2) return "";
            std::string col = transform_expression(node->children[0]);
            auto list = node->children[1];
            std::ostringstream py;
            py << "df[\"" << col << "\"].is_in([";
            bool first = true;
            for (const auto& item : list->children) {
                if (!first) py << ", ";
                py << "\"" << item->value << "\"";
                first = false;
            }
            py << "])";
            return py.str();
        }

        case QNodeType::WITHIN_EXPR: {
            if (node->children.size() < 2) return "";
            std::string col = transform_expression(node->children[0]);
            auto range = node->children[1];
            if (range->children.size() >= 2)
                return "df[\"" + col + "\"].is_between(" +
                       transform_expression(range->children[0]) + ", " +
                       transform_expression(range->children[1]) + ")";
            return "";
        }

        case QNodeType::LIKE_EXPR: {
            if (node->children.size() < 2) return "";
            return "df[\"" + transform_expression(node->children[0]) +
                   "\"].str.contains(\"" + node->children[1]->value + "\")";
        }

        case QNodeType::COND: {
            if (node->children.size() < 3) return "";
            return transform_expression(node->children[1]) + " if " +
                   transform_expression(node->children[0]) + " else " +
                   transform_expression(node->children[2]);
        }

        case QNodeType::ASSIGN: {
            if (!node->children.empty())
                return node->value + "=" + transform_expression(node->children[0]);
            return "";
        }

        case QNodeType::LIST: {
            std::ostringstream py;
            py << "[";
            bool first = true;
            for (const auto& item : node->children) {
                if (!first) py << ", ";
                py << transform_expression(item);
                first = false;
            }
            py << "]";
            return py.str();
        }

        case QNodeType::EACH: {
            if (node->children.size() < 2) return "";
            return "[" + transform_expression(node->children[0]) +
                   " for x in " + transform_expression(node->children[1]) + "]";
        }

        case QNodeType::INDEX: {
            if (!node->children.empty())
                return node->value + "[" + transform_expression(node->children[0]) + "]";
            return node->value;
        }

        case QNodeType::FUNCTION_DEF: {
            std::ostringstream py;
            std::vector<std::string> params;
            std::shared_ptr<QNode> body;
            for (const auto& c : node->children) {
                if (c->type == QNodeType::COLUMN) params.push_back(c->value);
                else if (c->type == QNodeType::BLOCK) body = c;
            }
            py << "lambda";
            if (!params.empty()) {
                py << " ";
                bool first = true;
                for (const auto& p : params) {
                    if (!first) py << ", ";
                    py << p;
                    first = false;
                }
            }
            py << ": ";
            if (body && !body->children.empty())
                py << transform_expression(body->children.back());
            return py.str();
        }

        default:
            return "";
    }
}

} // namespace zeptodb::migration
