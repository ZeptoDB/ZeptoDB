// ============================================================================
// ZeptoDB: SQL-backed Action-Outcome supervisor adapter
// ============================================================================

#include "zeptodb/server/action_outcome_sql_adapter.h"

#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zeptodb::server {
namespace {

using zeptodb::feeds::ActionOutcomeDecision;
using zeptodb::feeds::ActionOutcomeDecisionResult;
using zeptodb::feeds::ActionOutcomeProposal;
using zeptodb::feeds::ActionOutcomeProposalLoadResult;
using zeptodb::sql::QueryExecutor;
using zeptodb::sql::QueryResultSet;
using zeptodb::storage::ColumnType;

bool is_safe_identifier(const std::string& value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first) && value.front() != '_') return false;
    return std::all_of(value.begin() + 1, value.end(), [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || ch == '_';
    });
}

bool require_identifier(const std::string& value,
                        const std::string& label,
                        std::string* error) {
    if (is_safe_identifier(value)) return true;
    if (error) *error = label + " must be a simple SQL identifier";
    return false;
}

std::string sql_string_literal(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') out.push_back('\'');
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

int64_t now_ns() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

size_t row_count(const QueryResultSet& result) {
    return std::max(result.rows.size(), result.typed_rows.size());
}

int64_t cell_i64(const QueryResultSet& result, size_t row, size_t col) {
    if (row < result.typed_rows.size() && col < result.typed_rows[row].size()) {
        if (col < result.column_types.size() &&
            (result.column_types[col] == ColumnType::FLOAT64 ||
             result.column_types[col] == ColumnType::FLOAT32)) {
            return static_cast<int64_t>(result.typed_rows[row][col].f);
        }
        return result.typed_rows[row][col].i;
    }
    if (row < result.rows.size() && col < result.rows[row].size()) {
        return result.rows[row][col];
    }
    return 0;
}

size_t string_cell_index(const QueryResultSet& result, size_t row, size_t col) {
    size_t per_row = 0;
    size_t before_col = 0;
    for (size_t c = 0; c < result.column_names.size(); ++c) {
        const bool is_string =
            c < result.column_types.size() &&
            (result.column_types[c] == ColumnType::STRING ||
             result.column_types[c] == ColumnType::SYMBOL);
        if (is_string) {
            if (c < col) ++before_col;
            ++per_row;
        }
    }
    return row * per_row + before_col;
}

std::string cell_string(const QueryResultSet& result, size_t row, size_t col) {
    if (col < result.column_types.size() &&
        (result.column_types[col] == ColumnType::STRING ||
         result.column_types[col] == ColumnType::SYMBOL)) {
        if (result.symbol_dict != nullptr) {
            return std::string(result.symbol_dict->lookup(
                static_cast<uint32_t>(cell_i64(result, row, col))));
        }
        const size_t idx = string_cell_index(result, row, col);
        if (idx < result.string_rows.size()) return result.string_rows[idx];
    }
    if (result.rows.empty() && col == 0 && row < result.string_rows.size()) {
        return result.string_rows[row];
    }
    return std::to_string(cell_i64(result, row, col));
}

std::string insert_count_sql(const std::string& table,
                             const std::vector<std::string>& columns,
                             const std::vector<std::string>& values) {
    std::ostringstream sql;
    sql << "INSERT INTO " << table << " (";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i != 0) sql << ", ";
        sql << columns[i];
    }
    sql << ") VALUES (";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) sql << ", ";
        sql << values[i];
    }
    sql << ")";
    return sql.str();
}

bool execute_ok(QueryExecutor& executor,
                const std::string& sql,
                const std::string& context,
                std::string* error) {
    const auto result = executor.execute(sql);
    if (result.ok()) return true;
    if (error) *error = context + ": " + result.error;
    return false;
}

bool table_has_rows(QueryExecutor& executor,
                    const std::string& table,
                    const std::string& context,
                    bool* has_rows,
                    std::string* error) {
    std::ostringstream sql;
    sql << "SELECT count(*) FROM " << table;
    const auto result = executor.execute(sql.str());
    if (!result.ok()) {
        if (error) *error = context + ": " + result.error;
        return false;
    }
    *has_rows = row_count(result) > 0 && cell_i64(result, 0, 0) > 0;
    return true;
}

bool row_exists_by_string_key(QueryExecutor& executor,
                              const std::string& table,
                              const std::string& column,
                              const std::string& value,
                              const std::string& context,
                              bool* exists,
                              std::string* error) {
    const auto code = executor.intern_symbol_for_ingest(value);
    std::ostringstream sql;
    sql << "SELECT " << column
        << " FROM " << table
        << " WHERE " << column
        << " = " << static_cast<int64_t>(code)
        << " LIMIT 1";
    const auto result = executor.execute(sql.str());
    if (!result.ok()) {
        if (error) *error = context + ": " + result.error;
        return false;
    }
    *exists = row_count(result) > 0 || !result.string_rows.empty();
    return true;
}

std::string create_proposal_table_sql(const ActionOutcomeSqlAdapterConfig& config) {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << config.runtime.proposal_table
        << " (" << config.proposal_id_column << " STRING, "
        << config.proposal_source_type_column << " STRING, "
        << config.proposal_action_column << " STRING, "
        << config.proposal_ts_column << " TIMESTAMP_NS)";
    return sql.str();
}

std::string create_history_table_sql(const ActionOutcomeSqlAdapterConfig& config) {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << config.runtime.history_table
        << " (" << config.history_action_column << " STRING, "
        << config.history_outcome_score_column << " INT64, "
        << "source_ts_ns TIMESTAMP_NS)";
    return sql.str();
}

std::string create_decision_table_sql(const ActionOutcomeSqlAdapterConfig& config) {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << config.runtime.decision_table
        << " (" << config.decision_proposal_id_column << " STRING, "
        << config.decision_decision_column << " STRING, "
        << config.decision_final_action_column << " STRING, "
        << config.decision_reason_column << " STRING, "
        << config.decision_evidence_count_column << " INT64, "
        << config.decision_fail_closed_column << " BOOL, "
        << config.decision_ts_column << " TIMESTAMP_NS)";
    return sql.str();
}

std::string create_evidence_table_sql(const ActionOutcomeSqlAdapterConfig& config) {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << config.runtime.evidence_table
        << " (" << config.evidence_proposal_id_column << " STRING, "
        << config.evidence_count_column << " INT64, "
        << config.evidence_reason_column << " STRING, "
        << config.evidence_ts_column << " TIMESTAMP_NS)";
    return sql.str();
}

std::string create_commit_table_sql(const ActionOutcomeSqlAdapterConfig& config) {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << config.commit_table
        << " (" << config.commit_proposal_id_column << " STRING, "
        << config.commit_decision_column << " STRING, "
        << config.commit_final_action_column << " STRING, "
        << config.commit_reason_column << " STRING, "
        << config.commit_evidence_count_column << " INT64, "
        << config.commit_fail_closed_column << " BOOL, "
        << config.commit_decision_written_column << " BOOL, "
        << config.commit_evidence_written_column << " BOOL, "
        << config.commit_ts_column << " TIMESTAMP_NS)";
    return sql.str();
}

std::string create_ownership_table_sql(const ActionOutcomeSqlAdapterConfig& config) {
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << config.ownership_table
        << " (" << config.ownership_supervisor_column << " STRING, "
        << config.ownership_owner_id_column << " STRING, "
        << config.ownership_epoch_column << " INT64, "
        << config.ownership_lease_expires_at_column << " INT64, "
        << config.ownership_heartbeat_ts_column << " TIMESTAMP_NS)";
    return sql.str();
}

struct WorkerLeaseState {
    explicit WorkerLeaseState(uint64_t initial_epoch)
        : owner_epoch(initial_epoch) {}

    std::atomic<uint64_t> owner_epoch;
};

struct CommittedDecisionState {
    bool exists = false;
    ActionOutcomeDecision decision;
    int64_t committed_ts_ns = 0;
};

bool delete_ownership_rows(QueryExecutor& executor,
                           const ActionOutcomeSqlAdapterConfig& config,
                           std::string* error) {
    const auto name_code =
        executor.intern_symbol_for_ingest(config.runtime.name);
    std::ostringstream sql;
    sql << "DELETE FROM " << config.ownership_table
        << " WHERE " << config.ownership_supervisor_column
        << " = " << static_cast<int64_t>(name_code);
    return execute_ok(executor, sql.str(), "delete ownership rows", error);
}

bool insert_ownership_row(QueryExecutor& executor,
                          const ActionOutcomeSqlAdapterConfig& config,
                          uint64_t owner_epoch,
                          int64_t heartbeat_ts_ns,
                          int64_t lease_expires_at_ns,
                          std::string* error) {
    const std::string sql = insert_count_sql(
        config.ownership_table,
        {config.ownership_supervisor_column,
         config.ownership_owner_id_column,
         config.ownership_epoch_column,
         config.ownership_lease_expires_at_column,
         config.ownership_heartbeat_ts_column},
        {sql_string_literal(config.runtime.name),
         sql_string_literal(config.worker_owner_id),
         std::to_string(owner_epoch),
         std::to_string(lease_expires_at_ns),
         std::to_string(heartbeat_ts_ns)});
    return execute_ok(executor, sql, "insert ownership lease", error);
}

bool renew_ownership_lease(QueryExecutor& executor,
                           const ActionOutcomeSqlAdapterConfig& config,
                           int64_t heartbeat_ts_ns,
                           int64_t lease_expires_at_ns,
                           std::string* error) {
    const auto name_code =
        executor.intern_symbol_for_ingest(config.runtime.name);
    std::ostringstream sql;
    sql << "UPDATE " << config.ownership_table
        << " SET " << config.ownership_heartbeat_ts_column
        << " = " << heartbeat_ts_ns
        << ", " << config.ownership_lease_expires_at_column
        << " = " << lease_expires_at_ns
        << " WHERE " << config.ownership_supervisor_column
        << " = " << static_cast<int64_t>(name_code);
    return execute_ok(executor, sql.str(), "renew ownership lease", error);
}

bool acquire_ownership_lease(QueryExecutor& executor,
                             const ActionOutcomeSqlAdapterConfig& config,
                             uint64_t owner_epoch,
                             int64_t heartbeat_ts_ns,
                             int64_t lease_expires_at_ns,
                             std::string* error) {
    if (!delete_ownership_rows(executor, config, error)) {
        return false;
    }
    return insert_ownership_row(
        executor, config, owner_epoch, heartbeat_ts_ns, lease_expires_at_ns, error);
}

bool worker_ownership_allows_processing(
    QueryExecutor& executor,
    const ActionOutcomeSqlAdapterConfig& config,
    WorkerLeaseState* lease_state,
    bool* allowed,
    std::string* error) {
    *allowed = false;
    if (!config.require_worker_ownership) {
        *allowed = true;
        return true;
    }

    const auto name_code =
        executor.intern_symbol_for_ingest(config.runtime.name);
    std::ostringstream sql;
    sql << "SELECT " << config.ownership_owner_id_column << ", "
        << config.ownership_epoch_column;
    if (config.manage_worker_lease) {
        sql << ", " << config.ownership_lease_expires_at_column
            << ", " << config.ownership_heartbeat_ts_column;
    }
    sql << " FROM " << config.ownership_table
        << " WHERE " << config.ownership_supervisor_column
        << " = " << static_cast<int64_t>(name_code);
    if (config.manage_worker_lease) {
        sql << " ORDER BY " << config.ownership_lease_expires_at_column
            << " DESC";
    }
    sql << " LIMIT 1";
    const auto result = executor.execute(sql.str());
    if (!result.ok()) {
        if (error) *error = "ownership query failed: " + result.error;
        return false;
    }

    const int64_t heartbeat_ts_ns = now_ns();
    const int64_t lease_expires_at_ns =
        heartbeat_ts_ns +
        static_cast<int64_t>(config.worker_lease_ttl_ms) * 1000000LL;
    if (row_count(result) == 0 && result.string_rows.empty()) {
        if (config.manage_worker_lease) {
            uint64_t epoch = lease_state ? lease_state->owner_epoch.load() : 0;
            if (epoch == 0) epoch = 1;
            if (!acquire_ownership_lease(
                    executor,
                    config,
                    epoch,
                    heartbeat_ts_ns,
                    lease_expires_at_ns,
                    error)) {
                return false;
            }
            if (lease_state) lease_state->owner_epoch.store(epoch);
            *allowed = true;
        }
        return true;
    }

    const std::string owner_id = cell_string(result, 0, 0);
    const uint64_t owner_epoch = static_cast<uint64_t>(cell_i64(result, 0, 1));
    if (owner_id == config.worker_owner_id) {
        if (lease_state) lease_state->owner_epoch.store(owner_epoch);
        if (config.manage_worker_lease &&
            !renew_ownership_lease(
                executor, config, heartbeat_ts_ns, lease_expires_at_ns, error)) {
            return false;
        }
        *allowed = config.manage_worker_lease ||
                   owner_epoch == config.worker_owner_epoch;
        return true;
    }

    if (config.manage_worker_lease) {
        const int64_t current_lease_expires_at_ns = cell_i64(result, 0, 2);
        if (current_lease_expires_at_ns <= heartbeat_ts_ns) {
            const uint64_t current_local_epoch =
                lease_state ? lease_state->owner_epoch.load()
                            : config.worker_owner_epoch;
            const uint64_t next_epoch =
                std::max(current_local_epoch, owner_epoch) + 1;
            if (!acquire_ownership_lease(
                    executor,
                    config,
                    next_epoch,
                    heartbeat_ts_ns,
                    lease_expires_at_ns,
                    error)) {
                return false;
            }
            if (lease_state) lease_state->owner_epoch.store(next_epoch);
            *allowed = true;
        }
    }
    return true;
}

bool load_committed_decision(QueryExecutor& executor,
                             const ActionOutcomeSqlAdapterConfig& config,
                             const std::string& proposal_id,
                             CommittedDecisionState* state,
                             std::string* error) {
    const auto proposal_code = executor.intern_symbol_for_ingest(proposal_id);
    std::ostringstream sql;
    sql << "SELECT " << config.commit_proposal_id_column << ", "
        << config.commit_decision_column << ", "
        << config.commit_final_action_column << ", "
        << config.commit_reason_column << ", "
        << config.commit_evidence_count_column << ", "
        << config.commit_fail_closed_column << ", "
        << config.commit_ts_column
        << " FROM " << config.commit_table
        << " WHERE " << config.commit_proposal_id_column
        << " = " << static_cast<int64_t>(proposal_code)
        << " LIMIT 1";
    const auto result = executor.execute(sql.str());
    if (!result.ok()) {
        if (error) *error = "commit idempotency query failed: " + result.error;
        return false;
    }
    if (row_count(result) == 0 && result.string_rows.empty()) {
        *state = CommittedDecisionState{};
        return true;
    }

    CommittedDecisionState loaded;
    loaded.exists = true;
    loaded.decision.proposal_id = cell_string(result, 0, 0);
    loaded.decision.decision = cell_string(result, 0, 1);
    loaded.decision.final_action = cell_string(result, 0, 2);
    loaded.decision.reason = cell_string(result, 0, 3);
    loaded.decision.evidence_count = static_cast<uint64_t>(cell_i64(result, 0, 4));
    loaded.decision.fail_closed = cell_i64(result, 0, 5) != 0;
    loaded.committed_ts_ns = cell_i64(result, 0, 6);
    *state = std::move(loaded);
    return true;
}

bool insert_atomic_commit_row(QueryExecutor& executor,
                              const ActionOutcomeSqlAdapterConfig& config,
                              const ActionOutcomeDecision& decision,
                              int64_t ts,
                              std::string* error) {
    const std::string commit_sql = insert_count_sql(
        config.commit_table,
        {config.commit_proposal_id_column,
         config.commit_decision_column,
         config.commit_final_action_column,
         config.commit_reason_column,
         config.commit_evidence_count_column,
         config.commit_fail_closed_column,
         config.commit_decision_written_column,
         config.commit_evidence_written_column,
         config.commit_ts_column},
        {sql_string_literal(decision.proposal_id),
         sql_string_literal(decision.decision),
         sql_string_literal(decision.final_action),
         sql_string_literal(decision.reason),
         std::to_string(decision.evidence_count),
         decision.fail_closed ? "1" : "0",
         "1",
         "1",
         std::to_string(ts)});
    return execute_ok(executor, commit_sql, "insert atomic sink commit", error);
}

bool sink_decision_state(QueryExecutor& executor,
                         const ActionOutcomeSqlAdapterConfig& config,
                         const ActionOutcomeDecision& decision,
                         int64_t ts,
                         bool decision_exists,
                         bool evidence_exists,
                         std::string* error) {
    if (!evidence_exists) {
        const std::string evidence_sql = insert_count_sql(
            config.runtime.evidence_table,
            {config.evidence_proposal_id_column,
             config.evidence_count_column,
             config.evidence_reason_column,
             config.evidence_ts_column},
            {sql_string_literal(decision.proposal_id),
             std::to_string(decision.evidence_count),
             sql_string_literal(decision.reason),
             std::to_string(ts)});
        if (!execute_ok(executor, evidence_sql, "insert evidence summary", error)) {
            return false;
        }
    }

    if (!decision_exists) {
        const std::string decision_sql = insert_count_sql(
            config.runtime.decision_table,
            {config.decision_proposal_id_column,
             config.decision_decision_column,
             config.decision_final_action_column,
             config.decision_reason_column,
             config.decision_evidence_count_column,
             config.decision_fail_closed_column,
             config.decision_ts_column},
            {sql_string_literal(decision.proposal_id),
             sql_string_literal(decision.decision),
             sql_string_literal(decision.final_action),
             sql_string_literal(decision.reason),
             std::to_string(decision.evidence_count),
             decision.fail_closed ? "1" : "0",
             std::to_string(ts)});
        if (!execute_ok(executor, decision_sql, "insert decision", error)) {
            return false;
        }
    }

    return true;
}

bool repair_projection_from_commit(QueryExecutor& executor,
                                   const ActionOutcomeSqlAdapterConfig& config,
                                   const CommittedDecisionState& committed,
                                   std::string* error) {
    bool decision_exists = false;
    bool evidence_exists = false;
    if (!row_exists_by_string_key(
            executor,
            config.runtime.decision_table,
            config.decision_proposal_id_column,
            committed.decision.proposal_id,
            "decision projection query failed",
            &decision_exists,
            error)) {
        return false;
    }
    if (!row_exists_by_string_key(
            executor,
            config.runtime.evidence_table,
            config.evidence_proposal_id_column,
            committed.decision.proposal_id,
            "evidence projection query failed",
            &evidence_exists,
            error)) {
        return false;
    }
    return sink_decision_state(
        executor,
        config,
        committed.decision,
        committed.committed_ts_ns,
        decision_exists,
        evidence_exists,
        error);
}

void append_proposal_rows(const QueryResultSet& result,
                          std::vector<ActionOutcomeProposal>* proposals,
                          std::unordered_set<std::string>* seen_ids) {
    const size_t rows = row_count(result);
    proposals->reserve(proposals->size() + rows);
    for (size_t r = 0; r < rows; ++r) {
        ActionOutcomeProposal proposal;
        proposal.proposal_id = cell_string(result, r, 0);
        proposal.source_type = cell_string(result, r, 1);
        proposal.proposed_action = cell_string(result, r, 2);
        proposal.source_ts_ns = cell_i64(result, r, 3);
        if (seen_ids) {
            const auto [_, inserted] = seen_ids->insert(proposal.proposal_id);
            if (!inserted) continue;
        }
        proposals->push_back(std::move(proposal));
    }
}

ActionOutcomeDecision no_evidence_decision(
    const ActionOutcomeProposal& proposal,
    const ActionOutcomeSqlAdapterConfig& config) {
    ActionOutcomeDecision decision;
    decision.proposal_id = proposal.proposal_id;
    decision.decision = "suppress_no_evidence";
    decision.final_action = config.runtime.fail_closed_action;
    decision.reason = "no_historical_outcome_evidence";
    decision.evidence_count = 0;
    decision.fail_closed = true;
    return decision;
}

} // namespace

bool validateActionOutcomeSqlAdapterConfig(
    const ActionOutcomeSqlAdapterConfig& config,
    std::string* error) {
    const auto& runtime = config.runtime;
    const std::vector<std::pair<std::string, std::string>> identifiers = {
        {runtime.history_table, "history_table"},
        {runtime.proposal_table, "proposal_table"},
        {runtime.decision_table, "decision_table"},
        {runtime.evidence_table, "evidence_table"},
        {config.commit_table, "commit_table"},
        {config.proposal_id_column, "proposal_id_column"},
        {config.proposal_source_type_column, "proposal_source_type_column"},
        {config.proposal_action_column, "proposal_action_column"},
        {config.proposal_ts_column, "proposal_ts_column"},
        {config.history_action_column, "history_action_column"},
        {config.history_outcome_score_column, "history_outcome_score_column"},
        {config.decision_proposal_id_column, "decision_proposal_id_column"},
        {config.decision_decision_column, "decision_decision_column"},
        {config.decision_final_action_column, "decision_final_action_column"},
        {config.decision_reason_column, "decision_reason_column"},
        {config.decision_evidence_count_column,
         "decision_evidence_count_column"},
        {config.decision_fail_closed_column, "decision_fail_closed_column"},
        {config.decision_ts_column, "decision_ts_column"},
        {config.evidence_proposal_id_column, "evidence_proposal_id_column"},
        {config.evidence_count_column, "evidence_count_column"},
        {config.evidence_reason_column, "evidence_reason_column"},
        {config.evidence_ts_column, "evidence_ts_column"},
        {config.commit_proposal_id_column, "commit_proposal_id_column"},
        {config.commit_decision_column, "commit_decision_column"},
        {config.commit_final_action_column, "commit_final_action_column"},
        {config.commit_reason_column, "commit_reason_column"},
        {config.commit_evidence_count_column,
         "commit_evidence_count_column"},
        {config.commit_fail_closed_column, "commit_fail_closed_column"},
        {config.commit_decision_written_column,
         "commit_decision_written_column"},
        {config.commit_evidence_written_column,
         "commit_evidence_written_column"},
        {config.commit_ts_column, "commit_ts_column"},
        {config.ownership_table, "ownership_table"},
        {config.ownership_supervisor_column, "ownership_supervisor_column"},
        {config.ownership_owner_id_column, "ownership_owner_id_column"},
        {config.ownership_epoch_column, "ownership_epoch_column"},
        {config.ownership_lease_expires_at_column,
         "ownership_lease_expires_at_column"},
        {config.ownership_heartbeat_ts_column, "ownership_heartbeat_ts_column"},
    };
    for (const auto& [value, label] : identifiers) {
        if (!require_identifier(value, label, error)) return false;
    }
    if (config.manage_worker_lease && !config.require_worker_ownership) {
        if (error) *error = "manage_worker_lease requires worker ownership";
        return false;
    }
    if (config.require_worker_ownership && config.worker_owner_id.empty()) {
        if (error) *error = "worker_owner_id is required when ownership is enabled";
        return false;
    }
    if (config.manage_worker_lease) {
        constexpr uint64_t kMaxLeaseTtlMs =
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max() / 1000000LL);
        if (config.worker_lease_ttl_ms == 0 ||
            config.worker_lease_ttl_ms > kMaxLeaseTtlMs) {
            if (error) *error = "worker_lease_ttl_ms must be positive and fit in int64 nanoseconds";
            return false;
        }
    }
    const size_t proposal_limit =
        config.proposal_query_limit == 0
            ? runtime.batch_limit
            : config.proposal_query_limit;
    if (proposal_limit == 0) {
        if (error) *error = "proposal_query_limit or runtime.batch_limit must be positive";
        return false;
    }
    if (config.history_evidence_limit == 0) {
        if (error) *error = "history_evidence_limit must be positive";
        return false;
    }
    if (config.suppress_min_failure_count == 0) {
        if (error) *error = "suppress_min_failure_count must be positive";
        return false;
    }
    return true;
}

bool ensureActionOutcomeSqlTables(QueryExecutor& executor,
                                  const ActionOutcomeSqlAdapterConfig& config,
                                  std::string* error) {
    if (!validateActionOutcomeSqlAdapterConfig(config, error)) return false;
    return execute_ok(executor, create_proposal_table_sql(config),
                      "create proposal table", error) &&
           execute_ok(executor, create_history_table_sql(config),
                      "create history table", error) &&
           execute_ok(executor, create_decision_table_sql(config),
                      "create decision table", error) &&
           execute_ok(executor, create_evidence_table_sql(config),
                      "create evidence table", error) &&
           execute_ok(executor, create_commit_table_sql(config),
                      "create commit table", error) &&
           execute_ok(executor, create_ownership_table_sql(config),
                      "create ownership table", error);
}

zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks
makeActionOutcomeSqlRuntimeHooks(QueryExecutor& executor,
                                 ActionOutcomeSqlAdapterConfig config) {
    std::string config_error;
    if (!validateActionOutcomeSqlAdapterConfig(config, &config_error)) {
        throw std::invalid_argument(config_error);
    }

    zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks hooks;
    auto lease_state = std::make_shared<WorkerLeaseState>(
        config.worker_owner_epoch);

    hooks.load_proposals = [&executor, config, lease_state]() {
        ActionOutcomeProposalLoadResult out;
        bool ownership_allows_work = false;
        if (!worker_ownership_allows_processing(
                executor,
                config,
                lease_state.get(),
                &ownership_allows_work,
                &out.error)) {
            out.ok = false;
            return out;
        }
        if (!ownership_allows_work) {
            out.ok = true;
            return out;
        }

        const size_t limit = config.proposal_query_limit == 0
            ? config.runtime.batch_limit
            : config.proposal_query_limit;
        auto base_select = [&config]() {
            std::ostringstream sql;
            sql << "SELECT " << config.proposal_id_column << ", "
                << config.proposal_source_type_column << ", "
                << config.proposal_action_column << ", "
                << config.proposal_ts_column
                << " FROM " << config.runtime.proposal_table;
            return sql.str();
        };
        auto commit_subquery = [&config]() {
            std::ostringstream sql;
            sql << "SELECT " << config.commit_proposal_id_column
                << " FROM " << config.commit_table;
            return sql.str();
        };
        auto append_query = [&](const std::string& sql,
                                const std::string& context,
                                std::unordered_set<std::string>* seen_ids) {
            const auto result = executor.execute(sql);
            if (!result.ok()) {
                out.ok = false;
                out.error = context + ": " + result.error;
                return false;
            }
            append_proposal_rows(result, &out.proposals, seen_ids);
            return true;
        };

        std::unordered_set<std::string> seen_ids;
        bool commit_table_has_rows = false;
        if (!table_has_rows(
                executor,
                config.commit_table,
                "commit ledger count failed",
                &commit_table_has_rows,
                &out.error)) {
            out.ok = false;
            return out;
        }
        if (commit_table_has_rows) {
            std::ostringstream sql;
            sql << base_select()
                << " WHERE " << config.proposal_id_column
                << " IN (" << commit_subquery() << ")"
                << " ORDER BY " << config.proposal_ts_column << " ASC"
                << " LIMIT " << limit;
            if (!append_query(
                    sql.str(), "proposal repair-candidate query failed",
                    &seen_ids)) {
                return out;
            }
        }
        {
            std::ostringstream sql;
            sql << base_select();
            if (commit_table_has_rows) {
                sql << " WHERE NOT " << config.proposal_id_column
                    << " IN (" << commit_subquery() << ")";
            }
            sql << " ORDER BY " << config.proposal_ts_column << " ASC"
                << " LIMIT " << limit;
            if (!append_query(sql.str(), "proposal query failed", &seen_ids)) {
                return out;
            }
        }
        out.ok = true;
        return out;
    };

    hooks.already_decided = [&executor, config](const std::string& proposal_id) {
        std::string error;
        CommittedDecisionState committed;
        if (!load_committed_decision(
                executor, config, proposal_id, &committed, &error)) {
            throw std::runtime_error(error);
        }
        if (!committed.exists) return false;
        if (!repair_projection_from_commit(executor, config, committed, &error)) {
            throw std::runtime_error(error);
        }
        return true;
    };

    hooks.decide = [&executor, config](const ActionOutcomeProposal& proposal) {
        ActionOutcomeDecisionResult out;
        const auto action_code =
            executor.intern_symbol_for_ingest(proposal.proposed_action);
        std::ostringstream sql;
        sql << "SELECT " << config.history_action_column << ", "
            << config.history_outcome_score_column
            << " FROM " << config.runtime.history_table
            << " WHERE " << config.history_action_column
            << " = " << static_cast<int64_t>(action_code)
            << " LIMIT " << config.history_evidence_limit;

        const auto result = executor.execute(sql.str());
        if (!result.ok()) {
            out.ok = false;
            out.error = "history query failed: " + result.error;
            return out;
        }

        const size_t rows = row_count(result);
        if (rows == 0) {
            out.ok = true;
            out.decision = no_evidence_decision(proposal, config);
            return out;
        }

        uint64_t failure_count = 0;
        for (size_t r = 0; r < rows; ++r) {
            const int64_t score = cell_i64(result, r, 1);
            if (score < config.suppress_outcome_score_below) {
                ++failure_count;
            }
        }

        out.ok = true;
        out.decision.proposal_id = proposal.proposal_id;
        out.decision.evidence_count = static_cast<uint64_t>(rows);
        if (failure_count >= config.suppress_min_failure_count) {
            out.decision.decision = "suppress_historical_failure";
            out.decision.final_action = config.runtime.fail_closed_action;
            out.decision.reason =
                "historical_negative_outcome_count=" +
                std::to_string(failure_count);
            out.decision.fail_closed = false;
        } else {
            out.decision.decision = "allow";
            out.decision.final_action = proposal.proposed_action;
            out.decision.reason = "historical_outcomes_non_negative";
            out.decision.fail_closed = false;
        }
        return out;
    };

    hooks.sink_decision = [&executor, config](const ActionOutcomeDecision& decision,
                                              std::string* error) {
        const int64_t ts = now_ns();
        CommittedDecisionState committed;
        if (!load_committed_decision(
                executor, config, decision.proposal_id, &committed, error)) {
            return false;
        }
        if (!committed.exists) {
            if (!insert_atomic_commit_row(
                executor,
                config,
                decision,
                ts,
                error)) {
                return false;
            }
            committed.exists = true;
            committed.decision = decision;
            committed.committed_ts_ns = ts;
        }
        return repair_projection_from_commit(executor, config, committed, error);
    };

    return hooks;
}

} // namespace zeptodb::server
