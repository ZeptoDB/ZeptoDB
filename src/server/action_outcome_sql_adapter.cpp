// ============================================================================
// ZeptoDB: SQL-backed Action-Outcome supervisor adapter
// ============================================================================

#include "zeptodb/server/action_outcome_sql_adapter.h"

#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
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
    };
    for (const auto& [value, label] : identifiers) {
        if (!require_identifier(value, label, error)) return false;
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
                      "create evidence table", error);
}

zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks
makeActionOutcomeSqlRuntimeHooks(QueryExecutor& executor,
                                 ActionOutcomeSqlAdapterConfig config) {
    std::string config_error;
    if (!validateActionOutcomeSqlAdapterConfig(config, &config_error)) {
        throw std::invalid_argument(config_error);
    }

    zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks hooks;

    hooks.load_proposals = [&executor, config]() {
        ActionOutcomeProposalLoadResult out;
        const size_t limit = config.proposal_query_limit == 0
            ? config.runtime.batch_limit
            : config.proposal_query_limit;
        std::ostringstream sql;
        sql << "SELECT " << config.proposal_id_column << ", "
            << config.proposal_source_type_column << ", "
            << config.proposal_action_column << ", "
            << config.proposal_ts_column
            << " FROM " << config.runtime.proposal_table
            << " ORDER BY " << config.proposal_ts_column << " ASC"
            << " LIMIT " << limit;
        const auto result = executor.execute(sql.str());
        if (!result.ok()) {
            out.ok = false;
            out.error = "proposal query failed: " + result.error;
            return out;
        }
        out.ok = true;
        const size_t rows = row_count(result);
        out.proposals.reserve(rows);
        for (size_t r = 0; r < rows; ++r) {
            ActionOutcomeProposal proposal;
            proposal.proposal_id = cell_string(result, r, 0);
            proposal.source_type = cell_string(result, r, 1);
            proposal.proposed_action = cell_string(result, r, 2);
            proposal.source_ts_ns = cell_i64(result, r, 3);
            out.proposals.push_back(std::move(proposal));
        }
        return out;
    };

    hooks.already_decided = [&executor, config](const std::string& proposal_id) {
        const auto code = executor.intern_symbol_for_ingest(proposal_id);
        std::ostringstream sql;
        sql << "SELECT " << config.decision_proposal_id_column
            << " FROM " << config.runtime.decision_table
            << " WHERE " << config.decision_proposal_id_column
            << " = " << static_cast<int64_t>(code)
            << " LIMIT 1";
        const auto result = executor.execute(sql.str());
        if (!result.ok()) {
            throw std::runtime_error("decision idempotency query failed: " +
                                     result.error);
        }
        return row_count(result) > 0 || !result.string_rows.empty();
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
        return execute_ok(executor, decision_sql, "insert decision", error);
    };

    return hooks;
}

} // namespace zeptodb::server
