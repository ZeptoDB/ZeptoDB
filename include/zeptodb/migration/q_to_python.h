// ============================================================================
// ZeptoDB: q to Python Transformer
// ============================================================================
#pragma once

#include "zeptodb/migration/q_parser.h"
#include <string>
#include <unordered_map>

namespace zeptodb::migration {

class QToPythonTransformer {
public:
    QToPythonTransformer();

    // Transform single q AST → Python code
    std::string transform(std::shared_ptr<QNode> q_ast);

    // Transform multi-line q script → Python script
    std::string transform_script(const std::string& q_script);

private:
    std::string transform_select(std::shared_ptr<QNode> node);
    std::string transform_expression(std::shared_ptr<QNode> node);
    std::string transform_function(std::shared_ptr<QNode> node);
    std::string transform_where(std::shared_ptr<QNode> node);
    std::string transform_aj(std::shared_ptr<QNode> node);

    // q function → Python method mapping
    std::unordered_map<std::string, std::string> func_map_;
    void init_func_map();

    // Indentation
    int indent_ = 0;
    std::string ind() const;
};

} // namespace zeptodb::migration
