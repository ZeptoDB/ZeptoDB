#pragma once
// ============================================================================
// ZeptoDB: SQL-backed Action-Outcome supervisor adapter
// ============================================================================

#include "zeptodb/feeds/action_outcome_supervisor_runtime.h"
#include "zeptodb/sql/executor.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace zeptodb::server {

/// SQL source/sink contract for the experimental Action-Outcome supervisor.
///
/// Thread-safety: value type. Hooks created from this config call the supplied
/// QueryExecutor on the worker thread and rely on ZeptoDB's normal SQL
/// executor concurrency behavior. Identifiers are validated before SQL is
/// emitted; string values are always SQL-literal escaped.
struct ActionOutcomeSqlAdapterConfig {
    zeptodb::feeds::ActionOutcomeSupervisorRuntimeConfig runtime;

    std::string proposal_id_column = "proposal_id";
    std::string proposal_source_type_column = "source_type";
    std::string proposal_action_column = "proposed_action";
    std::string proposal_ts_column = "source_ts_ns";

    std::string history_action_column = "action";
    std::string history_outcome_score_column = "outcome_score";

    std::string decision_proposal_id_column = "proposal_id";
    std::string decision_decision_column = "decision";
    std::string decision_final_action_column = "final_action";
    std::string decision_reason_column = "reason";
    std::string decision_evidence_count_column = "evidence_count";
    std::string decision_fail_closed_column = "fail_closed";
    std::string decision_ts_column = "decided_ts_ns";

    std::string evidence_proposal_id_column = "proposal_id";
    std::string evidence_count_column = "evidence_count";
    std::string evidence_reason_column = "reason";
    std::string evidence_ts_column = "written_ts_ns";

    /// Proposal query bound. Zero means `runtime.batch_limit`.
    size_t proposal_query_limit = 0;

    /// Maximum matching history rows scanned per proposal.
    size_t history_evidence_limit = 64;

    /// Suppress when at least this many history rows are below the threshold.
    uint64_t suppress_min_failure_count = 1;

    /// Outcome scores below this threshold are treated as failed outcomes.
    int64_t suppress_outcome_score_below = 0;
};

/// Validate table/column identifiers and numeric limits.
[[nodiscard]] bool validateActionOutcomeSqlAdapterConfig(
    const ActionOutcomeSqlAdapterConfig& config,
    std::string* error = nullptr);

/// Create the default SQL contract tables when they are missing.
///
/// Existing tables are left unchanged. This helper is intended for demos and
/// controlled product pilots; production deployments should still manage
/// schemas through their normal migration path.
[[nodiscard]] bool ensureActionOutcomeSqlTables(
    zeptodb::sql::QueryExecutor& executor,
    const ActionOutcomeSqlAdapterConfig& config,
    std::string* error = nullptr);

/// Build runtime hooks backed by ZeptoDB SQL tables.
///
/// The proposal loader reads pending proposals from `runtime.proposal_table`,
/// idempotency checks `runtime.decision_table`, the decision provider computes
/// a simple historical-outcome policy over `runtime.history_table`, and the
/// sink writes evidence summary then decision rows.
[[nodiscard]] zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks
makeActionOutcomeSqlRuntimeHooks(
    zeptodb::sql::QueryExecutor& executor,
    ActionOutcomeSqlAdapterConfig config);

} // namespace zeptodb::server
