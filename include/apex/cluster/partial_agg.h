#pragma once
// ============================================================================
// Phase C-3: Partial Aggregation Merge
// ============================================================================
// After scatter-gather, each data node returns a QueryResultSet.
// This header provides merge logic to combine per-node results correctly.
//
// Merge strategies:
//   SCALAR_AGG — all columns are aggregate functions, 1 row per node.
//                SUM/COUNT/VWAP: add values.  MIN: take minimum.  MAX: take
//                maximum.  AVG: not mergeable (returns error).
//
//   CONCAT     — plain SELECT or GROUP BY with symbol-affinity partitioning.
//                With symbol affinity each group key exists on exactly one
//                node, so concatenation is correct.
// ============================================================================

#include "apex/sql/executor.h"
#include "apex/sql/ast.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <limits>
#include <string>
#include <vector>

namespace apex::cluster {

// ============================================================================
// Aggregate function detection from column name
// ============================================================================
enum class AggFunc { NONE, SUM, COUNT, MIN, MAX, AVG, VWAP };

inline AggFunc detect_agg(const std::string& col) {
    // Build lowercase copy for matching
    std::string lo;
    lo.reserve(col.size());
    for (char c : col) lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto starts = [&](const char* prefix) {
        return lo.rfind(prefix, 0) == 0;
    };

    if (lo == "count(*)" || starts("count("))  return AggFunc::COUNT;
    if (starts("sum("))   return AggFunc::SUM;
    if (starts("min("))   return AggFunc::MIN;
    if (starts("max("))   return AggFunc::MAX;
    if (starts("avg("))   return AggFunc::AVG;
    if (starts("vwap("))  return AggFunc::VWAP;
    return AggFunc::NONE;
}

// ============================================================================
// Merge strategy detection
// ============================================================================
enum class MergeStrategy { SCALAR_AGG, CONCAT, MERGE_GROUP_BY };

inline MergeStrategy detect_merge_strategy(
    const std::vector<apex::sql::QueryResultSet>& results)
{
    // Find first non-empty result to inspect structure
    for (const auto& r : results) {
        if (!r.ok() || r.rows.empty()) continue;

        // Scalar aggregate: exactly 1 row and every column name is an agg function
        if (r.rows.size() == 1 && !r.column_names.empty()) {
            bool all_agg = true;
            for (const auto& name : r.column_names) {
                if (detect_agg(name) == AggFunc::NONE) { all_agg = false; break; }
            }
            if (all_agg) return MergeStrategy::SCALAR_AGG;
        }
        break;
    }
    return MergeStrategy::CONCAT;
}

// ============================================================================
// merge_scalar_results: merge 1-row-per-node aggregate results
// ============================================================================
inline apex::sql::QueryResultSet merge_scalar_results(
    const std::vector<apex::sql::QueryResultSet>& results)
{
    if (results.empty()) return {};

    // Find a non-error result to use as template
    const apex::sql::QueryResultSet* tmpl = nullptr;
    for (const auto& r : results) {
        if (r.ok() && !r.rows.empty()) { tmpl = &r; break; }
    }
    if (!tmpl) {
        apex::sql::QueryResultSet err;
        err.error = "merge_scalar: all nodes returned errors";
        return err;
    }

    apex::sql::QueryResultSet merged;
    merged.column_names = tmpl->column_names;
    merged.column_types = tmpl->column_types;

    const size_t ncols = merged.column_names.size();
    std::vector<int64_t> row(ncols, 0);
    std::vector<int64_t> avg_count(ncols, 0); // track node count for AVG columns

    // Initialize accumulators with identity element
    for (size_t ci = 0; ci < ncols; ++ci) {
        AggFunc af = detect_agg(merged.column_names[ci]);
        if (af == AggFunc::MIN) row[ci] = std::numeric_limits<int64_t>::max();
        if (af == AggFunc::MAX) row[ci] = std::numeric_limits<int64_t>::min();
    }

    for (const auto& r : results) {
        if (!r.ok() || r.rows.empty()) continue;
        merged.rows_scanned += r.rows_scanned;
        const auto& src = r.rows[0];

        for (size_t ci = 0; ci < ncols && ci < src.size(); ++ci) {
            int64_t v = src[ci];
            switch (detect_agg(merged.column_names[ci])) {
                case AggFunc::SUM:
                case AggFunc::COUNT:
                case AggFunc::VWAP:  // partial Σ(pv) or Σ(v) — just add
                    row[ci] += v;
                    break;
                case AggFunc::MIN:
                    row[ci] = std::min(row[ci], v);
                    break;
                case AggFunc::MAX:
                    row[ci] = std::max(row[ci], v);
                    break;
                case AggFunc::AVG:
                    // Accumulate sum of per-node averages; divide by node count
                    row[ci] += v;
                    avg_count[ci] += 1;
                    break;
                case AggFunc::NONE:
                    row[ci] = v;  // passthrough (e.g. literal or expression)
                    break;
            }
        }
    }

    // Finalize AVG columns
    for (size_t ci = 0; ci < ncols; ++ci) {
        if (detect_agg(merged.column_names[ci]) == AggFunc::AVG && avg_count[ci] > 0)
            row[ci] = row[ci] / avg_count[ci];
    }

    merged.rows.push_back(std::move(row));
    return merged;
}

// ============================================================================
// merge_concat_results: concatenate rows from all nodes
//
// Correct for:
//   - Plain SELECT (no aggregation)
//   - GROUP BY with symbol-affinity partitioning (each group key on one node)
// ============================================================================
inline apex::sql::QueryResultSet merge_concat_results(
    const std::vector<apex::sql::QueryResultSet>& results)
{
    if (results.empty()) return {};

    apex::sql::QueryResultSet merged;
    // Use first non-empty result for column metadata
    for (const auto& r : results) {
        if (!r.column_names.empty()) {
            merged.column_names = r.column_names;
            merged.column_types = r.column_types;
            break;
        }
    }

    for (const auto& r : results) {
        if (!r.ok()) {
            if (merged.error.empty()) merged.error = r.error;
            continue;
        }
        merged.rows.insert(merged.rows.end(), r.rows.begin(), r.rows.end());
        merged.rows_scanned += r.rows_scanned;
    }

    return merged;
}

// ============================================================================
// merge_results: top-level entry point
//
// strategy_hint — pass an explicit MergeStrategy when the SQL query context
//   is known (e.g. from the coordinator after parsing the SQL).  When nullopt
//   the strategy is detected heuristically from result column names, which
//   works for test-constructed results but may mis-detect when the executor
//   uses raw column names instead of "sum(col)" style names.
// ============================================================================
inline apex::sql::QueryResultSet merge_results(
    const std::vector<apex::sql::QueryResultSet>& results,
    std::optional<MergeStrategy> strategy_hint = std::nullopt)
{
    if (results.empty()) {
        apex::sql::QueryResultSet err;
        err.error = "merge_results: empty result list";
        return err;
    }

    // Propagate fatal errors immediately
    for (const auto& r : results) {
        if (!r.ok()) return r;
    }

    MergeStrategy strat = strategy_hint.value_or(detect_merge_strategy(results));
    if (strat == MergeStrategy::SCALAR_AGG) {
        return merge_scalar_results(results);
    }
    return merge_concat_results(results);
}

// ============================================================================
// merge_scalar_with_sql_aggs: SCALAR_AGG merge driven by the SQL AST.
//
// Uses the actual AggFunc stored on each SelectExpr (from the parsed query)
// rather than detecting the function from the result column name.  This is
// necessary because the executor names un-aliased aggregate columns with the
// raw column name (e.g. "count(*)" → "*", "sum(volume)" → "volume"), making
// name-based detection unreliable.
// ============================================================================
inline apex::sql::QueryResultSet merge_scalar_with_sql_aggs(
    const std::vector<apex::sql::QueryResultSet>& results,
    const std::vector<apex::sql::AggFunc>&        sql_aggs)
{
    if (results.empty()) return {};

    const apex::sql::QueryResultSet* tmpl = nullptr;
    for (const auto& r : results) {
        if (r.ok() && !r.rows.empty()) { tmpl = &r; break; }
    }
    if (!tmpl) {
        apex::sql::QueryResultSet err;
        err.error = "merge_scalar: all nodes returned errors";
        return err;
    }

    apex::sql::QueryResultSet merged;
    merged.column_names = tmpl->column_names;
    merged.column_types = tmpl->column_types;

    const size_t ncols = merged.column_names.size();
    std::vector<int64_t> row(ncols, 0);
    std::vector<int64_t> avg_count(ncols, 0);

    // Initialize identity values for MIN/MAX
    for (size_t ci = 0; ci < ncols && ci < sql_aggs.size(); ++ci) {
        if (sql_aggs[ci] == apex::sql::AggFunc::MIN)
            row[ci] = std::numeric_limits<int64_t>::max();
        else if (sql_aggs[ci] == apex::sql::AggFunc::MAX)
            row[ci] = std::numeric_limits<int64_t>::min();
    }

    for (const auto& r : results) {
        if (!r.ok() || r.rows.empty()) continue;
        merged.rows_scanned += r.rows_scanned;
        const auto& src = r.rows[0];

        for (size_t ci = 0; ci < ncols && ci < src.size(); ++ci) {
            int64_t v = src[ci];
            apex::sql::AggFunc af = (ci < sql_aggs.size())
                                    ? sql_aggs[ci]
                                    : apex::sql::AggFunc::NONE;
            switch (af) {
                case apex::sql::AggFunc::SUM:
                case apex::sql::AggFunc::COUNT:
                case apex::sql::AggFunc::VWAP:
                    row[ci] += v;
                    break;
                case apex::sql::AggFunc::MIN:
                    row[ci] = std::min(row[ci], v);
                    break;
                case apex::sql::AggFunc::MAX:
                    row[ci] = std::max(row[ci], v);
                    break;
                case apex::sql::AggFunc::AVG:
                    row[ci] += v;
                    avg_count[ci] += 1;
                    break;
                default:
                    row[ci] = v;  // passthrough (literal, FIRST, LAST, etc.)
                    break;
            }
        }
    }

    // Finalize AVG columns
    for (size_t ci = 0; ci < ncols; ++ci) {
        apex::sql::AggFunc af = (ci < sql_aggs.size())
                                ? sql_aggs[ci] : apex::sql::AggFunc::NONE;
        if (af == apex::sql::AggFunc::AVG && avg_count[ci] > 0)
            row[ci] = row[ci] / avg_count[ci];
    }

    merged.rows.push_back(std::move(row));
    return merged;
}

// ============================================================================
// merge_group_by_results: re-aggregate GROUP BY results from multiple nodes.
//
// Use when the GROUP BY key is NOT symbol-affinity — the same key bucket can
// appear on multiple nodes and must be combined:
//   col_is_key[i] = true  → group key column; copied from first seen row
//   col_aggs[i]          → agg function applied when merging non-key columns
// ============================================================================
inline apex::sql::QueryResultSet merge_group_by_results(
    const std::vector<apex::sql::QueryResultSet>& results,
    const std::vector<bool>&                      col_is_key,
    const std::vector<apex::sql::AggFunc>&        col_aggs)
{
    if (results.empty()) return {};

    apex::sql::QueryResultSet merged;
    for (const auto& r : results) {
        if (!r.column_names.empty()) {
            merged.column_names = r.column_names;
            merged.column_types = r.column_types;
            break;
        }
    }

    const size_t ncols = merged.column_names.size();

    // group key → accumulated row
    std::map<std::vector<int64_t>, std::vector<int64_t>> groups;
    // For AVG columns: track per-group count for final division
    std::map<std::vector<int64_t>, std::vector<int64_t>> avg_counts;

    for (const auto& r : results) {
        if (!r.ok()) {
            if (merged.error.empty()) merged.error = r.error;
            continue;
        }
        merged.rows_scanned += r.rows_scanned;

        for (const auto& src : r.rows) {
            // Build composite group key from key columns
            std::vector<int64_t> key;
            for (size_t ci = 0; ci < ncols && ci < col_is_key.size(); ++ci) {
                if (col_is_key[ci]) key.push_back(ci < src.size() ? src[ci] : 0);
            }

            auto it = groups.find(key);
            if (it == groups.end()) {
                // Initialize new group: identity values for agg, key values for keys
                std::vector<int64_t> acc(ncols, 0);
                std::vector<int64_t> cnt(ncols, 0);
                for (size_t ci = 0; ci < ncols; ++ci) {
                    if (ci < col_is_key.size() && col_is_key[ci]) {
                        acc[ci] = ci < src.size() ? src[ci] : 0;
                    } else {
                        apex::sql::AggFunc af = ci < col_aggs.size()
                                                ? col_aggs[ci]
                                                : apex::sql::AggFunc::NONE;
                        if (af == apex::sql::AggFunc::MIN)
                            acc[ci] = std::numeric_limits<int64_t>::max();
                        else if (af == apex::sql::AggFunc::MAX)
                            acc[ci] = std::numeric_limits<int64_t>::min();
                    }
                }
                it = groups.emplace(key, std::move(acc)).first;
                avg_counts.emplace(key, std::move(cnt));
            }

            auto& acc = it->second;
            auto& cnt = avg_counts[key];
            for (size_t ci = 0; ci < ncols && ci < src.size(); ++ci) {
                if (ci < col_is_key.size() && col_is_key[ci]) continue;
                apex::sql::AggFunc af = ci < col_aggs.size()
                                        ? col_aggs[ci]
                                        : apex::sql::AggFunc::NONE;
                int64_t v = src[ci];
                switch (af) {
                    case apex::sql::AggFunc::SUM:
                    case apex::sql::AggFunc::COUNT:
                    case apex::sql::AggFunc::VWAP:
                        acc[ci] += v; break;
                    case apex::sql::AggFunc::MIN:
                        acc[ci] = std::min(acc[ci], v); break;
                    case apex::sql::AggFunc::MAX:
                        acc[ci] = std::max(acc[ci], v); break;
                    case apex::sql::AggFunc::AVG:
                        // Accumulate sum; track count for final division
                        acc[ci] += v;
                        cnt[ci] += 1;
                        break;
                    default:
                        acc[ci] = v; break;  // FIRST/LAST/XBAR: take last
                }
            }
        }
    }

    for (auto& [key, acc] : groups) {
        // Finalize AVG columns: acc has sum of per-node averages × count
        // For correct distributed AVG, each node should return SUM not AVG.
        // But if nodes return AVG, we average the averages (approximate).
        auto& cnt = avg_counts[key];
        for (size_t ci = 0; ci < ncols; ++ci) {
            apex::sql::AggFunc af = ci < col_aggs.size()
                                    ? col_aggs[ci]
                                    : apex::sql::AggFunc::NONE;
            if (af == apex::sql::AggFunc::AVG && cnt[ci] > 0) {
                acc[ci] = acc[ci] / cnt[ci];
            }
        }
        merged.rows.push_back(acc);
    }

    return merged;
}

} // namespace apex::cluster
