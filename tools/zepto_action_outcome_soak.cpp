// ============================================================================
// ZeptoDB: Action-Outcome supervisor SQL soak and fault-injection harness
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/feeds/action_outcome_supervisor_runtime.h"
#include "zeptodb/server/action_outcome_sql_adapter.h"
#include "zeptodb/sql/executor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::feeds::ActionOutcomeSupervisorRuntime;
using zeptodb::server::ActionOutcomeSqlAdapterConfig;
using zeptodb::sql::QueryExecutor;
using zeptodb::sql::QueryResultSet;
using zeptodb::storage::ColumnType;

struct Options {
    uint64_t duration_sec = 60;
    uint64_t iterations = 0;
    uint64_t proposals_per_pass = 4;
    uint64_t fault_every = 10;
    uint64_t sleep_ms = 100;
    uint64_t batch_limit = 32;
    uint64_t proposal_query_limit = 100000;
};

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "  --duration-sec N        Wall-clock soak duration (default: 60)\n"
        << "  --iterations N          Stop after N passes; 0 means duration-driven\n"
        << "  --proposals-per-pass N  New proposals inserted before each pass (default: 4)\n"
        << "  --fault-every N         Malform evidence projection every N passes; 0 disables\n"
        << "  --sleep-ms N            Delay between passes (default: 100)\n"
        << "  --batch-limit N         Runtime batch limit (default: 32)\n"
        << "  --proposal-query-limit N SQL proposal query limit (default: 100000)\n";
}

bool parse_u64(const char* value, uint64_t* out) {
    if (value == nullptr || *value == '\0') return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0') return false;
    *out = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](uint64_t* target) {
            if (i + 1 >= argc || !parse_u64(argv[++i], target)) {
                std::cerr << "Invalid value for " << arg << "\n";
                return false;
            }
            return true;
        };
        if (arg == "--duration-sec") {
            if (!need_value(&options->duration_sec)) return false;
        } else if (arg == "--iterations") {
            if (!need_value(&options->iterations)) return false;
        } else if (arg == "--proposals-per-pass") {
            if (!need_value(&options->proposals_per_pass)) return false;
        } else if (arg == "--fault-every") {
            if (!need_value(&options->fault_every)) return false;
        } else if (arg == "--sleep-ms") {
            if (!need_value(&options->sleep_ms)) return false;
        } else if (arg == "--batch-limit") {
            if (!need_value(&options->batch_limit)) return false;
        } else if (arg == "--proposal-query-limit") {
            if (!need_value(&options->proposal_query_limit)) return false;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    if (options->proposals_per_pass == 0 || options->batch_limit == 0 ||
        options->proposal_query_limit == 0) {
        std::cerr << "proposals-per-pass, batch-limit, and proposal-query-limit must be positive\n";
        return false;
    }
    if (options->proposals_per_pass > options->batch_limit) {
        std::cerr << "proposals-per-pass must not exceed batch-limit\n";
        return false;
    }
    return true;
}

size_t row_count(const QueryResultSet& result) {
    return std::max(result.rows.size(), result.typed_rows.size());
}

int64_t cell_i64(const QueryResultSet& result, size_t row, size_t col) {
    if (row < result.typed_rows.size() && col < result.typed_rows[row].size()) {
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
    return std::to_string(cell_i64(result, row, col));
}

bool exec_ok(QueryExecutor& executor,
             const std::string& sql,
             const std::string& context) {
    const auto result = executor.execute(sql);
    if (result.ok()) return true;
    std::cerr << context << " failed: " << result.error << "\nSQL: " << sql << "\n";
    return false;
}

int64_t table_count(QueryExecutor& executor, const std::string& table) {
    const auto result = executor.execute("SELECT count(*) FROM " + table);
    if (!result.ok() || result.rows.empty() || result.rows[0].empty()) {
        std::cerr << "count failed for " << table << ": " << result.error << "\n";
        return -1;
    }
    return result.rows[0][0];
}

bool create_evidence_projection(QueryExecutor& executor) {
    return exec_ok(executor,
                   "CREATE TABLE physical_ai_supervision_evidence "
                   "(proposal_id STRING, evidence_count INT64, reason STRING, "
                   "written_ts_ns TIMESTAMP_NS)",
                   "create evidence projection");
}

bool reset_evidence_projection(QueryExecutor& executor, bool malformed) {
    if (!exec_ok(executor,
                 "DROP TABLE IF EXISTS physical_ai_supervision_evidence",
                 "drop evidence projection")) {
        return false;
    }
    if (malformed) {
        return exec_ok(executor,
                       "CREATE TABLE physical_ai_supervision_evidence "
                       "(proposal_id STRING)",
                       "create malformed evidence projection");
    }
    return create_evidence_projection(executor);
}

bool insert_fixture_history(QueryExecutor& executor) {
    return exec_ok(executor,
                   "INSERT INTO physical_ai_action_history "
                   "(action, outcome_score, source_ts_ns) VALUES "
                   "('continue_route', 10, 10),"
                   "('rollback', -10, 20)",
                   "insert history");
}

bool insert_proposals(QueryExecutor& executor,
                      uint64_t pass,
                      uint64_t proposals_per_pass,
                      uint64_t* expected_total) {
    std::string sql =
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES ";
    for (uint64_t i = 0; i < proposals_per_pass; ++i) {
        if (i != 0) sql += ",";
        const uint64_t ordinal = *expected_total + i;
        const std::string action = (ordinal % 2 == 0) ? "continue_route" : "rollback";
        sql += "('soak_p" + std::to_string(ordinal) + "', 'soak_fault', '" +
               action + "', " + std::to_string(pass * 1000000 + i) + ")";
    }
    if (!exec_ok(executor, sql, "insert proposals")) return false;
    *expected_total += proposals_per_pass;
    return true;
}

bool unique_commit_ids_match(QueryExecutor& executor, int64_t expected) {
    const auto result = executor.execute(
        "SELECT proposal_id FROM physical_ai_supervision_commits");
    if (!result.ok()) {
        std::cerr << "commit id scan failed: " << result.error << "\n";
        return false;
    }
    std::unordered_set<std::string> ids;
    const size_t rows = row_count(result);
    for (size_t r = 0; r < rows; ++r) {
        ids.insert(cell_string(result, r, 0));
    }
    if (static_cast<int64_t>(ids.size()) != expected ||
        static_cast<int64_t>(rows) != expected) {
        std::cerr << "commit uniqueness mismatch: rows=" << rows
                  << " unique=" << ids.size()
                  << " expected=" << expected << "\n";
        return false;
    }
    return true;
}

bool verify_counts(QueryExecutor& executor, uint64_t expected_total) {
    const int64_t expected = static_cast<int64_t>(expected_total);
    const int64_t commits =
        table_count(executor, "physical_ai_supervision_commits");
    const int64_t decisions =
        table_count(executor, "physical_ai_supervision_decisions");
    const int64_t evidence =
        table_count(executor, "physical_ai_supervision_evidence");
    if (commits != expected || decisions != expected || evidence != expected) {
        std::cerr << "projection mismatch: commits=" << commits
                  << " decisions=" << decisions
                  << " evidence=" << evidence
                  << " expected=" << expected << "\n";
        return false;
    }
    return unique_commit_ids_match(executor, expected);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }

    PipelineConfig pipeline_config;
    pipeline_config.storage_mode = StorageMode::PURE_IN_MEMORY;
    auto pipeline = std::make_unique<ZeptoPipeline>(pipeline_config);
    pipeline->start();
    QueryExecutor executor(*pipeline);

    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = static_cast<size_t>(options.batch_limit);
    config.runtime.max_sink_errors_per_pass =
        static_cast<uint32_t>(std::max<uint64_t>(options.batch_limit, 1));
    config.proposal_query_limit = static_cast<size_t>(options.proposal_query_limit);

    std::string error;
    if (!zeptodb::server::ensureActionOutcomeSqlTables(executor, config, &error)) {
        std::cerr << "ensure contract failed: " << error << "\n";
        return 1;
    }
    if (!insert_fixture_history(executor)) return 1;

    ActionOutcomeSupervisorRuntime runtime;
    if (!runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(executor, config),
            &error) ||
        !runtime.configure(config.runtime, &error) ||
        !runtime.start(&error)) {
        std::cerr << "runtime setup failed: " << error << "\n";
        return 1;
    }

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(options.duration_sec);
    uint64_t pass = 0;
    uint64_t expected_total = 0;
    uint64_t injected_faults = 0;
    uint64_t repaired_faults = 0;
    while (options.iterations == 0 || pass < options.iterations) {
        if (options.iterations == 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        ++pass;
        if (!insert_proposals(
                executor, pass, options.proposals_per_pass, &expected_total)) {
            return 1;
        }

        const bool inject_fault =
            options.fault_every != 0 && pass % options.fault_every == 0;
        if (inject_fault) {
            ++injected_faults;
            if (!reset_evidence_projection(executor, true)) return 1;
            if (runtime.runOnce(&error)) {
                std::cerr << "fault pass unexpectedly succeeded\n";
                return 1;
            }
            if (error.find("insert evidence summary") == std::string::npos) {
                std::cerr << "fault pass failed for unexpected reason: "
                          << error << "\n";
                return 1;
            }
            if (!reset_evidence_projection(executor, false)) return 1;
            if (!runtime.runOnce(&error)) {
                std::cerr << "repair pass failed: " << error << "\n";
                return 1;
            }
            ++repaired_faults;
        } else if (!runtime.runOnce(&error)) {
            std::cerr << "pass failed: " << error << "\n";
            return 1;
        }

        if (!verify_counts(executor, expected_total)) return 1;
        if (options.sleep_ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.sleep_ms));
        }
    }

    if (!runtime.stop(&error)) {
        std::cerr << "runtime stop failed: " << error << "\n";
        return 1;
    }
    if (!verify_counts(executor, expected_total)) return 1;

    const auto snap = runtime.snapshot();
    std::cout << "Action-Outcome soak PASS\n"
              << "passes=" << pass
              << " proposals=" << expected_total
              << " faults_injected=" << injected_faults
              << " faults_repaired=" << repaired_faults
              << " worker_failures=" << snap.worker_failures_total
              << " duplicates=" << snap.proposals_duplicate_total
              << " processed=" << snap.proposals_processed_total
              << "\n";
    pipeline->stop();
    return 0;
}
