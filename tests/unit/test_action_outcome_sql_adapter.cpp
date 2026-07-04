// ============================================================================
// ZeptoDB: SQL-backed Action-Outcome supervisor adapter tests
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/feeds/action_outcome_supervisor_runtime.h"
#include "zeptodb/server/action_outcome_sql_adapter.h"
#include "zeptodb/sql/executor.h"

#include <memory>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

namespace {

using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::feeds::ActionOutcomeSupervisorRuntime;
using zeptodb::server::ActionOutcomeSqlAdapterConfig;
using zeptodb::sql::QueryExecutor;
using zeptodb::sql::QueryResultSet;

std::string string_cell(const QueryResultSet& result, size_t row, size_t col) {
    if (result.symbol_dict == nullptr || row >= result.rows.size() ||
        col >= result.rows[row].size()) {
        return {};
    }
    return std::string(result.symbol_dict->lookup(
        static_cast<uint32_t>(result.rows[row][col])));
}

int64_t int_cell(const QueryResultSet& result, size_t row, size_t col) {
    if (row >= result.rows.size() || col >= result.rows[row].size()) return 0;
    return result.rows[row][col];
}

class ActionOutcomeSqlAdapterTest : public ::testing::Test {
protected:
    void SetUp() override {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        pipeline_ = std::make_unique<ZeptoPipeline>(cfg);
        pipeline_->start();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
    }

    void TearDown() override {
        executor_.reset();
        if (pipeline_) {
            pipeline_->stop();
        }
        pipeline_.reset();
    }

    void ensure_contract(const ActionOutcomeSqlAdapterConfig& config) {
        std::string error;
        ASSERT_TRUE(zeptodb::server::ensureActionOutcomeSqlTables(
            *executor_, config, &error)) << error;
    }

    void exec_ok(const std::string& sql) {
        const auto result = executor_->execute(sql);
        ASSERT_TRUE(result.ok()) << result.error << "\nSQL: " << sql;
    }

    std::unique_ptr<ZeptoPipeline> pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
};

} // namespace

TEST_F(ActionOutcomeSqlAdapterTest, EnsureTablesCreatesDefaultContract) {
    ActionOutcomeSqlAdapterConfig config;
    ensure_contract(config);

    EXPECT_TRUE(pipeline_->schema_registry().exists(config.runtime.proposal_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.runtime.history_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.runtime.decision_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.runtime.evidence_table));
}

TEST_F(ActionOutcomeSqlAdapterTest, HooksLoadDecideSinkAndSkipDuplicatesFromSql) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 2;
    ensure_contract(config);

    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_allow', 'vla_log', 'continue_route', 200),"
        "('p_stop', 'ros2_bag', 'rollback', 100)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 10, 10),"
        "('rollback', -5, 11)");

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.processed_count, 2u);
    EXPECT_EQ(snap.decisions_allow_total, 1u);
    EXPECT_EQ(snap.decisions_suppress_total, 1u);
    EXPECT_EQ(snap.evidence_rows_written_total, 2u);

    const auto decisions = executor_->execute(
        "SELECT proposal_id, decision, final_action, reason, "
        "evidence_count, fail_closed "
        "FROM physical_ai_supervision_decisions");
    ASSERT_TRUE(decisions.ok()) << decisions.error;
    ASSERT_EQ(decisions.rows.size(), 2u);

    std::unordered_map<std::string, size_t> by_id;
    for (size_t r = 0; r < decisions.rows.size(); ++r) {
        by_id[string_cell(decisions, r, 0)] = r;
    }
    ASSERT_TRUE(by_id.count("p_allow"));
    ASSERT_TRUE(by_id.count("p_stop"));
    EXPECT_EQ(string_cell(decisions, by_id["p_allow"], 1), "allow");
    EXPECT_EQ(string_cell(decisions, by_id["p_allow"], 2), "continue_route");
    EXPECT_EQ(int_cell(decisions, by_id["p_allow"], 4), 1);
    EXPECT_EQ(string_cell(decisions, by_id["p_stop"], 1),
              "suppress_historical_failure");
    EXPECT_EQ(string_cell(decisions, by_id["p_stop"], 2), "manual_review");

    const auto evidence = executor_->execute(
        "SELECT count(*) FROM physical_ai_supervision_evidence");
    ASSERT_TRUE(evidence.ok()) << evidence.error;
    ASSERT_EQ(evidence.rows.size(), 1u);
    EXPECT_EQ(evidence.rows[0][0], 2);

    ASSERT_TRUE(runtime.runOnce(&error)) << error;
    snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.duplicate_count, 2u);
    EXPECT_EQ(snap.last_pass.processed_count, 0u);
}

TEST_F(ActionOutcomeSqlAdapterTest, NoEvidenceFailsClosedAndPersistsDecision) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 1;
    ensure_contract(config);

    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_unknown', 'sim_trace', 'untested_action', 1)");

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    const auto decisions = executor_->execute(
        "SELECT proposal_id, decision, final_action, reason, "
        "evidence_count, fail_closed "
        "FROM physical_ai_supervision_decisions");
    ASSERT_TRUE(decisions.ok()) << decisions.error;
    ASSERT_EQ(decisions.rows.size(), 1u);
    EXPECT_EQ(string_cell(decisions, 0, 0), "p_unknown");
    EXPECT_EQ(string_cell(decisions, 0, 1), "suppress_no_evidence");
    EXPECT_EQ(string_cell(decisions, 0, 2), "manual_review");
    EXPECT_EQ(string_cell(decisions, 0, 3), "no_historical_outcome_evidence");
    EXPECT_EQ(int_cell(decisions, 0, 4), 0);
    EXPECT_EQ(int_cell(decisions, 0, 5), 1);
}

TEST(ActionOutcomeSqlAdapterConfigTest, RejectsUnsafeIdentifiersAndLimits) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.proposal_table = "bad-table";
    std::string error;
    EXPECT_FALSE(zeptodb::server::validateActionOutcomeSqlAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("proposal_table"), std::string::npos);

    config.runtime.proposal_table = "physical_ai_action_proposals";
    config.history_evidence_limit = 0;
    EXPECT_FALSE(zeptodb::server::validateActionOutcomeSqlAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("history_evidence_limit"), std::string::npos);
}
