// ============================================================================
// ZeptoDB: SQL/HTTP-backed edge/fleet connector adapter tests
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/feeds/edge_fleet_connector_runtime.h"
#include "zeptodb/server/edge_fleet_sql_http_adapter.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"

#include "test_port_helper.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {

using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::feeds::EdgeFleetConnectorRuntime;
using zeptodb::server::HttpServer;
using zeptodb::server::EdgeFleetSqlHttpAdapterConfig;
using zeptodb::sql::QueryExecutor;
using zeptodb::sql::QueryResultSet;
using namespace std::chrono_literals;

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

class EdgeFleetSqlHttpAdapterTest : public ::testing::Test {
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

    void ensure_contract(const EdgeFleetSqlHttpAdapterConfig& config) {
        std::string error;
        ASSERT_TRUE(zeptodb::server::ensureEdgeFleetSqlHttpTables(
            *executor_, config, &error)) << error;
    }

    void exec_ok(QueryExecutor& executor, const std::string& sql) {
        const auto result = executor.execute(sql);
        ASSERT_TRUE(result.ok()) << result.error << "\nSQL: " << sql;
    }

    void exec_ok(const std::string& sql) {
        exec_ok(*executor_, sql);
    }

    int64_t table_count(const std::string& table_name) {
        const auto result = executor_->execute("SELECT count(*) FROM " + table_name);
        EXPECT_TRUE(result.ok()) << result.error;
        if (!result.ok() || result.rows.empty() || result.rows[0].empty()) return -1;
        return result.rows[0][0];
    }

    void insert_outbox_row(const std::string& event_id,
                           uint64_t stream_seq,
                           const std::string& event_kind,
                           int64_t source_edge_node_id = 1,
                           const std::string& query_id = "q_edge") {
        insert_outbox_row_into(*executor_,
                               event_id,
                               stream_seq,
                               event_kind,
                               source_edge_node_id,
                               query_id);
    }

    void insert_outbox_row_into(QueryExecutor& executor,
                                const std::string& event_id,
                                uint64_t stream_seq,
                                const std::string& event_kind,
                                int64_t source_edge_node_id = 1,
                                const std::string& query_id = "q_edge") {
        std::string candidate_id;
        std::string suppression_key;
        std::string selected_action;
        int64_t selected_action_code = 0;
        std::string selected_expected_key;
        int64_t unsafe_action_code = 0;
        int64_t recovery_top1_hit = 0;
        int64_t avoids_risky_repeat = 0;
        int64_t risky_action_suppressed = 0;
        int64_t suppressed_count = 0;
        int64_t edge_latency_ms = 0;
        int64_t retrieval_rank = 0;
        std::string quality_label;
        int64_t quality_code = 0;
        int64_t score_micros = 0;
        std::string action_class;
        int64_t action_code = 0;
        std::string outcome_label;
        int64_t raw_value_micros = 0;
        int64_t gated_value_micros = 0;
        int64_t context_score_micros = 0;

        if (event_kind == "decision") {
            selected_action = "reroute_zone";
            selected_action_code = 9;
            selected_expected_key = query_id + "|reroute_zone";
            unsafe_action_code = 2;
            recovery_top1_hit = 1;
            avoids_risky_repeat = 1;
            risky_action_suppressed = 1;
            suppressed_count = 2;
            edge_latency_ms = 13;
        } else if (event_kind == "retrieval") {
            candidate_id = "case_1";
            suppression_key = "q_edge|case_1";
            retrieval_rank = 1;
            quality_label = "useful";
            quality_code = 1;
            score_micros = 914131;
            action_class = "reroute_zone";
            action_code = 9;
        } else {
            candidate_id = "case_2";
            suppression_key = "q_edge|case_2";
            action_class = "continue_route";
            action_code = 2;
            outcome_label = "failure";
            raw_value_micros = -1000000;
            gated_value_micros = -120000;
            context_score_micros = 723912;
        }

        const int64_t decision_ts_ns = 1810000001000050000LL;
        const int64_t ready_ts_ns =
            1810000001250050000LL + static_cast<int64_t>(stream_seq);
        exec_ok(
            executor,
            "INSERT INTO physical_ai_edge_feed_outbox_016 "
            "(feed_event_id, stream_seq, event_kind, query_id, query_seq, "
            "candidate_id, suppression_key, selected_action, "
            "selected_action_code, selected_expected_key, unsafe_action_code, "
            "recovery_top1_hit, avoids_risky_repeat, risky_action_suppressed, "
            "suppressed_count, edge_latency_ms, retrieval_rank, quality_label, "
            "quality_code, score_micros, action_class, action_code, outcome_label, "
            "raw_value_micros, gated_value_micros, context_score_micros, "
            "source_edge_node_id, decision_ts_ns, ready_ts_ns, timestamp) VALUES ('" +
            event_id + "', " + std::to_string(stream_seq) + ", '" + event_kind +
            "', '" + query_id + "', 7, '" + candidate_id + "', '" + suppression_key +
            "', '" + selected_action + "', " +
            std::to_string(selected_action_code) + ", '" + selected_expected_key +
            "', " + std::to_string(unsafe_action_code) + ", " +
            std::to_string(recovery_top1_hit) + ", " +
            std::to_string(avoids_risky_repeat) + ", " +
            std::to_string(risky_action_suppressed) + ", " +
            std::to_string(suppressed_count) + ", " +
            std::to_string(edge_latency_ms) + ", " +
            std::to_string(retrieval_rank) + ", '" + quality_label + "', " +
            std::to_string(quality_code) + ", " +
            std::to_string(score_micros) + ", '" + action_class + "', " +
            std::to_string(action_code) + ", '" + outcome_label + "', " +
            std::to_string(raw_value_micros) + ", " +
            std::to_string(gated_value_micros) + ", " +
            std::to_string(context_score_micros) + ", " +
            std::to_string(source_edge_node_id) + ", " +
            std::to_string(decision_ts_ns) + ", " +
            std::to_string(ready_ts_ns) + ", " + std::to_string(ready_ts_ns) +
            ")");
    }

    void seed_three_event_outbox() {
        insert_outbox_row("edge_decision", 1, "decision");
        insert_outbox_row("edge_retrieval", 2, "retrieval");
        insert_outbox_row("edge_suppression", 3, "suppression");
    }

    std::unique_ptr<ZeptoPipeline> pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
};

} // namespace

TEST_F(EdgeFleetSqlHttpAdapterTest, EnsureTablesCreatesDefaultContract) {
    EdgeFleetSqlHttpAdapterConfig config;
    ensure_contract(config);

    EXPECT_TRUE(pipeline_->schema_registry().exists(config.runtime.edge_outbox_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.runtime.fleet_ack_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.fleet_inbox_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.fleet_decision_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.fleet_retrieval_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.fleet_suppression_table));
    EXPECT_TRUE(pipeline_->schema_registry().exists(config.fleet_telemetry_table));
}

TEST_F(EdgeFleetSqlHttpAdapterTest, HooksLoadOutboxAndMaterializeFleetRows) {
    EdgeFleetSqlHttpAdapterConfig config;
    config.runtime.feed.batch_limit = 8;
    config.runtime.feed.max_inflight = 8;
    ensure_contract(config);
    seed_three_event_outbox();

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.batch_event_count, 3u);
    EXPECT_EQ(snap.last_pass.acked_count, 3u);
    EXPECT_EQ(snap.acked_count, 3u);
    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 3);
    EXPECT_EQ(table_count(config.fleet_inbox_table), 3);
    EXPECT_EQ(table_count(config.fleet_decision_table), 1);
    EXPECT_EQ(table_count(config.fleet_retrieval_table), 1);
    EXPECT_EQ(table_count(config.fleet_suppression_table), 1);
    EXPECT_EQ(table_count(config.fleet_telemetry_table), 1);

    const auto decisions = executor_->execute(
        "SELECT query_id, selected_action, selected_action_code "
        "FROM physical_ai_fleet_edge_decisions_016");
    ASSERT_TRUE(decisions.ok()) << decisions.error;
    ASSERT_EQ(decisions.rows.size(), 1u);
    EXPECT_EQ(string_cell(decisions, 0, 0), "q_edge");
    EXPECT_EQ(string_cell(decisions, 0, 1), "reroute_zone");
    EXPECT_EQ(int_cell(decisions, 0, 2), 9);
}

TEST_F(EdgeFleetSqlHttpAdapterTest, RunOnceSkipsAckedRowsBeforeDelivery) {
    EdgeFleetSqlHttpAdapterConfig config;
    config.runtime.feed.batch_limit = 8;
    config.runtime.feed.max_inflight = 8;
    ensure_contract(config);
    seed_three_event_outbox();

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.outbox_events_seen, 0u);
    EXPECT_EQ(snap.last_pass.duplicate_count, 0u);
    EXPECT_EQ(snap.last_pass.acked_count, 0u);
    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 3);
    EXPECT_EQ(table_count(config.fleet_inbox_table), 3);
    EXPECT_EQ(table_count(config.fleet_telemetry_table), 2);
}

TEST_F(EdgeFleetSqlHttpAdapterTest, OutboxQueryLimitBoundsLoadedRows) {
    EdgeFleetSqlHttpAdapterConfig config;
    config.runtime.feed.batch_limit = 8;
    config.runtime.feed.max_inflight = 8;
    config.outbox_query_limit = 2;
    ensure_contract(config);
    seed_three_event_outbox();

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.outbox_events_seen, 2u);
    EXPECT_EQ(snap.last_pass.acked_count, 2u);
    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 2);
    EXPECT_EQ(table_count(config.fleet_inbox_table), 2);
}

TEST_F(EdgeFleetSqlHttpAdapterTest, OutboxQueryLimitPagesPastAckedRows) {
    EdgeFleetSqlHttpAdapterConfig config;
    config.runtime.feed.batch_limit = 8;
    config.runtime.feed.max_inflight = 8;
    config.outbox_query_limit = 1;
    ensure_contract(config);
    seed_three_event_outbox();

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(
        zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
        &error)) << error;
    ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    ASSERT_TRUE(runtime.runOnce(&error)) << error;
    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 1);
    ASSERT_TRUE(runtime.runOnce(&error)) << error;
    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 2);
    ASSERT_TRUE(runtime.runOnce(&error)) << error;
    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 3);
    EXPECT_EQ(table_count(config.fleet_inbox_table), 3);
}

TEST_F(EdgeFleetSqlHttpAdapterTest, ServerRuntimeSoakAcrossRestartsConverges) {
    const auto checkpoint =
        zepto_test_util::unique_test_path("edge_fleet_sql_soak_checkpoint");
    std::filesystem::remove(checkpoint);

    EdgeFleetSqlHttpAdapterConfig config;
    config.runtime.feed.batch_limit = 4;
    config.runtime.feed.max_inflight = 4;
    config.runtime.feed.checkpoint_path = checkpoint.string();
    config.outbox_query_limit = 4;
    ensure_contract(config);

    constexpr size_t kEvents = 24;
    for (size_t index = 0; index < kEvents; ++index) {
        const uint64_t stream_seq = index + 1;
        const std::string kind = (index % 3 == 0)
            ? "decision"
            : ((index % 3 == 1) ? "retrieval" : "suppression");
        insert_outbox_row("edge_soak_" + std::to_string(stream_seq),
                          stream_seq,
                          kind,
                          1,
                          "q_edge_soak_" + std::to_string(stream_seq));
    }

    std::string error;
    for (size_t epoch = 0;
         epoch < 16 && table_count(config.runtime.fleet_ack_table) <
                            static_cast<int64_t>(kEvents);
         ++epoch) {
        EdgeFleetConnectorRuntime runtime;
        ASSERT_TRUE(runtime.setWorkerHooks(
            zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(runtime.start(&error)) << error;
        ASSERT_TRUE(runtime.runOnce(&error)) << error;
        ASSERT_TRUE(runtime.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(config.runtime.fleet_ack_table),
              static_cast<int64_t>(kEvents));
    EXPECT_EQ(table_count(config.fleet_inbox_table),
              static_cast<int64_t>(kEvents));
    EXPECT_EQ(table_count(config.fleet_decision_table), 8);
    EXPECT_EQ(table_count(config.fleet_retrieval_table), 8);
    EXPECT_EQ(table_count(config.fleet_suppression_table), 8);
    EXPECT_GE(table_count(config.fleet_telemetry_table), 6);
    EXPECT_TRUE(std::filesystem::exists(checkpoint));

    std::filesystem::remove(checkpoint);
}

TEST_F(EdgeFleetSqlHttpAdapterTest,
       ReplacementRuntimeSkipsAckedNodeAndConsolidatesNewNodeRows) {
    const auto checkpoint =
        zepto_test_util::unique_test_path("edge_fleet_node_replacement_checkpoint");
    std::filesystem::remove(checkpoint);

    EdgeFleetSqlHttpAdapterConfig config;
    config.runtime.feed.batch_limit = 3;
    config.runtime.feed.max_inflight = 3;
    config.runtime.feed.checkpoint_path = checkpoint.string();
    config.outbox_query_limit = 3;
    ensure_contract(config);

    insert_outbox_row("edge_node_a_decision", 1, "decision", 1, "q_node_a_1");
    insert_outbox_row("edge_node_a_retrieval", 2, "retrieval", 1, "q_node_a_2");
    insert_outbox_row("edge_node_a_suppression", 3, "suppression", 1, "q_node_a_3");
    insert_outbox_row("edge_node_b_decision", 4, "decision", 2, "q_node_b_4");
    insert_outbox_row("edge_node_b_retrieval", 5, "retrieval", 2, "q_node_b_5");
    insert_outbox_row("edge_node_b_suppression", 6, "suppression", 2, "q_node_b_6");

    std::string error;
    {
        EdgeFleetConnectorRuntime node_a_runtime;
        ASSERT_TRUE(node_a_runtime.setWorkerHooks(
            zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(node_a_runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(node_a_runtime.start(&error)) << error;
        ASSERT_TRUE(node_a_runtime.runOnce(&error)) << error;
        EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 3);
        ASSERT_TRUE(node_a_runtime.stop(&error)) << error;
    }

    {
        EdgeFleetConnectorRuntime replacement_runtime;
        ASSERT_TRUE(replacement_runtime.setWorkerHooks(
            zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(replacement_runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(replacement_runtime.start(&error)) << error;
        ASSERT_TRUE(replacement_runtime.runOnce(&error)) << error;

        const auto snap = replacement_runtime.snapshot();
        EXPECT_EQ(snap.last_pass.acked_before, 3u);
        EXPECT_EQ(snap.last_pass.batch_event_count, 3u);
        EXPECT_EQ(snap.last_pass.acked_count, 3u);
        ASSERT_TRUE(replacement_runtime.stop(&error)) << error;
    }

    EXPECT_EQ(table_count(config.runtime.fleet_ack_table), 6);
    EXPECT_EQ(table_count(config.fleet_inbox_table), 6);

    const auto acks = executor_->execute(
        "SELECT source_edge_node_id FROM " + config.runtime.fleet_ack_table);
    ASSERT_TRUE(acks.ok()) << acks.error;
    size_t node_a_acks = 0;
    size_t node_b_acks = 0;
    for (size_t row = 0; row < acks.rows.size(); ++row) {
        if (int_cell(acks, row, 0) == 1) ++node_a_acks;
        if (int_cell(acks, row, 0) == 2) ++node_b_acks;
    }
    EXPECT_EQ(node_a_acks, 3u);
    EXPECT_EQ(node_b_acks, 3u);

    const auto inbox_late = executor_->execute(
        "SELECT late_delivery FROM " + config.fleet_inbox_table);
    ASSERT_TRUE(inbox_late.ok()) << inbox_late.error;
    for (size_t row = 0; row < inbox_late.rows.size(); ++row) {
        EXPECT_EQ(int_cell(inbox_late, row, 0), 0);
    }

    std::filesystem::remove(checkpoint);
}

TEST_F(EdgeFleetSqlHttpAdapterTest,
       HttpOnlyServerRuntimeConvergesAcrossLiveEdgeAndFleetNodes) {
    PipelineConfig edge_cfg;
    edge_cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline edge_pipeline(edge_cfg);
    edge_pipeline.start();
    QueryExecutor edge_executor(edge_pipeline);

    PipelineConfig fleet_cfg;
    fleet_cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    ZeptoPipeline fleet_pipeline(fleet_cfg);
    fleet_pipeline.start();
    QueryExecutor fleet_executor(fleet_pipeline);

    const uint16_t edge_port = zepto_test_util::pick_free_port();
    const uint16_t fleet_port = zepto_test_util::pick_free_port();
    HttpServer edge_server(edge_executor, edge_port);
    HttpServer fleet_server(fleet_executor, fleet_port);
    edge_server.start_async();
    fleet_server.start_async();
    std::this_thread::sleep_for(80ms);

    EdgeFleetSqlHttpAdapterConfig local_tables;
    std::string error;
    ASSERT_TRUE(zeptodb::server::ensureEdgeFleetSqlHttpTables(
        edge_executor, local_tables, &error)) << error;
    ASSERT_TRUE(zeptodb::server::ensureEdgeFleetSqlHttpTables(
        fleet_executor, local_tables, &error)) << error;

    insert_outbox_row_into(edge_executor,
                           "http_node_a_decision",
                           1,
                           "decision",
                           1,
                           "q_http_node_a_1");
    insert_outbox_row_into(edge_executor,
                           "http_node_a_retrieval",
                           2,
                           "retrieval",
                           1,
                           "q_http_node_a_2");
    insert_outbox_row_into(edge_executor,
                           "http_node_a_suppression",
                           3,
                           "suppression",
                           1,
                           "q_http_node_a_3");
    insert_outbox_row_into(edge_executor,
                           "http_node_b_decision",
                           4,
                           "decision",
                           2,
                           "q_http_node_b_4");
    insert_outbox_row_into(edge_executor,
                           "http_node_b_retrieval",
                           5,
                           "retrieval",
                           2,
                           "q_http_node_b_5");
    insert_outbox_row_into(edge_executor,
                           "http_node_b_suppression",
                           6,
                           "suppression",
                           2,
                           "q_http_node_b_6");

    const auto checkpoint =
        zepto_test_util::unique_test_path("edge_fleet_http_only_runtime_checkpoint");
    std::filesystem::remove(checkpoint);

    EdgeFleetSqlHttpAdapterConfig config;
    config.edge_sql_url = "http://127.0.0.1:" + std::to_string(edge_port);
    config.fleet_sql_url = "http://127.0.0.1:" + std::to_string(fleet_port);
    config.runtime.feed.batch_limit = 3;
    config.runtime.feed.max_inflight = 3;
    config.runtime.feed.checkpoint_path = checkpoint.string();
    config.outbox_query_limit = 3;

    {
        EdgeFleetConnectorRuntime node_a_runtime;
        ASSERT_TRUE(node_a_runtime.setWorkerHooks(
            zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(node_a_runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(node_a_runtime.start(&error)) << error;
        ASSERT_TRUE(node_a_runtime.runOnce(&error)) << error;
        const auto first_ack_count = fleet_executor.execute(
            "SELECT count(*) FROM " + config.runtime.fleet_ack_table);
        ASSERT_TRUE(first_ack_count.ok()) << first_ack_count.error;
        ASSERT_FALSE(first_ack_count.rows.empty());
        ASSERT_FALSE(first_ack_count.rows[0].empty());
        EXPECT_EQ(first_ack_count.rows[0][0], 3);
        ASSERT_TRUE(node_a_runtime.stop(&error)) << error;
    }

    {
        EdgeFleetConnectorRuntime replacement_runtime;
        ASSERT_TRUE(replacement_runtime.setWorkerHooks(
            zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(*executor_, config),
            &error)) << error;
        ASSERT_TRUE(replacement_runtime.configure(config.runtime, &error)) << error;
        ASSERT_TRUE(replacement_runtime.start(&error)) << error;
        ASSERT_TRUE(replacement_runtime.runOnce(&error)) << error;

        const auto snap = replacement_runtime.snapshot();
        EXPECT_EQ(snap.last_pass.acked_before, 3u);
        EXPECT_EQ(snap.last_pass.acked_count, 3u);
        ASSERT_TRUE(replacement_runtime.stop(&error)) << error;
    }

    const auto ack_count = fleet_executor.execute(
        "SELECT count(*) FROM " + config.runtime.fleet_ack_table);
    ASSERT_TRUE(ack_count.ok()) << ack_count.error;
    ASSERT_FALSE(ack_count.rows.empty());
    ASSERT_FALSE(ack_count.rows[0].empty());
    EXPECT_EQ(ack_count.rows[0][0], 6);

    const auto inbox_late = fleet_executor.execute(
        "SELECT late_delivery FROM " + config.fleet_inbox_table);
    ASSERT_TRUE(inbox_late.ok()) << inbox_late.error;
    for (size_t row = 0; row < inbox_late.rows.size(); ++row) {
        EXPECT_EQ(int_cell(inbox_late, row, 0), 0);
    }

    std::filesystem::remove(checkpoint);
    edge_server.stop();
    fleet_server.stop();
    edge_pipeline.stop();
    fleet_pipeline.stop();
}

TEST_F(EdgeFleetSqlHttpAdapterTest, RejectsOversizedOutboxLoad) {
    EdgeFleetSqlHttpAdapterConfig config;
    config.max_outbox_bytes = 8;
    ensure_contract(config);
    insert_outbox_row("edge_decision", 1, "decision");

    auto hooks = zeptodb::server::makeEdgeFleetSqlHttpRuntimeHooks(
        *executor_, config);
    const auto loaded = hooks.load_outbox();
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error.find("max_outbox_bytes"), std::string::npos);
}

TEST_F(EdgeFleetSqlHttpAdapterTest, RejectsInvalidIdentifiersAndRemoteBootstrap) {
    EdgeFleetSqlHttpAdapterConfig config;
    std::string error;
    config.runtime.edge_outbox_table = "bad-name";
    EXPECT_FALSE(zeptodb::server::validateEdgeFleetSqlHttpAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("edge_outbox_table"), std::string::npos);

    config = EdgeFleetSqlHttpAdapterConfig{};
    config.edge_sql_url = "ftp://127.0.0.1:8123";
    error.clear();
    EXPECT_FALSE(zeptodb::server::validateEdgeFleetSqlHttpAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("edge_sql_url"), std::string::npos);

    config = EdgeFleetSqlHttpAdapterConfig{};
    config.outbox_query_limit = 0;
    error.clear();
    EXPECT_FALSE(zeptodb::server::validateEdgeFleetSqlHttpAdapterConfig(
        config, &error));
    EXPECT_NE(error.find("outbox_query_limit"), std::string::npos);

    config = EdgeFleetSqlHttpAdapterConfig{};
    config.edge_sql_url = "http://127.0.0.1:8123";
    error.clear();
    EXPECT_FALSE(zeptodb::server::ensureEdgeFleetSqlHttpTables(
        *executor_, config, &error));
    EXPECT_NE(error.find("local adapter"), std::string::npos);
}
