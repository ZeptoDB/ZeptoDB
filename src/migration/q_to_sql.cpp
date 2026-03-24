// ============================================================================
// ZeptoDB: q to SQL Transformer Implementation (Extended)
// ============================================================================
#include "zeptodb/migration/q_parser.h"
#include <sstream>
#include <algorithm>

namespace zeptodb::migration {

QToSQLTransformer::QToSQLTransformer() {
    init_function_map();
}

void QToSQLTransformer::init_function_map() {
    function_map_["wavg"] = "VWAP";
    function_map_["sum"] = "SUM";
    function_map_["avg"] = "AVG";
    function_map_["min"] = "MIN";
    function_map_["max"] = "MAX";
    function_map_["count"] = "COUNT";
    function_map_["first"] = "FIRST";
    function_map_["last"] = "LAST";
    function_map_["xbar"] = "xbar";
    function_map_["ema"] = "ema";
    function_map_["mavg"] = "AVG";
    function_map_["msum"] = "SUM";
    function_map_["mmin"] = "MIN";
    function_map_["mmax"] = "MAX";
    function_map_["deltas"] = "DELTA";
    function_map_["ratios"] = "RATIO";
    function_map_["prev"] = "LAG";
    function_map_["next"] = "LEAD";
    function_map_["med"] = "MEDIAN";
    function_map_["var"] = "VARIANCE";
    function_map_["dev"] = "STDDEV";
    function_map_["sdev"] = "STDDEV";
    function_map_["abs"] = "ABS";
    function_map_["sqrt"] = "SQRT";
    function_map_["floor"] = "FLOOR";
    function_map_["ceiling"] = "CEILING";
    function_map_["log"] = "LOG";
    function_map_["exp"] = "EXP";
    function_map_["distinct"] = "DISTINCT";
    function_map_["neg"] = "-";
}

std::string QToSQLTransformer::transform(std::shared_ptr<QNode> q_ast) {
    if (!q_ast) return "";

    switch (q_ast->type) {
        case QNodeType::SELECT: return transform_select(q_ast);
        case QNodeType::UPDATE: {
            // update col:expr from table where cond
            std::ostringstream sql;
            sql << "UPDATE ";
            std::string table, where_clause;
            std::vector<std::shared_ptr<QNode>> assigns;
            for (const auto& c : q_ast->children) {
                if (c->type == QNodeType::FROM) table = c->value;
                else if (c->type == QNodeType::WHERE) where_clause = transform_where(c);
                else assigns.push_back(c);
            }
            sql << table << " SET ";
            bool first = true;
            for (const auto& a : assigns) {
                if (!first) sql << ", ";
                if (a->type == QNodeType::ASSIGN) {
                    sql << a->value << " = " << transform_expression(a->children[0]);
                } else {
                    sql << transform_expression(a);
                }
                first = false;
            }
            if (!where_clause.empty()) sql << " " << where_clause;
            return sql.str();
        }
        case QNodeType::DELETE: {
            std::ostringstream sql;
            sql << "DELETE";
            std::string table, where_clause;
            for (const auto& c : q_ast->children) {
                if (c->type == QNodeType::FROM) table = c->value;
                else if (c->type == QNodeType::WHERE) where_clause = transform_where(c);
            }
            sql << " FROM " << table;
            if (!where_clause.empty()) sql << " " << where_clause;
            return sql.str();
        }
        case QNodeType::EXEC: {
            // exec → SELECT (single column result)
            auto copy = std::make_shared<QNode>(QNodeType::SELECT);
            copy->children = q_ast->children;
            return transform_select(copy);
        }
        default:
            return transform_expression(q_ast);
    }
}

std::string QToSQLTransformer::transform_select(std::shared_ptr<QNode> node) {
    std::ostringstream sql;
    sql << "SELECT ";

    bool has_select_cols = false;
    std::string from_table, where_clause, group_by_clause;

    for (const auto& child : node->children) {
        if (child->type == QNodeType::FROM) {
            from_table = child->value;
        } else if (child->type == QNodeType::WHERE) {
            where_clause = transform_where(child);
        } else if (child->type == QNodeType::BY) {
            group_by_clause = transform_by(child);
        } else if (child->type == QNodeType::AJ) {
            return transform_aj(child);
        } else if (child->type == QNodeType::WJ) {
            return transform_wj(child);
        } else {
            if (has_select_cols) sql << ", ";
            sql << transform_expression(child);
            has_select_cols = true;
        }
    }

    if (!has_select_cols) sql << "*";
    if (!from_table.empty()) sql << " FROM " << from_table;
    if (!where_clause.empty()) sql << " " << where_clause;
    if (!group_by_clause.empty()) sql << " " << group_by_clause;

    return sql.str();
}

std::string QToSQLTransformer::transform_where(std::shared_ptr<QNode> node) {
    std::ostringstream sql;
    sql << "WHERE ";

    bool first = true;
    for (const auto& child : node->children) {
        if (child->type == QNodeType::FBY) continue;
        if (!first) sql << " AND ";
        sql << transform_expression(child);
        first = false;
    }
    return sql.str();
}

std::string QToSQLTransformer::transform_by(std::shared_ptr<QNode> node) {
    std::ostringstream sql;
    sql << "GROUP BY ";
    bool first = true;
    for (const auto& child : node->children) {
        if (!first) sql << ", ";
        if (child->type == QNodeType::COLUMN)
            sql << child->value;
        else
            sql << transform_expression(child);
        first = false;
    }
    return sql.str();
}

std::string QToSQLTransformer::transform_fby(std::shared_ptr<QNode> node) {
    return "PARTITION BY " + node->value;
}

std::string QToSQLTransformer::transform_aj(std::shared_ptr<QNode> node) {
    if (node->children.size() < 3) return "-- aj parsing error";
    std::ostringstream sql;
    auto join_cols = node->children[0];
    auto table1 = node->children[1];
    auto table2 = node->children[2];

    sql << "SELECT * FROM " << table1->value << "\n"
        << "ASOF JOIN " << table2->value << "\nON ";
    bool first = true;
    for (const auto& col : join_cols->children) {
        if (!first) sql << " AND ";
        sql << table1->value << "." << col->value
            << " = " << table2->value << "." << col->value;
        first = false;
    }
    return sql.str();
}

std::string QToSQLTransformer::transform_wj(std::shared_ptr<QNode> node) {
    std::ostringstream sql;
    sql << "-- Window JOIN (wj) — requires manual review\n";
    if (node->children.size() >= 4) {
        auto table1 = node->children[2];
        sql << "SELECT * FROM " << table1->value << "\n";
        sql << "WINDOW JOIN ...";
    }
    return sql.str();
}

std::string QToSQLTransformer::transform_function(std::shared_ptr<QNode> node) {
    std::string func_name = node->value;
    auto it = function_map_.find(func_name);
    if (it != function_map_.end()) func_name = it->second;

    // wavg → VWAP expansion
    if (node->value == "wavg" && node->children.size() == 2) {
        return "(SUM(" + transform_expression(node->children[0]) +
               " * " + transform_expression(node->children[1]) +
               ") / SUM(" + transform_expression(node->children[0]) + "))";
    }

    // xbar
    if (node->value == "xbar" && node->children.size() == 2) {
        return "xbar(" + transform_expression(node->children[1]) +
               ", " + transform_expression(node->children[0]) + ")";
    }

    // prev/next → LAG/LEAD
    if ((node->value == "prev" || node->value == "next") && node->children.size() == 1) {
        return func_name + "(" + transform_expression(node->children[0]) + ", 1) OVER ()";
    }

    // neg → unary minus
    if (node->value == "neg" && node->children.size() == 1) {
        return "-(" + transform_expression(node->children[0]) + ")";
    }

    // distinct → DISTINCT keyword
    if (node->value == "distinct" && node->children.size() == 1) {
        return "DISTINCT " + transform_expression(node->children[0]);
    }

    // Generic: FUNC(args)
    std::ostringstream sql;
    sql << func_name << "(";
    bool first = true;
    for (const auto& arg : node->children) {
        if (!first) sql << ", ";
        sql << transform_expression(arg);
        first = false;
    }
    sql << ")";
    return sql.str();
}

std::string QToSQLTransformer::transform_expression(std::shared_ptr<QNode> node) {
    if (!node) return "";

    switch (node->type) {
        case QNodeType::COLUMN:
            return node->value;

        case QNodeType::LITERAL: {
            bool numeric = !node->value.empty();
            for (char c : node->value)
                if (!std::isdigit(c) && c != '.' && c != '-') { numeric = false; break; }
            if (numeric) return node->value;
            return "'" + node->value + "'";
        }

        case QNodeType::FUNCTION_CALL:
            return transform_function(node);

        case QNodeType::BINARY_OP: {
            if (node->children.size() < 2) return "";
            std::string op = node->value;
            if (op == "and") op = "AND";
            else if (op == "or") op = "OR";
            return transform_expression(node->children[0]) +
                   " " + op + " " +
                   transform_expression(node->children[1]);
        }

        case QNodeType::UNARY_OP: {
            if (node->children.empty()) return "";
            if (node->value == "not")
                return "NOT " + transform_expression(node->children[0]);
            return node->value + "(" + transform_expression(node->children[0]) + ")";
        }

        case QNodeType::IN_EXPR: {
            if (node->children.size() < 2) return "";
            std::ostringstream sql;
            sql << transform_expression(node->children[0]) << " IN (";
            auto list = node->children[1];
            bool first = true;
            for (const auto& item : list->children) {
                if (!first) sql << ", ";
                sql << "'" << item->value << "'";
                first = false;
            }
            sql << ")";
            return sql.str();
        }

        case QNodeType::WITHIN_EXPR: {
            if (node->children.size() < 2) return "";
            auto col = transform_expression(node->children[0]);
            auto range = node->children[1];
            if (range->children.size() >= 2) {
                return col + " BETWEEN " +
                       transform_expression(range->children[0]) + " AND " +
                       transform_expression(range->children[1]);
            }
            return "";
        }

        case QNodeType::LIKE_EXPR: {
            if (node->children.size() < 2) return "";
            return transform_expression(node->children[0]) +
                   " LIKE '" + node->children[1]->value + "'";
        }

        case QNodeType::ASSIGN: {
            // In SQL context, assignment becomes alias
            if (!node->children.empty())
                return transform_expression(node->children[0]) + " AS " + node->value;
            return "";
        }

        case QNodeType::COND: {
            if (node->children.size() < 3) return "";
            return "CASE WHEN " + transform_expression(node->children[0]) +
                   " THEN " + transform_expression(node->children[1]) +
                   " ELSE " + transform_expression(node->children[2]) + " END";
        }

        case QNodeType::INDEX: {
            if (!node->children.empty())
                return node->value + "[" + transform_expression(node->children[0]) + "]";
            return node->value;
        }

        default:
            return "";
    }
}

} // namespace zeptodb::migration
