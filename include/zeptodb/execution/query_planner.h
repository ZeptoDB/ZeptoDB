#pragma once
// ============================================================================
// Layer 3: Query Planner — Logical → Physical Plan Translation
// ============================================================================
// 문서 근거: layer3_execution_engine.md §1
//   - Logical Query Plan → Physical Plan (DAG Operators)
//   - 파이프라인 최소화 최적화
// ============================================================================

#include "zeptodb/common/types.h"
#include <string>
#include <vector>
#include <memory>

namespace zeptodb::execution {

// Placeholder — JIT 및 플래너 확장은 Phase 2에서 구현
// 현재는 인터페이스 정의만

enum class OpType : uint8_t {
    SCAN,
    FILTER,
    PROJECT,
    AGGREGATE,
    SORT,
    LIMIT,
};

struct PlanNode {
    OpType type;
    std::string description;
    std::vector<std::shared_ptr<PlanNode>> children;
};

class QueryPlanner {
public:
    QueryPlanner() = default;
    // TODO: AST → Physical Plan 변환
};

} // namespace zeptodb::execution
