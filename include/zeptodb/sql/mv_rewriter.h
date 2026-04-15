#pragma once
// ============================================================================
// ZeptoDB: MV Query Rewriter
// ============================================================================
// Rewrites SELECT ... GROUP BY queries into direct MaterializedView lookups
// when an exact-matching MV exists, eliminating full partition scans.
// ============================================================================

#include "zeptodb/sql/ast.h"
#include "zeptodb/storage/materialized_view.h"
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace zeptodb::sql {

struct MVRewriteResult {
    std::string view_name;
    storage::MaterializedViewManager::ViewResult view_result;
};

class MVRewriter {
public:
    /// Try to rewrite a SELECT into a direct MV lookup.
    /// Returns nullopt if no matching MV is found.
    static std::optional<MVRewriteResult> try_rewrite(
        const SelectStmt& stmt,
        storage::MaterializedViewManager& mgr)
    {
        // ── Guard: only simple GROUP BY aggregations are candidates ──────
        if (!stmt.group_by || stmt.where || stmt.join || stmt.having)
            return std::nullopt;
        if (stmt.distinct || stmt.from_subquery || !stmt.cte_defs.empty())
            return std::nullopt;
        if (stmt.set_op != SelectStmt::SetOp::NONE)
            return std::nullopt;

        // ── Extract query shape: agg columns + group-by keys ─────────────
        std::vector<AggCol> query_aggs;

        for (auto& col : stmt.columns) {
            if (col.agg == AggFunc::NONE) continue; // group-by key, skip
            auto mapped = map_agg(col.agg);
            if (!mapped) return std::nullopt; // unmappable agg (AVG, VWAP, etc.)
            query_aggs.push_back({*mapped, col.column});
        }
        if (query_aggs.empty()) return std::nullopt;

        // Collect non-xbar group-by columns and xbar bucket
        std::vector<std::string> query_group_cols;
        int64_t query_xbar = 0;
        for (size_t i = 0; i < stmt.group_by->columns.size(); ++i) {
            int64_t xb = (i < stmt.group_by->xbar_buckets.size())
                         ? stmt.group_by->xbar_buckets[i] : 0;
            if (xb > 0)
                query_xbar = xb;
            else
                query_group_cols.push_back(stmt.group_by->columns[i]);
        }

        // ── Match against registered MVs ─────────────────────────────────
        auto defs = mgr.get_all_defs();
        for (auto& def : defs) {
            if (def.source_table != stmt.from_table) continue;
            if (def.xbar_bucket != query_xbar) continue;

            // Compare group-by columns (order-insensitive)
            if (!sets_equal(query_group_cols, def.group_by)) continue;

            // Every query agg must have a matching MV column
            if (!aggs_match(query_aggs, def.columns)) continue;

            // ── Match found — return pre-aggregated result ───────────────
            auto vr = mgr.query(def.view_name);
            return MVRewriteResult{def.view_name, std::move(vr)};
        }

        return std::nullopt;
    }

private:
    struct AggCol {
        storage::MVAggType agg;
        std::string source_col;
    };

    /// Map SQL AggFunc to storage MVAggType. Returns nullopt for unmappable.
    static std::optional<storage::MVAggType> map_agg(AggFunc f) {
        switch (f) {
            case AggFunc::SUM:   return storage::MVAggType::SUM;
            case AggFunc::COUNT: return storage::MVAggType::COUNT;
            case AggFunc::MIN:   return storage::MVAggType::MIN;
            case AggFunc::MAX:   return storage::MVAggType::MAX;
            case AggFunc::FIRST: return storage::MVAggType::FIRST;
            case AggFunc::LAST:  return storage::MVAggType::LAST;
            default:             return std::nullopt;
        }
    }

    /// Order-insensitive string set equality.
    static bool sets_equal(std::vector<std::string> a,
                           std::vector<std::string> b) {
        if (a.size() != b.size()) return false;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        return a == b;
    }

    /// Check every query aggregation has a matching MVColumnDef.
    static bool aggs_match(const std::vector<AggCol>& query,
                           const std::vector<storage::MVColumnDef>& mv_cols) {
        for (auto& qa : query) {
            bool found = false;
            for (auto& mc : mv_cols) {
                if (mc.agg == qa.agg && mc.source_col == qa.source_col) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
};

} // namespace zeptodb::sql
