// ============================================================================
// ZeptoDB: Experimental Action-Outcome supervisor runtime unit tests
// ============================================================================

#include "zeptodb/feeds/action_outcome_supervisor_runtime.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

using namespace zeptodb::feeds;

namespace {

ActionOutcomeProposal proposal(std::string id, int64_t ts) {
    ActionOutcomeProposal out;
    out.proposal_id = std::move(id);
    out.source_type = "ros2_bag";
    out.proposed_action = "continue_route";
    out.source_ts_ns = ts;
    return out;
}

ActionOutcomeDecision allow_decision(const ActionOutcomeProposal& item) {
    ActionOutcomeDecision decision;
    decision.proposal_id = item.proposal_id;
    decision.decision = "allow";
    decision.final_action = item.proposed_action;
    decision.reason = "positive_action_outcome_pressure";
    decision.evidence_count = 3;
    return decision;
}

bool wait_until(const std::function<bool()>& pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

TEST(ActionOutcomeSupervisorRuntimeTest, ConfigureStartStopExposeSnapshotAndMetrics) {
    ActionOutcomeSupervisorRuntimeConfig config;
    config.name = "runtime-test";
    config.history_table = "history";
    config.proposal_table = "proposals";
    config.decision_table = "decisions";
    config.evidence_table = "evidence";
    config.batch_limit = 7;

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    auto snap = runtime.snapshot();
    EXPECT_TRUE(snap.configured);
    EXPECT_TRUE(snap.enabled);
    EXPECT_EQ(snap.mode, "shadow");
    EXPECT_EQ(snap.history_table, "history");
    EXPECT_EQ(snap.batch_limit, 7u);
    EXPECT_EQ(snap.start_total, 1u);

    const std::string metrics = runtime.formatPrometheus();
    EXPECT_NE(metrics.find("zepto_action_outcome_supervisor_enabled{supervisor=\"runtime-test\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("zepto_action_outcome_supervisor_worker_passes_total{supervisor=\"runtime-test\"} 0"),
              std::string::npos);

    ASSERT_TRUE(runtime.stop(&error)) << error;
    EXPECT_FALSE(runtime.snapshot().enabled);
}

TEST(ActionOutcomeSupervisorRuntimeTest, RejectsInvalidModeAndLimits) {
    ActionOutcomeSupervisorRuntime runtime;
    ActionOutcomeSupervisorRuntimeConfig config;
    config.mode = "advisory";
    std::string error;
    EXPECT_FALSE(runtime.configure(config, &error));
    EXPECT_NE(error.find("shadow"), std::string::npos);

    config.mode = "shadow";
    config.batch_limit = 0;
    EXPECT_FALSE(runtime.configure(config, &error));
    EXPECT_NE(error.find("batch_limit"), std::string::npos);
}

TEST(ActionOutcomeSupervisorRuntimeTest, RunOnceProcessesBatchAndSkipsDecidedProposals) {
    ActionOutcomeSupervisorRuntimeConfig config;
    config.batch_limit = 3;

    std::unordered_set<std::string> decided{"p2"};
    std::vector<std::string> sunk;
    ActionOutcomeSupervisorRuntimeHooks hooks;
    hooks.load_proposals = [] {
        ActionOutcomeProposalLoadResult out;
        out.ok = true;
        out.proposals = {
            proposal("p3", 3),
            proposal("p2", 2),
            proposal("p1", 1),
            proposal("p4", 4),
        };
        return out;
    };
    hooks.already_decided = [&](const std::string& id) {
        return decided.find(id) != decided.end();
    };
    hooks.decide = [](const ActionOutcomeProposal& item) {
        ActionOutcomeDecisionResult out;
        out.ok = true;
        out.decision = allow_decision(item);
        return out;
    };
    hooks.sink_decision = [&](const ActionOutcomeDecision& decision, std::string*) {
        sunk.push_back(decision.proposal_id);
        decided.insert(decision.proposal_id);
        return true;
    };

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.proposals_seen, 4u);
    EXPECT_EQ(snap.last_pass.batch_proposals, 3u);
    EXPECT_EQ(snap.last_pass.duplicate_count, 1u);
    EXPECT_EQ(snap.last_pass.processed_count, 2u);
    EXPECT_EQ(snap.decisions_allow_total, 2u);
    EXPECT_EQ(snap.evidence_rows_written_total, 6u);
    ASSERT_EQ(sunk.size(), 2u);
    EXPECT_EQ(sunk[0], "p1");
    EXPECT_EQ(sunk[1], "p3");
}

TEST(ActionOutcomeSupervisorRuntimeTest, DecisionErrorFailsClosedAndWritesSink) {
    ActionOutcomeSupervisorRuntimeConfig config;
    config.fail_closed_action = "manual_review";

    ActionOutcomeDecision captured;
    ActionOutcomeSupervisorRuntimeHooks hooks;
    hooks.load_proposals = [] {
        ActionOutcomeProposalLoadResult out;
        out.ok = true;
        out.proposals = {proposal("p1", 1)};
        return out;
    };
    hooks.decide = [](const ActionOutcomeProposal&) {
        ActionOutcomeDecisionResult out;
        out.ok = false;
        out.error = "no compatible evidence";
        return out;
    };
    hooks.sink_decision = [&](const ActionOutcomeDecision& decision, std::string*) {
        captured = decision;
        return true;
    };

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    EXPECT_EQ(captured.decision, "suppress_no_evidence");
    EXPECT_EQ(captured.final_action, "manual_review");
    EXPECT_TRUE(captured.fail_closed);
    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.last_pass.decision_error_count, 1u);
    EXPECT_EQ(snap.fail_closed_total, 1u);
    EXPECT_EQ(snap.decisions_suppress_total, 1u);
}

TEST(ActionOutcomeSupervisorRuntimeTest, DecisionExceptionFailsClosedAndWritesSink) {
    ActionOutcomeSupervisorRuntimeHooks hooks;
    ActionOutcomeDecision captured;
    hooks.load_proposals = [] {
        ActionOutcomeProposalLoadResult out;
        out.ok = true;
        out.proposals = {proposal("p1", 1)};
        return out;
    };
    hooks.decide = [](const ActionOutcomeProposal&) -> ActionOutcomeDecisionResult {
        throw std::runtime_error("model unavailable");
    };
    hooks.sink_decision = [&](const ActionOutcomeDecision& decision, std::string*) {
        captured = decision;
        return true;
    };

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(ActionOutcomeSupervisorRuntimeConfig{}, &error))
        << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    ASSERT_TRUE(runtime.runOnce(&error)) << error;

    EXPECT_EQ(captured.decision, "suppress_no_evidence");
    EXPECT_TRUE(captured.fail_closed);
    EXPECT_NE(captured.reason.find("decision hook exception"), std::string::npos);
    EXPECT_EQ(runtime.snapshot().fail_closed_total, 1u);
}

TEST(ActionOutcomeSupervisorRuntimeTest, LoaderExceptionCountsWorkerFailure) {
    ActionOutcomeSupervisorRuntimeHooks hooks;
    hooks.load_proposals = []() -> ActionOutcomeProposalLoadResult {
        throw std::runtime_error("source unavailable");
    };
    hooks.decide = [](const ActionOutcomeProposal&) {
        ActionOutcomeDecisionResult out;
        out.ok = true;
        return out;
    };
    hooks.sink_decision = [](const ActionOutcomeDecision&, std::string*) {
        return true;
    };

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(ActionOutcomeSupervisorRuntimeConfig{}, &error))
        << error;
    ASSERT_TRUE(runtime.start(&error)) << error;
    EXPECT_FALSE(runtime.runOnce(&error));
    EXPECT_NE(error.find("proposal load exception"), std::string::npos);

    const auto snap = runtime.snapshot();
    EXPECT_EQ(snap.worker_failures_total, 1u);
    EXPECT_EQ(snap.consecutive_failures, 1u);
}

TEST(ActionOutcomeSupervisorRuntimeTest, WorkerModeRequiresHooksOnStart) {
    ActionOutcomeSupervisorRuntimeConfig config;
    config.worker_enabled = true;
    config.worker_poll_interval_ms = 1;

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    EXPECT_FALSE(runtime.start(&error));
    EXPECT_NE(error.find("requires proposal loader"), std::string::npos);
    EXPECT_EQ(runtime.snapshot().start_failures_total, 1u);
}

TEST(ActionOutcomeSupervisorRuntimeTest, BackgroundWorkerRecordsLoadFailureBudget) {
    ActionOutcomeSupervisorRuntimeConfig config;
    config.worker_enabled = true;
    config.worker_poll_interval_ms = 1;
    config.max_consecutive_failures = 2;

    ActionOutcomeSupervisorRuntimeHooks hooks;
    hooks.load_proposals = [] {
        ActionOutcomeProposalLoadResult out;
        out.ok = false;
        out.error = "source unavailable";
        return out;
    };
    hooks.decide = [](const ActionOutcomeProposal&) {
        ActionOutcomeDecisionResult out;
        out.ok = true;
        return out;
    };
    hooks.sink_decision = [](const ActionOutcomeDecision&, std::string*) {
        return true;
    };

    ActionOutcomeSupervisorRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.setWorkerHooks(std::move(hooks), &error)) << error;
    ASSERT_TRUE(runtime.configure(config, &error)) << error;
    ASSERT_TRUE(runtime.start(&error)) << error;

    ASSERT_TRUE(wait_until([&] {
        return runtime.snapshot().failure_budget_exhausted;
    }));

    ASSERT_TRUE(runtime.stop(&error)) << error;
    const auto snap = runtime.snapshot();
    EXPECT_GE(snap.worker_failures_total, 2u);
    EXPECT_FALSE(snap.worker_running);
}
