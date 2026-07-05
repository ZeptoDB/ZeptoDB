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

    std::string commit_table = "physical_ai_supervision_commits";
    std::string commit_proposal_id_column = "proposal_id";
    std::string commit_decision_column = "decision";
    std::string commit_final_action_column = "final_action";
    std::string commit_reason_column = "reason";
    std::string commit_evidence_count_column = "evidence_count";
    std::string commit_fail_closed_column = "fail_closed";
    std::string commit_decision_written_column = "decision_written";
    std::string commit_evidence_written_column = "evidence_written";
    std::string commit_ts_column = "committed_ts_ns";

    /// Optional owner/fencing guard. When enabled, proposal loading returns no
    /// work unless `ownership_table` contains this supervisor name with the
    /// matching owner id and epoch. When `manage_worker_lease` is also enabled,
    /// the SQL adapter acquires/renews an expiring SQL lease and heartbeat row
    /// before loading proposals. Thread-safety: value config copied into hooks.
    bool require_worker_ownership = false;
    bool manage_worker_lease = false;
    std::string worker_owner_id;
    uint64_t worker_owner_epoch = 0;
    uint64_t worker_lease_ttl_ms = 15000;
    std::string ownership_table = "physical_ai_supervisor_ownership";
    std::string ownership_supervisor_column = "supervisor_name";
    std::string ownership_owner_id_column = "owner_id";
    std::string ownership_epoch_column = "owner_epoch";
    std::string ownership_lease_expires_at_column = "lease_expires_at_ns";
    std::string ownership_heartbeat_ts_column = "heartbeat_ts_ns";

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
/// idempotency checks the atomic commit ledger, the decision provider computes
/// a simple historical-outcome policy over `runtime.history_table`, and the
/// sink writes one atomic commit row before repairing decision/evidence
/// projections.
[[nodiscard]] zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks
makeActionOutcomeSqlRuntimeHooks(
    zeptodb::sql::QueryExecutor& executor,
    ActionOutcomeSqlAdapterConfig config);

} // namespace zeptodb::server
