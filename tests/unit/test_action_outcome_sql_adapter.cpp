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

    int64_t table_count(const std::string& table_name) {
        const auto result = executor_->execute("SELECT count(*) FROM " + table_name);
        EXPECT_TRUE(result.ok()) << result.error;
        if (!result.ok() || result.rows.empty() || result.rows[0].empty()) return -1;
        return result.rows[0][0];
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
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.commit_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.ownership_table));
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

TEST_F(ActionOutcomeSqlAdapterTest,
       UndecidedProposalsProgressPastCommittedPrefixAtQueryLimit) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 4;
    config.proposal_query_limit = 4;
    ensure_contract(config);

    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 10, 10)");
    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p0', 'soak', 'continue_route', 0),"
        "('p1', 'soak', 'continue_route', 1),"
        "('p2', 'soak', 'continue_route', 2),"
        "('p3', 'soak', 'continue_route', 3)");

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.processed_count, 4u);
    EXPECT_EQ(table_count(config.commit_table), 4);

    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p4', 'soak', 'continue_route', 4),"
        "('p5', 'soak', 'continue_route', 5),"
        "('p6', 'soak', 'continue_route', 6),"
        "('p7', 'soak', 'continue_route', 7)");
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.duplicate_count, 4u);
    EXPECT_EQ(snap.last_pass.processed_count, 4u);
    EXPECT_EQ(table_count(config.commit_table), 8);
    EXPECT_EQ(table_count(config.runtime.decision_table), 8);
    EXPECT_EQ(table_count(config.runtime.evidence_table), 8);
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

TEST_F(ActionOutcomeSqlAdapterTest, RestartedRuntimeSkipsPersistedDecisions) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 2;
    ensure_contract(config);

    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_agv_continue', 'shadow_ab', 'continue_route', 100),"
        "('p_arm_torque', 'shadow_ab', 'increase_torque_limit', 200)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', -10, 10),"
        "('increase_torque_limit', -20, 20)");

    std::string error;
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        ASSERT_TRUE(runtime.runOnce(&error)) << error;

        const auto snap = runtime.snapshot();
        EXPECT_EQ(snap.last_pass.processed_count, 2u);
        EXPECT_EQ(snap.last_pass.duplicate_count, 0u);
        EXPECT_EQ(snap.decisions_suppress_total, 2u);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    ASSERT_EQ(table_count(config.runtime.decision_table), 2);
    ASSERT_EQ(table_count(config.runtime.evidence_table), 2);
    ASSERT_EQ(table_count(config.commit_table), 2);

    {
        ActionOutcomeSupervisorRuntime restarted;
        ASSERT_TRUE(restarted.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(restarted.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(restarted.start(&error)) << error;
        ASSERT_TRUE(restarted.runOnce(&error)) << error;

        const auto snap = restarted.snapshot();
        EXPECT_EQ(snap.last_pass.processed_count, 0u);
        EXPECT_EQ(snap.last_pass.duplicate_count, 2u);
        EXPECT_EQ(snap.evidence_rows_written_total, 0u);
        ASSERT_TRUE(restarted.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(config.runtime.decision_table), 2);
    EXPECT_EQ(table_count(config.runtime.evidence_table), 2);
    EXPECT_EQ(table_count(config.commit_table), 2);
}

TEST_F(ActionOutcomeSqlAdapterTest,
       RetryAfterAtomicCommitFailureDoesNotWriteProjection) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 1;

    exec_ok(
        "CREATE TABLE physical_ai_action_proposals "
        "(proposal_id STRING, source_type STRING, proposed_action STRING, "
        "source_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "CREATE TABLE physical_ai_action_history "
        "(action STRING, outcome_score INT64, source_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_evidence "
        "(proposal_id STRING, evidence_count INT64, reason STRING, "
        "written_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_commits "
        "(proposal_id STRING, decision_written BOOL, evidence_written BOOL, "
        "committed_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_decisions "
        "(proposal_id STRING, decision STRING, final_action STRING, "
        "reason STRING, evidence_count INT64, fail_closed BOOL, "
        "decided_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_retry', 'fault_injection', 'continue_route', 100)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 5, 10)");

    std::string error;
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        EXPECT_FALSE(runtime.runOnce(&error));
        EXPECT_NE(error.find("insert atomic sink commit"), std::string::npos);

        const auto snap = runtime.snapshot();
        EXPECT_EQ(snap.last_pass.processed_count, 0u);
        EXPECT_EQ(snap.last_pass.sink_error_count, 1u);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    ASSERT_EQ(table_count(config.runtime.evidence_table), 0);
    ASSERT_EQ(table_count(config.runtime.decision_table), 0);
    ASSERT_EQ(table_count(config.commit_table), 0);

    exec_ok("DROP TABLE physical_ai_supervision_commits");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_commits "
        "(proposal_id STRING, decision STRING, final_action STRING, "
        "reason STRING, evidence_count INT64, fail_closed BOOL, "
        "decision_written BOOL, evidence_written BOOL, "
        "committed_ts_ns TIMESTAMP_NS)");

    {
        ActionOutcomeSupervisorRuntime retry;
        ASSERT_TRUE(retry.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(retry.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(retry.start(&error)) << error;
        ASSERT_TRUE(retry.runOnce(&error)) << error;

        const auto snap = retry.snapshot();
        EXPECT_EQ(snap.last_pass.processed_count, 1u);
        EXPECT_EQ(snap.last_pass.sink_error_count, 0u);
        ASSERT_TRUE(retry.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(config.runtime.evidence_table), 1);
    EXPECT_EQ(table_count(config.runtime.decision_table), 1);
    EXPECT_EQ(table_count(config.commit_table), 1);
}

TEST_F(ActionOutcomeSqlAdapterTest,
       CommittedDecisionRepairsProjectionAfterSinkFailure) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 1;

    exec_ok(
        "CREATE TABLE physical_ai_action_proposals "
        "(proposal_id STRING, source_type STRING, proposed_action STRING, "
        "source_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "CREATE TABLE physical_ai_action_history "
        "(action STRING, outcome_score INT64, source_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_decisions "
        "(proposal_id STRING, decision STRING, final_action STRING, "
        "reason STRING, evidence_count INT64, fail_closed BOOL, "
        "decided_ts_ns TIMESTAMP_NS)");
    exec_ok("CREATE TABLE physical_ai_supervision_evidence (proposal_id STRING)");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_commits "
        "(proposal_id STRING, decision STRING, final_action STRING, "
        "reason STRING, evidence_count INT64, fail_closed BOOL, "
        "decision_written BOOL, evidence_written BOOL, "
        "committed_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_projection_retry', 'fault_injection', 'continue_route', 100)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 5, 10)");

    std::string error;
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        EXPECT_FALSE(runtime.runOnce(&error));
        EXPECT_NE(error.find("insert evidence summary"), std::string::npos);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    ASSERT_EQ(table_count(config.commit_table), 1);
    ASSERT_EQ(table_count(config.runtime.decision_table), 0);
    ASSERT_EQ(table_count(config.runtime.evidence_table), 0);

    exec_ok("DROP TABLE physical_ai_supervision_evidence");
    exec_ok(
        "CREATE TABLE physical_ai_supervision_evidence "
        "(proposal_id STRING, evidence_count INT64, reason STRING, "
        "written_ts_ns TIMESTAMP_NS)");

    {
        ActionOutcomeSupervisorRuntime retry;
        ASSERT_TRUE(retry.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(retry.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(retry.start(&error)) << error;
        ASSERT_TRUE(retry.runOnce(&error)) << error;

        const auto snap = retry.snapshot();
        EXPECT_EQ(snap.last_pass.duplicate_count, 1u);
        EXPECT_EQ(snap.last_pass.processed_count, 0u);
        ASSERT_TRUE(retry.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(config.commit_table), 1);
    EXPECT_EQ(table_count(config.runtime.decision_table), 1);
    EXPECT_EQ(table_count(config.runtime.evidence_table), 1);
}

TEST_F(ActionOutcomeSqlAdapterTest,
       PartialDecisionStateRepairsEvidenceAndCommitWithoutDuplicateDecision) {
    ActionOutcomeSqlAdapterConfig config;
    config.runtime.batch_limit = 1;
    ensure_contract(config);

    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_repair', 'fault_injection', 'continue_route', 100)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 5, 10)");
    exec_ok(
        "INSERT INTO physical_ai_supervision_decisions "
        "(proposal_id, decision, final_action, reason, evidence_count, "
        "fail_closed, decided_ts_ns) VALUES "
        "('p_repair', 'allow', 'continue_route', 'preexisting_decision', "
        "1, 0, 123)");

    std::string error;
    ActionOutcomeSupervisorRuntime runtime;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeActionOutcomeSqlRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    EXPECT_EQ(table_count(config.runtime.decision_table), 1);
    EXPECT_EQ(table_count(config.runtime.evidence_table), 1);
    EXPECT_EQ(table_count(config.commit_table), 1);
    EXPECT_EQ(runtime.snapshot().last_pass.processed_count, 1u);
}

TEST_F(ActionOutcomeSqlAdapterTest,
       WorkerOwnershipGateSkipsNonOwnerAndAllowsCurrentOwner) {
    ActionOutcomeSqlAdapterConfig owner_config;
    owner_config.runtime.batch_limit = 1;
    owner_config.require_worker_ownership = true;
    owner_config.worker_owner_id = "node-a";
    owner_config.worker_owner_epoch = 7;
    ensure_contract(owner_config);

    exec_ok(
        "INSERT INTO physical_ai_supervisor_ownership "
        "(supervisor_name, owner_id, owner_epoch) VALUES "
        "('physical_ai_action_outcome', 'node-a', 7)");
    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_owned', 'cluster_gate', 'continue_route', 100)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 10, 10)");

    std::string error;
    ActionOutcomeSqlAdapterConfig non_owner_config = owner_config;
    non_owner_config.worker_owner_id = "node-b";
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, non_owner_config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(non_owner_config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        ASSERT_TRUE(runtime.runOnce(&error)) << error;

        const auto snap = runtime.snapshot();
        EXPECT_EQ(snap.last_pass.batch_proposals, 0u);
        EXPECT_EQ(snap.last_pass.processed_count, 0u);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    ActionOutcomeSqlAdapterConfig stale_owner_config = owner_config;
    stale_owner_config.worker_owner_epoch = 6;
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, stale_owner_config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(stale_owner_config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        ASSERT_TRUE(runtime.runOnce(&error)) << error;

        const auto snap = runtime.snapshot();
        EXPECT_EQ(snap.last_pass.batch_proposals, 0u);
        EXPECT_EQ(snap.last_pass.processed_count, 0u);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(owner_config.runtime.decision_table), 0);
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, owner_config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(owner_config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        ASSERT_TRUE(runtime.runOnce(&error)) << error;

        const auto snap = runtime.snapshot();
        EXPECT_EQ(snap.last_pass.processed_count, 1u);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(owner_config.runtime.decision_table), 1);
    EXPECT_EQ(table_count(owner_config.runtime.evidence_table), 1);
}

TEST_F(ActionOutcomeSqlAdapterTest,
       WorkerOwnershipEpochHandoffFencesReplacedOwner) {
    ActionOutcomeSqlAdapterConfig node_a_config;
    node_a_config.runtime.batch_limit = 4;
    node_a_config.require_worker_ownership = true;
    node_a_config.worker_owner_id = "node-a";
    node_a_config.worker_owner_epoch = 1;
    ensure_contract(node_a_config);

    exec_ok(
        "INSERT INTO physical_ai_supervisor_ownership "
        "(supervisor_name, owner_id, owner_epoch) VALUES "
        "('physical_ai_action_outcome', 'node-a', 1)");
    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_before_handoff', 'cluster_gate', 'continue_route', 100)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 10, 10)");

    std::string error;
    {
        ActionOutcomeSupervisorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, node_a_config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(node_a_config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        ASSERT_TRUE(runtime.runOnce(&error)) << error;
        EXPECT_EQ(runtime.snapshot().last_pass.processed_count, 1u);
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    exec_ok("DROP TABLE physical_ai_supervisor_ownership");
    exec_ok(
        "CREATE TABLE physical_ai_supervisor_ownership "
        "(supervisor_name STRING, owner_id STRING, owner_epoch INT64)");
    exec_ok(
        "INSERT INTO physical_ai_supervisor_ownership "
        "(supervisor_name, owner_id, owner_epoch) VALUES "
        "('physical_ai_action_outcome', 'node-b', 2)");
    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_after_handoff', 'cluster_gate', 'continue_route', 200)");

    {
        ActionOutcomeSupervisorRuntime stale_node_a;
        ASSERT_TRUE(stale_node_a.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, node_a_config),
            &error)) << error;
        ASSERT_TRUE(stale_node_a.configure(node_a_config.runtime, &error)) << error;
        ASSERT_TRUE(stale_node_a.start(&error)) << error;
        ASSERT_TRUE(stale_node_a.runOnce(&error)) << error;
        EXPECT_EQ(stale_node_a.snapshot().last_pass.batch_proposals, 0u);
        ASSERT_TRUE(stale_node_a.stop(&error)) << error;
    }

    ActionOutcomeSqlAdapterConfig node_b_config = node_a_config;
    node_b_config.worker_owner_id = "node-b";
    node_b_config.worker_owner_epoch = 2;
    {
        ActionOutcomeSupervisorRuntime node_b;
        ASSERT_TRUE(node_b.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, node_b_config),
            &error)) << error;
        ASSERT_TRUE(node_b.configure(node_b_config.runtime, &error)) << error;
        ASSERT_TRUE(node_b.start(&error)) << error;
        ASSERT_TRUE(node_b.runOnce(&error)) << error;

        const auto snap = node_b.snapshot();
        EXPECT_EQ(snap.last_pass.duplicate_count, 1u);
        EXPECT_EQ(snap.last_pass.processed_count, 1u);
        ASSERT_TRUE(node_b.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(node_a_config.runtime.decision_table), 2);
    EXPECT_EQ(table_count(node_a_config.runtime.evidence_table), 2);
}

TEST_F(ActionOutcomeSqlAdapterTest,
       WorkerLeaseAcquiresRenewsAndTakesOverExpiredOwner) {
    ActionOutcomeSqlAdapterConfig node_a_config;
    node_a_config.runtime.batch_limit = 1;
    node_a_config.require_worker_ownership = true;
    node_a_config.manage_worker_lease = true;
    node_a_config.worker_owner_id = "node-a";
    node_a_config.worker_owner_epoch = 1;
    node_a_config.worker_lease_ttl_ms = 1000;
    ensure_contract(node_a_config);

    exec_ok(
        "INSERT INTO physical_ai_action_proposals "
        "(proposal_id, source_type, proposed_action, source_ts_ns) VALUES "
        "('p_lease_a', 'cluster_lease', 'continue_route', 100),"
        "('p_lease_b', 'cluster_lease', 'continue_route', 200)");
    exec_ok(
        "INSERT INTO physical_ai_action_history "
        "(action, outcome_score, source_ts_ns) VALUES "
        "('continue_route', 10, 10)");

    std::string error;
    {
        ActionOutcomeSupervisorRuntime node_a;
        ASSERT_TRUE(node_a.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, node_a_config),
            &error)) << error;
        ASSERT_TRUE(node_a.configure(node_a_config.runtime, &error)) << error;
        ASSERT_TRUE(node_a.start(&error)) << error;
        ASSERT_TRUE(node_a.runOnce(&error)) << error;
        EXPECT_EQ(node_a.snapshot().last_pass.processed_count, 1u);
        ASSERT_TRUE(node_a.stop(&error)) << error;
    }

    const auto lease_after_a = executor_->execute(
        "SELECT owner_id, owner_epoch, lease_expires_at_ns "
        "FROM physical_ai_supervisor_ownership");
    ASSERT_TRUE(lease_after_a.ok()) << lease_after_a.error;
    ASSERT_EQ(lease_after_a.rows.size(), 1u);
    EXPECT_EQ(string_cell(lease_after_a, 0, 0), "node-a");
    EXPECT_EQ(int_cell(lease_after_a, 0, 1), 1);
    EXPECT_GT(int_cell(lease_after_a, 0, 2), 0);

    exec_ok("DROP TABLE physical_ai_supervisor_ownership");
    exec_ok(
        "CREATE TABLE physical_ai_supervisor_ownership "
        "(supervisor_name STRING, owner_id STRING, owner_epoch INT64, "
        "lease_expires_at_ns INT64, heartbeat_ts_ns TIMESTAMP_NS)");
    exec_ok(
        "INSERT INTO physical_ai_supervisor_ownership "
        "(supervisor_name, owner_id, owner_epoch, lease_expires_at_ns, "
        "heartbeat_ts_ns) VALUES "
        "('physical_ai_action_outcome', 'node-a', 1, 1, 1)");

    ActionOutcomeSqlAdapterConfig node_b_config = node_a_config;
    node_b_config.worker_owner_id = "node-b";
    node_b_config.worker_owner_epoch = 1;
    node_b_config.proposal_query_limit = 2;
    {
        ActionOutcomeSupervisorRuntime node_b;
        ASSERT_TRUE(node_b.setWorkerHooks(
            zeptodb::server::makeActionOutcomeSqlRuntimeHooks(
                *executor_, node_b_config),
            &error)) << error;
        ASSERT_TRUE(node_b.configure(node_b_config.runtime, &error)) << error;
        ASSERT_TRUE(node_b.start(&error)) << error;
        ASSERT_TRUE(node_b.runOnce(&error)) << error;
        EXPECT_EQ(node_b.snapshot().last_pass.processed_count, 1u);
        ASSERT_TRUE(node_b.stop(&error)) << error;
    }

    const auto lease_after_b = executor_->execute(
        "SELECT owner_id, owner_epoch FROM physical_ai_supervisor_ownership");
    ASSERT_TRUE(lease_after_b.ok()) << lease_after_b.error;
    ASSERT_EQ(lease_after_b.rows.size(), 1u);
    EXPECT_EQ(string_cell(lease_after_b, 0, 0), "node-b");
    EXPECT_EQ(int_cell(lease_after_b, 0, 1), 2);
    EXPECT_EQ(table_count(node_a_config.runtime.decision_table), 2);
    EXPECT_EQ(table_count(node_a_config.commit_table), 2);
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

    config.history_evidence_limit = 64;
    config.require_worker_ownership = true;
    config.worker_owner_id.clear();
    EXPECT_FALSE(zeptodb::server::validateActionOutcomeSqlAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("worker_owner_id"), std::string::npos);

    config.require_worker_ownership = false;
    config.manage_worker_lease = true;
    config.worker_owner_id = "node-a";
    EXPECT_FALSE(zeptodb::server::validateActionOutcomeSqlAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("manage_worker_lease"), std::string::npos);
}
