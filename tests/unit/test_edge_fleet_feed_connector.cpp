// ============================================================================
// ZeptoDB: Experimental edge/fleet feed connector unit tests
// ============================================================================

#include "zeptodb/feeds/edge_fleet_feed_connector.h"
#include "zeptodb/feeds/edge_fleet_connector_runtime.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

using namespace zeptodb::feeds;

namespace {

EdgeFleetFeedEvent event(std::string id, uint64_t seq) {
    EdgeFleetFeedEvent out;
    out.event_id = std::move(id);
    out.stream_seq = seq;
    out.kind = EdgeFleetEventKind::Decision;
    out.ready_ts_ns = 1'810'000'000'000'000'000LL + static_cast<int64_t>(seq);
    out.query_id = "q" + std::to_string(seq);
    out.payload_json = "{}";
    return out;
}

std::filesystem::path temp_checkpoint_path(std::string_view name) {
    return std::filesystem::temp_directory_path() /
           ("zeptodb_" + std::string(name) + ".checkpoint");
}

bool wait_until(const std::function<bool()>& predicate) {
    for (int i = 0; i < 200; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST(EdgeFleetFeedConnectorTest, ConfigDefaultsAndValidation) {
    EdgeFleetFeedConfig config;
    EXPECT_TRUE(EdgeFleetFeedConnector::isValidConfig(config));
    EXPECT_EQ(config.batch_limit, 128u);
    EXPECT_EQ(config.max_inflight, 128u);
    EXPECT_EQ(config.max_retries_per_event, 1u);
    EXPECT_EQ(config.max_failures_per_pass, 16u);
    EXPECT_EQ(config.retry_backoff_ms, 0u);
    EXPECT_TRUE(config.allow_late_events);

    config.batch_limit = 0;
    EXPECT_FALSE(EdgeFleetFeedConnector::isValidConfig(config));

    EXPECT_EQ(EdgeFleetFeedConnector::parseKind("decision"), EdgeFleetEventKind::Decision);
    EXPECT_EQ(EdgeFleetFeedConnector::parseKind("retrieval"), EdgeFleetEventKind::Retrieval);
    EXPECT_EQ(EdgeFleetFeedConnector::parseKind("suppression"), EdgeFleetEventKind::Suppression);
    EXPECT_FALSE(EdgeFleetFeedConnector::parseKind("other").has_value());
    EXPECT_EQ(EdgeFleetFeedConnector::kindName(EdgeFleetEventKind::Suppression),
              "suppression");
}

TEST(EdgeFleetFeedConnectorTest, ProcessesBoundedBatchAndPersistsAcks) {
    const auto checkpoint = temp_checkpoint_path("bounded");
    std::filesystem::remove(checkpoint);

    EdgeFleetFeedConfig config;
    config.batch_limit = 2;
    config.max_inflight = 3;
    config.checkpoint_path = checkpoint.string();

    std::vector<std::string> delivered;
    EdgeFleetFeedConnector connector(
        config,
        [&](const EdgeFleetFeedEvent& item) {
            delivered.push_back(item.event_id);
            return EdgeFleetDeliveryResult::Acked;
        });

    const std::vector<EdgeFleetFeedEvent> outbox{
        event("e3", 3),
        event("e1", 1),
        event("e2", 2),
    };

    const auto pass = connector.processOnce(outbox);
    EXPECT_EQ(pass.batch_event_count, 2u);
    EXPECT_EQ(pass.attempted_count, 2u);
    EXPECT_EQ(pass.acked_count, 2u);
    EXPECT_EQ(pass.acked_after, 2u);
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0], "e1");
    EXPECT_EQ(delivered[1], "e2");
    EXPECT_TRUE(connector.isAcked("e1"));
    EXPECT_TRUE(connector.isAcked("e2"));
    EXPECT_FALSE(connector.isAcked("e3"));
    EXPECT_EQ(connector.stats().checkpoint_saves, 1u);
    EXPECT_TRUE(std::filesystem::exists(checkpoint));

    std::filesystem::remove(checkpoint);
}

TEST(EdgeFleetFeedConnectorTest, RetriesDroppedEventAndAcceptsLateDelivery) {
    EdgeFleetFeedConfig config;
    config.batch_limit = 4;
    config.max_inflight = 4;
    config.max_retries_per_event = 1;

    std::unordered_map<std::string, int> attempts;
    EdgeFleetFeedConnector connector(
        config,
        [&](const EdgeFleetFeedEvent& item) {
            attempts[item.event_id]++;
            if (item.event_id == "e2" && attempts[item.event_id] == 1) {
                return EdgeFleetDeliveryResult::TransientFailure;
            }
            return EdgeFleetDeliveryResult::Acked;
        });

    const std::vector<EdgeFleetFeedEvent> first{
        event("e1", 1),
        event("e2", 2),
        event("e3", 3),
    };
    const auto first_pass = connector.processOnce(first);
    EXPECT_EQ(first_pass.acked_count, 2u);
    EXPECT_EQ(first_pass.transient_failure_count, 1u);
    EXPECT_EQ(connector.highestAckedStreamSeq(), 3u);
    EXPECT_FALSE(connector.isAcked("e2"));

    const std::vector<EdgeFleetFeedEvent> second{
        event("e1", 1),
        event("e2", 2),
        event("e3", 3),
    };
    const auto second_pass = connector.processOnce(second);
    EXPECT_EQ(second_pass.duplicate_count, 2u);
    EXPECT_EQ(second_pass.late_count, 1u);
    EXPECT_EQ(second_pass.acked_count, 1u);
    EXPECT_TRUE(connector.isAcked("e2"));
    EXPECT_EQ(connector.stats().late_events, 1u);
    EXPECT_EQ(connector.stats().duplicate_events, 2u);
}

TEST(EdgeFleetFeedConnectorTest, ReloadsCheckpointAfterRestart) {
    const auto checkpoint = temp_checkpoint_path("restart");
    std::filesystem::remove(checkpoint);

    EdgeFleetFeedConfig config;
    config.batch_limit = 8;
    config.max_inflight = 8;
    config.checkpoint_path = checkpoint.string();

    {
        EdgeFleetFeedConnector connector(
            config,
            [](const EdgeFleetFeedEvent&) { return EdgeFleetDeliveryResult::Acked; });
        const auto pass = connector.processOnce({event("e1", 1), event("e2", 2)});
        EXPECT_EQ(pass.acked_count, 2u);
        ASSERT_TRUE(connector.saveCheckpoint());
    }

    std::vector<std::string> delivered_after_restart;
    EdgeFleetFeedConnector restarted(
        config,
        [&](const EdgeFleetFeedEvent& item) {
            delivered_after_restart.push_back(item.event_id);
            return EdgeFleetDeliveryResult::Acked;
        });
    ASSERT_TRUE(restarted.loadCheckpoint());

    const auto pass = restarted.processOnce({
        event("e1", 1),
        event("e2", 2),
        event("e3", 3),
    });
    EXPECT_EQ(pass.acked_before, 2u);
    EXPECT_EQ(pass.duplicate_count, 2u);
    EXPECT_EQ(pass.acked_count, 1u);
    ASSERT_EQ(delivered_after_restart.size(), 1u);
    EXPECT_EQ(delivered_after_restart[0], "e3");

    std::filesystem::remove(checkpoint);
}

TEST(EdgeFleetFeedConnectorTest, KeepsEventUnackedOnAckBoundaryFailure) {
    EdgeFleetFeedConfig config;
    config.batch_limit = 1;
    config.max_inflight = 1;

    int attempts = 0;
    EdgeFleetFeedConnector connector(
        config,
        [&](const EdgeFleetFeedEvent&) {
            attempts++;
            if (attempts == 1) {
                return EdgeFleetDeliveryResult::AppliedButAckFailed;
            }
            return EdgeFleetDeliveryResult::Acked;
        });

    const auto first = connector.processOnce({event("e1", 1)});
    EXPECT_EQ(first.ack_boundary_failure_count, 1u);
    EXPECT_FALSE(connector.isAcked("e1"));

    const auto second = connector.processOnce({event("e1", 1)});
    EXPECT_EQ(second.acked_count, 1u);
    EXPECT_TRUE(connector.isAcked("e1"));
    EXPECT_EQ(connector.stats().ack_boundary_failures, 1u);
}

TEST(EdgeFleetFeedConnectorTest, RejectsMalformedEventsAndCanBlockLateEvents) {
    EdgeFleetFeedConfig config;
    config.batch_limit = 4;
    config.max_inflight = 4;
    config.allow_late_events = false;

    EdgeFleetFeedConnector connector(
        config,
        [](const EdgeFleetFeedEvent&) { return EdgeFleetDeliveryResult::Acked; });

    EdgeFleetFeedEvent empty_id = event("", 1);
    EdgeFleetFeedEvent zero_seq = event("zero", 0);
    const auto malformed = connector.processOnce({empty_id, zero_seq, event("e3", 3)});
    EXPECT_EQ(malformed.rejected_count, 2u);
    EXPECT_EQ(malformed.acked_count, 1u);
    EXPECT_TRUE(connector.isAcked("e3"));

    const auto late = connector.processOnce({event("e2", 2)});
    EXPECT_EQ(late.late_count, 1u);
    EXPECT_EQ(late.rejected_count, 1u);
    EXPECT_FALSE(connector.isAcked("e2"));
}

TEST(EdgeFleetFeedConnectorTest, RetriesTransientFailureWithinPass) {
    EdgeFleetFeedConfig config;
    config.batch_limit = 1;
    config.max_inflight = 1;
    config.max_retries_per_event = 2;

    int attempts = 0;
    EdgeFleetFeedConnector connector(
        config,
        [&](const EdgeFleetFeedEvent&) {
            attempts++;
            return attempts == 1
                ? EdgeFleetDeliveryResult::TransientFailure
                : EdgeFleetDeliveryResult::Acked;
        });

    const auto pass = connector.processOnce({event("e1", 1)});
    EXPECT_EQ(pass.attempted_count, 2u);
    EXPECT_EQ(pass.transient_failure_count, 1u);
    EXPECT_EQ(pass.acked_count, 1u);
}

TEST(EdgeFleetFeedConnectorTest, StopsPassWhenFailureBudgetIsExhausted) {
    EdgeFleetFeedConfig config;
    config.batch_limit = 4;
    config.max_inflight = 4;
    config.max_retries_per_event = 3;
    config.max_failures_per_pass = 2;

    EdgeFleetFeedConnector connector(
        config,
        [](const EdgeFleetFeedEvent&) {
            return EdgeFleetDeliveryResult::TransientFailure;
        });

    const auto pass = connector.processOnce({
        event("e1", 1),
        event("e2", 2),
        event("e3", 3),
    });
    EXPECT_EQ(pass.attempted_count, 2u);
    EXPECT_EQ(pass.transient_failure_count, 2u);
    EXPECT_TRUE(pass.failure_budget_exhausted);
    EXPECT_EQ(connector.stats().failure_budget_exhausted, 1u);
}

TEST(EdgeFleetFeedConnectorTest, FormatsPrometheusCounters) {
    EdgeFleetFeedStats stats;
    stats.passes = 2;
    stats.events_attempted = 3;
    stats.events_acked = 2;
    stats.ack_boundary_failures = 1;
    stats.max_inflight_observed = 4;
    stats.failure_budget_exhausted = 1;

    const std::string text = EdgeFleetFeedConnector::formatPrometheus(
        "edge\"a", stats);
    EXPECT_NE(text.find("zepto_edge_fleet_feed_passes_total{connector=\"edge\\\"a\"} 2"),
              std::string::npos);
    EXPECT_NE(text.find("zepto_edge_fleet_feed_events_acked_total{connector=\"edge\\\"a\"} 2"),
              std::string::npos);
    EXPECT_NE(text.find("zepto_edge_fleet_feed_ack_boundary_failures_total{connector=\"edge\\\"a\"} 1"),
              std::string::npos);
    EXPECT_NE(text.find("zepto_edge_fleet_feed_max_inflight_observed{connector=\"edge\\\"a\"} 4"),
              std::string::npos);
    EXPECT_NE(text.find("zepto_edge_fleet_feed_failure_budget_exhausted_total{connector=\"edge\\\"a\"} 1"),
              std::string::npos);
}

TEST(EdgeFleetConnectorRuntimeTest, ConfigureStartStopExposeSnapshotAndMetrics) {
    EdgeFleetConnectorRuntimeConfig config;
    config.name = "runtime-test";
    config.edge_outbox_table = "edge_outbox";
    config.fleet_ack_table = "fleet_ack";
    config.feed.batch_limit = 7;
    config.feed.max_inflight = 5;
    config.feed.max_retries_per_event = 2;
    config.feed.max_failures_per_pass = 3;
    config.feed.retry_backoff_ms = 1;

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    auto snap = runtime.snapshot();
    EXPECT_TRUE(snap.configured);
    EXPECT_TRUE(snap.enabled);
    EXPECT_EQ(snap.name, "runtime-test");
    EXPECT_EQ(snap.edge_outbox_table, "edge_outbox");
    EXPECT_EQ(snap.fleet_ack_table, "fleet_ack");
    EXPECT_EQ(snap.batch_limit, 7u);
    EXPECT_EQ(snap.max_inflight, 5u);
    EXPECT_EQ(snap.max_retries_per_event, 2u);
    EXPECT_EQ(snap.max_failures_per_pass, 3u);
    EXPECT_EQ(snap.retry_backoff_ms, 1u);
    EXPECT_EQ(snap.start_total, 1u);

    const std::string metrics = runtime.formatPrometheus();
    EXPECT_NE(metrics.find("zepto_edge_fleet_connector_enabled{connector=\"runtime-test\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("zepto_edge_fleet_connector_start_total{connector=\"runtime-test\"} 1"),
              std::string::npos);

    ASSERT_TRUE(runtime.stop(&error)) << error;
    snap = runtime.snapshot();
    EXPECT_FALSE(snap.enabled);
    EXPECT_EQ(snap.stop_total, 1u);
}

TEST(EdgeFleetConnectorRuntimeTest, RejectsInvalidLimits) {
    EdgeFleetConnectorRuntimeConfig config;
    config.feed.batch_limit = 0;

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    EXPECT_FALSE(runtime.configure(config, &error));
    EXPECT_NE(error.find("positive"), std::string::npos);
    EXPECT_FALSE(runtime.snapshot().configured);
}

TEST(EdgeFleetConnectorRuntimeTest, MissingCheckpointStartsEmpty) {
    const auto checkpoint = temp_checkpoint_path("runtime_missing");
    std::filesystem::remove(checkpoint);

    EdgeFleetConnectorRuntimeConfig config;
    config.name = "checkpoint-test";
    config.feed.checkpoint_path = checkpoint.string();

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    const auto snap = runtime.snapshot();
    EXPECT_TRUE(snap.enabled);
    EXPECT_EQ(snap.acked_count, 0u);
    EXPECT_EQ(snap.feed_stats.checkpoint_loads, 0u);

    ASSERT_TRUE(runtime.stop(&error)) << error;
    EXPECT_TRUE(std::filesystem::exists(checkpoint));
    std::filesystem::remove(checkpoint);
}

TEST(EdgeFleetConnectorRuntimeTest, ClearDisablesAndRemovesConfig) {
    EdgeFleetConnectorRuntime runtime;
    EdgeFleetConnectorRuntimeConfig config;
    std::string error;

    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.clear(&error)) << error;

    const auto snap = runtime.snapshot();
    EXPECT_FALSE(snap.configured);
    EXPECT_FALSE(snap.enabled);
}

TEST(EdgeFleetConnectorRuntimeTest, RunOnceProcessesBoundedWorkerHooks) {
    EdgeFleetConnectorRuntimeConfig config;
    config.feed.batch_limit = 2;
    config.feed.max_inflight = 2;

    std::vector<std::string> delivered;
    EdgeFleetConnectorRuntimeHooks hooks;
    hooks.load_outbox = [] {
        EdgeFleetOutboxLoadResult out;
        out.ok = true;
        out.events = {event("e3", 3), event("e1", 1), event("e2", 2)};
        return out;
    };
    hooks.sink = [&](const EdgeFleetFeedEvent& item) {
        delivered.push_back(item.event_id);
        return EdgeFleetDeliveryResult::Acked;
    };

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    auto snap = runtime.snapshot();
    EXPECT_EQ(snap.worker_passes_total, 1u);
    EXPECT_EQ(snap.last_pass.batch_event_count, 2u);
    EXPECT_EQ(snap.last_pass.acked_count, 2u);
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0], "e1");
    EXPECT_EQ(delivered[1], "e2");
}

TEST(EdgeFleetConnectorRuntimeTest, BackgroundWorkerConvergesAcrossBatches) {
    EdgeFleetConnectorRuntimeConfig config;
    config.worker_enabled = true;
    config.worker_poll_interval_ms = 1;
    config.feed.batch_limit = 1;
    config.feed.max_inflight = 1;

    std::vector<std::string> delivered;
    EdgeFleetConnectorRuntimeHooks hooks;
    hooks.load_outbox = [] {
        EdgeFleetOutboxLoadResult out;
        out.ok = true;
        out.events = {event("e1", 1), event("e2", 2), event("e3", 3)};
        return out;
    };
    hooks.sink = [&](const EdgeFleetFeedEvent& item) {
        delivered.push_back(item.event_id);
        return EdgeFleetDeliveryResult::Acked;
    };

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    ASSERT_TRUE(wait_until([&] {
        return runtime.snapshot().acked_count == 3u;
    }));

    ASSERT_TRUE(runtime.stop(&error)) << error;
    const auto snap = runtime.snapshot();
    EXPECT_FALSE(snap.worker_running);
    EXPECT_EQ(snap.acked_count, 3u);
    EXPECT_GE(snap.worker_passes_total, 3u);
    EXPECT_EQ(snap.worker_start_total, 1u);
    EXPECT_GE(delivered.size(), 3u);
}

TEST(EdgeFleetConnectorRuntimeTest, WorkerRecordsLoadErrorsAndRecovers) {
    EdgeFleetConnectorRuntimeConfig config;
    config.worker_enabled = true;
    config.worker_poll_interval_ms = 1;
    config.feed.batch_limit = 4;
    config.feed.max_inflight = 4;

    int load_attempts = 0;
    EdgeFleetConnectorRuntimeHooks hooks;
    hooks.load_outbox = [&] {
        ++load_attempts;
        EdgeFleetOutboxLoadResult out;
        if (load_attempts == 1) {
            out.ok = false;
            out.error = "simulated edge outage";
            return out;
        }
        out.ok = true;
        out.events = {event("e1", 1)};
        return out;
    };
    hooks.sink = [](const EdgeFleetFeedEvent&) {
        return EdgeFleetDeliveryResult::Acked;
    };

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    ASSERT_TRUE(wait_until([&] {
        const auto snap = runtime.snapshot();
        return snap.worker_load_errors_total == 1u && snap.acked_count == 1u;
    }));

    ASSERT_TRUE(runtime.stop(&error)) << error;
    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.worker_load_errors_total, 1u);
    EXPECT_EQ(snap.acked_count, 1u);
    EXPECT_GE(load_attempts, 2);
}

TEST(EdgeFleetConnectorRuntimeTest, WorkerModeRequiresHooksOnStart) {
    EdgeFleetConnectorRuntimeConfig config;
    config.worker_enabled = true;
    config.worker_poll_interval_ms = 1;

    EdgeFleetConnectorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    EXPECT_FALSE(runtime.start(&error));
    EXPECT_NE(error.find("requires outbox loader"), std::string::npos);
    EXPECT_EQ(runtime.snapshot().start_failures_total, 1u);
}
