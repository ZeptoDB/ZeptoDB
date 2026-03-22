// ============================================================================
// APEX-DB: q to SQL Transformer Implementation
// ============================================================================
#include "apex/migration/q_parser.h"
#include <sstream>
#include <algorithm>

namespace apex::migration {

QToSQLTransformer::QToSQLTransformer() {
    init_function_map();
}

void QToSQLTransformer::init_function_map() {
    // q 함수 → SQL 함수 매핑
    function_map_["wavg"] = "VWAP";  // 가중 평균 → VWAP
    function_map_["sum"] = "SUM";
    function_map_["avg"] = "AVG";
    function_map_["min"] = "MIN";
    function_map_["max"] = "MAX";
    function_map_["count"] = "COUNT";
    function_map_["first"] = "FIRST";
    function_map_["last"] = "LAST";

    // 금융 함수
    function_map_["xbar"] = "xbar";   // APEX-DB native
    function_map_["ema"] = "ema";     // APEX-DB native
    function_map_["mavg"] = "AVG";    // moving avg → AVG OVER
    function_map_["msum"] = "SUM";    // moving sum → SUM OVER
    function_map_["mmin"] = "MIN";
    function_map_["mmax"] = "MAX";

    // 윈도우 함수
    function_map_["deltas"] = "DELTA";
    function_map_["ratios"] = "RATIO";
}

std::string QToSQLTransformer::transform(std::shared_ptr<QNode> q_ast) {
    if (!q_ast) return "";

    if (q_ast->type == QNodeType::SELECT) {
        return transform_select(q_ast);
    }

    return "";
}

std::string QToSQLTransformer::transform_select(std::shared_ptr<QNode> node) {
    std::ostringstream sql;

    sql << "SELECT ";

    // SELECT 절
    bool has_select_cols = false;
    std::string from_table;
    std::string where_clause;
    std::string group_by_clause;

    for (const auto& child : node->children) {
        if (child->type == QNodeType::FROM) {
            from_table = child->value;
        } else if (child->type == QNodeType::WHERE) {
            where_clause = transform_where(child);
        } else if (child->type == QNodeType::BY) {
            group_by_clause = transform_by(child);
        } else if (child->type == QNodeType::FBY) {
            // fby는 WHERE + WINDOW로 변환
        } else if (child->type == QNodeType::AJ) {
            return transform_aj(child);
        } else if (child->type == QNodeType::WJ) {
            return transform_wj(child);
        } else {
            // 일반 컬럼이나 표현식
            if (has_select_cols) sql << ", ";
            sql << transform_expression(child);
            has_select_cols = true;
        }
    }

    if (!has_select_cols) {
        sql << "*";  // select from trades (전체 선택)
    }

    // FROM 절
    if (!from_table.empty()) {
        sql << " FROM " << from_table;
    }

    // WHERE 절
    if (!where_clause.empty()) {
        sql << " " << where_clause;
    }

    // GROUP BY 절
    if (!group_by_clause.empty()) {
        sql << " " << group_by_clause;
    }

    return sql.str();
}

std::string QToSQLTransformer::transform_where(std::shared_ptr<QNode> node) {
    std::ostringstream sql;
    sql << "WHERE ";

    // WHERE 조건
    if (!node->children.empty()) {
        sql << transform_expression(node->children[0]);
    }

    return sql.str();
}

std::string QToSQLTransformer::transform_by(std::shared_ptr<QNode> node) {
    std::ostringstream sql;
    sql << "GROUP BY ";

    bool first = true;
    for (const auto& child : node->children) {
        if (!first) sql << ", ";
        sql << child->value;
        first = false;
    }

    return sql.str();
}

std::string QToSQLTransformer::transform_fby(std::shared_ptr<QNode> node) {
    // fby (filter by) → PARTITION BY 윈도우 함수
    std::ostringstream sql;
    sql << "PARTITION BY " << node->value;
    return sql.str();
}

std::string QToSQLTransformer::transform_aj(std::shared_ptr<QNode> node) {
    // aj[`time`sym;table1;table2] → ASOF JOIN
    std::ostringstream sql;

    if (node->children.size() < 3) {
        return "-- aj parsing error";
    }

    auto join_cols = node->children[0];  // symbol list
    auto table1 = node->children[1];
    auto table2 = node->children[2];

    sql << "SELECT * FROM " << table1->value << "\n";
    sql << "ASOF JOIN " << table2->value << "\n";
    sql << "ON ";

    // Join 컬럼들
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
    // wj[w;`sym`time;table1;(table2;(max;`bid);(min;`ask))]
    // → Window JOIN (APEX-DB native)

    std::ostringstream sql;
    sql << "-- Window JOIN conversion (complex, requires manual review)\n";
    sql << "-- Original wj structure preserved\n";

    return sql.str();
}

std::string QToSQLTransformer::transform_function(std::shared_ptr<QNode> node) {
    std::ostringstream sql;

    std::string func_name = node->value;

    // 함수 매핑
    auto it = function_map_.find(func_name);
    if (it != function_map_.end()) {
        func_name = it->second;
    }

    sql << func_name << "(";

    // Arguments
    bool first = true;
    for (const auto& arg : node->children) {
        if (!first) sql << ", ";
        sql << transform_expression(arg);
        first = false;
    }

    sql << ")";

    // 특수 처리: wavg (VWAP)
    if (node->value == "wavg" && node->children.size() == 2) {
        // size wavg price → SUM(size * price) / SUM(size)
        sql.str("");  // clear
        sql << "(SUM(" << transform_expression(node->children[0])
            << " * " << transform_expression(node->children[1])
            << ") / SUM(" << transform_expression(node->children[0]) << "))";
    }

    // xbar 특수 처리
    if (node->value == "xbar" && node->children.size() == 2) {
        // xbar[300;timestamp] → xbar(timestamp, 300)
        sql.str("");
        sql << "xbar(" << transform_expression(node->children[1])
            << ", " << transform_expression(node->children[0]) << ")";
    }

    return sql.str();
}

std::string QToSQLTransformer::transform_expression(std::shared_ptr<QNode> node) {
    if (!node) return "";

    switch (node->type) {
        case QNodeType::COLUMN:
            return node->value;

        case QNodeType::LITERAL:
            // 문자열은 따옴표 추가
            if (node->value.find_first_not_of("0123456789.-") != std::string::npos) {
                return "'" + node->value + "'";
            }
            return node->value;

        case QNodeType::FUNCTION_CALL:
            return transform_function(node);

        case QNodeType::BINARY_OP: {
            if (node->children.size() < 2) return "";

            std::ostringstream sql;
            sql << transform_expression(node->children[0])
                << " " << node->value << " "
                << transform_expression(node->children[1]);
            return sql.str();
        }

        default:
            return "";
    }
}

} // namespace apex::migration
