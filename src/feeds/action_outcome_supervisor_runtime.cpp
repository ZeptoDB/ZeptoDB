// ============================================================================
// ZeptoDB: Experimental Action-Outcome supervisor runtime lifecycle
// ============================================================================

#include "zeptodb/feeds/action_outcome_supervisor_runtime.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>
#include <utility>

namespace zeptodb::feeds {
namespace {

std::string sanitize_label_value(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool has_fail_closed_reason(const ActionOutcomeDecision& decision) {
    return decision.reason.find("fail_closed") != std::string::npos ||
           decision.decision == "suppress_no_evidence";
}

std::string exception_message(const std::string& context,
                              const std::exception& ex) {
    return context + ": " + ex.what();
}

std::string unknown_exception_message(const std::string& context) {
    return context + ": unknown exception";
}

} // namespace

ActionOutcomeSupervisorRuntime::ActionOutcomeSupervisorRuntime() = default;

ActionOutcomeSupervisorRuntime::~ActionOutcomeSupervisorRuntime() {
    if (snapshot().configured) {
        std::string ignored;
        (void)stop(&ignored);
    } else {
        stopWorkerThread();
    }
}

bool ActionOutcomeSupervisorRuntime::validateConfig(
    const ActionOutcomeSupervisorRuntimeConfig& config,
    std::string* error) {
    if (config.name.empty()) {
        if (error) *error = "supervisor name is required";
        return false;
    }
    if (config.mode != "shadow") {
        if (error) *error = "only shadow mode is supported for the experimental runtime";
        return false;
    }
    if (config.history_table.empty() || config.proposal_table.empty() ||
        config.decision_table.empty() || config.evidence_table.empty()) {
        if (error) *error = "history, proposal, decision, and evidence tables are required";
        return false;
    }
    if (config.fail_closed_action.empty()) {
        if (error) *error = "fail_closed_action is required";
        return false;
    }
    if (config.worker_poll_interval_ms == 0) {
        if (error) *error = "worker_poll_interval_ms must be positive";
        return false;
    }
    if (config.batch_limit == 0) {
        if (error) *error = "batch_limit must be positive";
        return false;
    }
    if (config.max_consecutive_failures == 0) {
        if (error) *error = "max_consecutive_failures must be positive";
        return false;
    }
    if (config.max_decision_errors_per_pass == 0 ||
        config.max_sink_errors_per_pass == 0) {
        if (error) {
            *error = "max_decision_errors_per_pass and max_sink_errors_per_pass must be positive";
        }
        return false;
    }
    return true;
}

bool ActionOutcomeSupervisorRuntime::configure(
    ActionOutcomeSupervisorRuntimeConfig config,
    std::string* error) {
    if (snapshot().enabled) {
        std::string stop_error;
        if (!stop(&stop_error)) {
            if (error) *error = "failed to stop existing supervisor: " + stop_error;
            return false;
        }
    }

    if (!validateConfig(config, error)) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = error ? *error : "invalid supervisor config";
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    config_ = std::move(config);
    configured_ = true;
    enabled_ = false;
    failure_budget_exhausted_ = false;
    consecutive_failures_ = 0;
    last_error_.clear();
    ++configure_total_;
    return true;
}

bool ActionOutcomeSupervisorRuntime::setWorkerHooks(
    ActionOutcomeSupervisorRuntimeHooks hooks,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled_) {
        last_error_ = "stop supervisor before changing worker hooks";
        if (error) *error = last_error_;
        return false;
    }
    hooks_ = std::move(hooks);
    last_error_.clear();
    return true;
}

bool ActionOutcomeSupervisorRuntime::start(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_) {
            last_error_ = "supervisor is not configured";
            if (error) *error = last_error_;
            ++start_failures_total_;
            return false;
        }
        if (config_.worker_enabled && !workerHooksConfiguredLocked()) {
            last_error_ = "worker mode requires proposal loader, decision provider, and decision sink hooks";
            if (error) *error = last_error_;
            ++start_failures_total_;
            return false;
        }
        if (enabled_) {
            last_error_.clear();
            return true;
        }
        enabled_ = true;
        worker_stop_requested_ = false;
        failure_budget_exhausted_ = false;
        consecutive_failures_ = 0;
        last_error_.clear();
        ++start_total_;
        if (config_.worker_enabled) {
            worker_running_ = true;
            ++worker_start_total_;
            worker_ = std::thread(&ActionOutcomeSupervisorRuntime::workerLoop, this);
        }
    }
    return true;
}

bool ActionOutcomeSupervisorRuntime::stop(std::string* error) {
    stopWorkerThread();

    std::lock_guard<std::mutex> lock(mu_);
    if (!configured_) {
        last_error_ = "supervisor is not configured";
        if (error) *error = last_error_;
        ++stop_failures_total_;
        return false;
    }
    enabled_ = false;
    last_error_.clear();
    ++stop_total_;
    return true;
}

bool ActionOutcomeSupervisorRuntime::clear(std::string* error) {
    if (snapshot().enabled) {
        std::string stop_error;
        if (!stop(&stop_error)) {
            if (error) *error = stop_error;
            return false;
        }
    } else {
        stopWorkerThread();
    }

    std::lock_guard<std::mutex> lock(mu_);
    config_ = ActionOutcomeSupervisorRuntimeConfig{};
    configured_ = false;
    enabled_ = false;
    worker_running_ = false;
    worker_stop_requested_ = false;
    failure_budget_exhausted_ = false;
    consecutive_failures_ = 0;
    last_pass_ = ActionOutcomeSupervisorPassResult{};
    last_error_.clear();
    return true;
}

bool ActionOutcomeSupervisorRuntime::runOnce(std::string* error) {
    ActionOutcomeProposalLoader loader;
    ActionOutcomeAlreadyDecided already_decided;
    ActionOutcomeDecisionProvider decide;
    ActionOutcomeDecisionSink sink;
    ActionOutcomeSupervisorRuntimeConfig config;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_) {
            last_error_ = "supervisor is not configured";
            if (error) *error = last_error_;
            return false;
        }
        if (!enabled_) {
            last_error_ = "supervisor is not enabled";
            if (error) *error = last_error_;
            return false;
        }
        if (!workerHooksConfiguredLocked()) {
            last_error_ = "worker hooks are not configured";
            if (error) *error = last_error_;
            return false;
        }
        loader = hooks_.load_proposals;
        already_decided = hooks_.already_decided;
        decide = hooks_.decide;
        sink = hooks_.sink_decision;
        config = config_;
        ++worker_wakeups_total_;
    }

    const auto started = std::chrono::steady_clock::now();
    ActionOutcomeSupervisorPassResult pass;
    ActionOutcomeProposalLoadResult loaded;
    try {
        loaded = loader();
    } catch (const std::exception& ex) {
        loaded.ok = false;
        loaded.error = exception_message("proposal load exception", ex);
    } catch (...) {
        loaded.ok = false;
        loaded.error = unknown_exception_message("proposal load exception");
    }
    if (!loaded.ok) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = loaded.error.empty() ? "proposal load failed" : loaded.error;
        if (error) *error = last_error_;
        ++worker_failures_total_;
        ++consecutive_failures_;
        failure_budget_exhausted_ =
            consecutive_failures_ >= config_.max_consecutive_failures;
        return false;
    }

    pass.proposals_seen = loaded.proposals.size();
    std::stable_sort(loaded.proposals.begin(), loaded.proposals.end(),
                     [](const ActionOutcomeProposal& left,
                        const ActionOutcomeProposal& right) {
                         if (left.source_ts_ns != right.source_ts_ns) {
                             return left.source_ts_ns < right.source_ts_ns;
                         }
                         return left.proposal_id < right.proposal_id;
                     });

    std::string sink_error;
    std::string fatal_error;
    for (const auto& proposal : loaded.proposals) {
        if (proposal.proposal_id.empty() || proposal.proposed_action.empty()) {
            if (pass.batch_proposals >= config.batch_limit) {
                break;
            }
            ++pass.batch_proposals;
            ++pass.rejected_count;
            continue;
        }
        if (already_decided) {
            bool duplicate = false;
            try {
                duplicate = already_decided(proposal.proposal_id);
            } catch (const std::exception& ex) {
                fatal_error = exception_message(
                    "already-decided hook exception", ex);
                break;
            } catch (...) {
                fatal_error = unknown_exception_message(
                    "already-decided hook exception");
                break;
            }
            if (duplicate) {
                ++pass.duplicate_count;
                continue;
            }
        }
        if (pass.batch_proposals >= config.batch_limit) {
            break;
        }
        ++pass.batch_proposals;

        ActionOutcomeDecision decision;
        ActionOutcomeDecisionResult decision_result;
        try {
            decision_result = decide(proposal);
        } catch (const std::exception& ex) {
            decision_result.ok = false;
            decision_result.error = exception_message("decision hook exception", ex);
        } catch (...) {
            decision_result.ok = false;
            decision_result.error =
                unknown_exception_message("decision hook exception");
        }
        if (decision_result.ok) {
            decision = decision_result.decision;
            if (decision.proposal_id.empty()) {
                decision.proposal_id = proposal.proposal_id;
            }
        } else {
            ++pass.decision_error_count;
            if (pass.decision_error_count >=
                config.max_decision_errors_per_pass) {
                fatal_error = decision_result.error.empty()
                    ? "decision error budget exhausted"
                    : "decision error budget exhausted:" + decision_result.error;
                break;
            }
            decision = failClosedDecision(
                proposal,
                config.fail_closed_action,
                decision_result.error.empty()
                    ? "decision_error_fail_closed"
                    : "decision_error_fail_closed:" + decision_result.error);
        }

        bool sink_ok = false;
        try {
            sink_ok = sink(decision, &sink_error);
        } catch (const std::exception& ex) {
            sink_error = exception_message("decision sink exception", ex);
        } catch (...) {
            sink_error = unknown_exception_message("decision sink exception");
        }
        if (!sink_ok) {
            ++pass.sink_error_count;
            if (pass.sink_error_count >= config.max_sink_errors_per_pass) {
                fatal_error = sink_error.empty()
                    ? "decision sink error budget exhausted"
                    : "decision sink error budget exhausted:" + sink_error;
                break;
            }
            continue;
        }
        ++pass.processed_count;
        pass.evidence_rows_written += decision.evidence_count;
        if (isSuppressDecision(decision)) {
            ++pass.suppress_count;
        } else {
            ++pass.allow_count;
        }
        if (decision.fail_closed || has_fail_closed_reason(decision)) {
            ++pass.fail_closed_count;
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    pass.latency_us = static_cast<uint64_t>(elapsed.count());

    {
        std::lock_guard<std::mutex> lock(mu_);
        last_pass_ = pass;
        ++worker_passes_total_;
        if (pass.batch_proposals == 0) {
            ++worker_idle_passes_total_;
        }
        proposals_processed_total_ += pass.processed_count;
        proposals_duplicate_total_ += pass.duplicate_count;
        proposals_rejected_total_ += pass.rejected_count;
        decisions_allow_total_ += pass.allow_count;
        decisions_suppress_total_ += pass.suppress_count;
        fail_closed_total_ += pass.fail_closed_count;
        evidence_rows_written_total_ += pass.evidence_rows_written;

        if (!fatal_error.empty()) {
            ++worker_failures_total_;
            ++consecutive_failures_;
            failure_budget_exhausted_ =
                consecutive_failures_ >= config_.max_consecutive_failures;
            last_error_ = fatal_error;
            if (error) *error = last_error_;
            return false;
        }
        if (pass.sink_error_count > 0) {
            ++worker_failures_total_;
            ++consecutive_failures_;
            failure_budget_exhausted_ =
                consecutive_failures_ >= config_.max_consecutive_failures;
            last_error_ = sink_error.empty() ? "decision sink failed" : sink_error;
            if (error) *error = last_error_;
            return false;
        }
        consecutive_failures_ = 0;
        failure_budget_exhausted_ = false;
        last_error_.clear();
    }
    return true;
}

ActionOutcomeSupervisorRuntimeSnapshot
ActionOutcomeSupervisorRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    ActionOutcomeSupervisorRuntimeSnapshot snap;
    snap.configured = configured_;
    snap.enabled = enabled_;
    snap.worker_running = worker_running_;
    snap.worker_hooks_configured = workerHooksConfiguredLocked();
    snap.failure_budget_exhausted = failure_budget_exhausted_;
    snap.name = config_.name;
    snap.mode = config_.mode;
    snap.history_table = config_.history_table;
    snap.proposal_table = config_.proposal_table;
    snap.decision_table = config_.decision_table;
    snap.evidence_table = config_.evidence_table;
    snap.fail_closed_action = config_.fail_closed_action;
    snap.worker_poll_interval_ms = config_.worker_poll_interval_ms;
    snap.batch_limit = config_.batch_limit;
    snap.max_consecutive_failures = config_.max_consecutive_failures;
    snap.max_decision_errors_per_pass = config_.max_decision_errors_per_pass;
    snap.max_sink_errors_per_pass = config_.max_sink_errors_per_pass;
    snap.consecutive_failures = consecutive_failures_;
    snap.configure_total = configure_total_;
    snap.start_total = start_total_;
    snap.stop_total = stop_total_;
    snap.start_failures_total = start_failures_total_;
    snap.stop_failures_total = stop_failures_total_;
    snap.worker_start_total = worker_start_total_;
    snap.worker_wakeups_total = worker_wakeups_total_;
    snap.worker_passes_total = worker_passes_total_;
    snap.worker_idle_passes_total = worker_idle_passes_total_;
    snap.worker_failures_total = worker_failures_total_;
    snap.proposals_processed_total = proposals_processed_total_;
    snap.proposals_duplicate_total = proposals_duplicate_total_;
    snap.proposals_rejected_total = proposals_rejected_total_;
    snap.decisions_allow_total = decisions_allow_total_;
    snap.decisions_suppress_total = decisions_suppress_total_;
    snap.fail_closed_total = fail_closed_total_;
    snap.evidence_rows_written_total = evidence_rows_written_total_;
    snap.last_pass = last_pass_;
    snap.last_error = last_error_;
    return snap;
}

std::string ActionOutcomeSupervisorRuntime::formatPrometheus() const {
    const auto snap = snapshot();
    const std::string label = sanitize_label_value(
        snap.name.empty() ? std::string("physical_ai_action_outcome") : snap.name);
    std::ostringstream os;
    os << "# HELP zepto_action_outcome_supervisor_configured Experimental Action-Outcome supervisor is configured\n";
    os << "# TYPE zepto_action_outcome_supervisor_configured gauge\n";
    os << "zepto_action_outcome_supervisor_configured{supervisor=\"" << label << "\"} "
       << (snap.configured ? 1 : 0) << "\n\n";

    os << "# HELP zepto_action_outcome_supervisor_enabled Experimental Action-Outcome supervisor is enabled\n";
    os << "# TYPE zepto_action_outcome_supervisor_enabled gauge\n";
    os << "zepto_action_outcome_supervisor_enabled{supervisor=\"" << label << "\"} "
       << (snap.enabled ? 1 : 0) << "\n\n";

    os << "# HELP zepto_action_outcome_supervisor_worker_running Experimental Action-Outcome worker is running\n";
    os << "# TYPE zepto_action_outcome_supervisor_worker_running gauge\n";
    os << "zepto_action_outcome_supervisor_worker_running{supervisor=\"" << label << "\"} "
       << (snap.worker_running ? 1 : 0) << "\n\n";

    os << "# HELP zepto_action_outcome_supervisor_worker_passes_total Experimental Action-Outcome worker passes\n";
    os << "# TYPE zepto_action_outcome_supervisor_worker_passes_total counter\n";
    os << "zepto_action_outcome_supervisor_worker_passes_total{supervisor=\"" << label << "\"} "
       << snap.worker_passes_total << "\n\n";

    os << "# HELP zepto_action_outcome_proposals_processed_total Action proposals processed by the experimental supervisor\n";
    os << "# TYPE zepto_action_outcome_proposals_processed_total counter\n";
    os << "zepto_action_outcome_proposals_processed_total{supervisor=\"" << label << "\"} "
       << snap.proposals_processed_total << "\n\n";

    os << "# HELP zepto_action_outcome_proposals_duplicate_total Action proposals skipped as already decided\n";
    os << "# TYPE zepto_action_outcome_proposals_duplicate_total counter\n";
    os << "zepto_action_outcome_proposals_duplicate_total{supervisor=\"" << label << "\"} "
       << snap.proposals_duplicate_total << "\n\n";

    os << "# HELP zepto_action_outcome_decisions_allow_total Allow decisions written by the experimental supervisor\n";
    os << "# TYPE zepto_action_outcome_decisions_allow_total counter\n";
    os << "zepto_action_outcome_decisions_allow_total{supervisor=\"" << label << "\"} "
       << snap.decisions_allow_total << "\n\n";

    os << "# HELP zepto_action_outcome_decisions_suppress_total Suppress or replace decisions written by the experimental supervisor\n";
    os << "# TYPE zepto_action_outcome_decisions_suppress_total counter\n";
    os << "zepto_action_outcome_decisions_suppress_total{supervisor=\"" << label << "\"} "
       << snap.decisions_suppress_total << "\n\n";

    os << "# HELP zepto_action_outcome_fail_closed_total Fail-closed decisions written by the experimental supervisor\n";
    os << "# TYPE zepto_action_outcome_fail_closed_total counter\n";
    os << "zepto_action_outcome_fail_closed_total{supervisor=\"" << label << "\"} "
       << snap.fail_closed_total << "\n\n";

    os << "# HELP zepto_action_outcome_evidence_rows_written_total Evidence rows written by the experimental supervisor\n";
    os << "# TYPE zepto_action_outcome_evidence_rows_written_total counter\n";
    os << "zepto_action_outcome_evidence_rows_written_total{supervisor=\"" << label << "\"} "
       << snap.evidence_rows_written_total << "\n\n";

    os << "# HELP zepto_action_outcome_worker_failures_total Experimental Action-Outcome worker failures\n";
    os << "# TYPE zepto_action_outcome_worker_failures_total counter\n";
    os << "zepto_action_outcome_worker_failures_total{supervisor=\"" << label << "\"} "
       << snap.worker_failures_total << "\n\n";

    os << "# HELP zepto_action_outcome_last_pass_latency_us Last Action-Outcome worker pass latency in microseconds\n";
    os << "# TYPE zepto_action_outcome_last_pass_latency_us gauge\n";
    os << "zepto_action_outcome_last_pass_latency_us{supervisor=\"" << label << "\"} "
       << snap.last_pass.latency_us << "\n";
    return os.str();
}

bool ActionOutcomeSupervisorRuntime::workerHooksConfiguredLocked() const {
    return static_cast<bool>(hooks_.load_proposals) &&
           static_cast<bool>(hooks_.decide) &&
           static_cast<bool>(hooks_.sink_decision);
}

bool ActionOutcomeSupervisorRuntime::isSuppressDecision(
    const ActionOutcomeDecision& decision) {
    return decision.decision != "allow";
}

ActionOutcomeDecision ActionOutcomeSupervisorRuntime::failClosedDecision(
    const ActionOutcomeProposal& proposal,
    const std::string& fail_closed_action,
    const std::string& reason) {
    ActionOutcomeDecision decision;
    decision.proposal_id = proposal.proposal_id;
    decision.decision = "suppress_no_evidence";
    decision.final_action = fail_closed_action;
    decision.reason = reason;
    decision.evidence_count = 0;
    decision.fail_closed = true;
    return decision;
}

void ActionOutcomeSupervisorRuntime::workerLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (worker_stop_requested_ || !enabled_) {
                break;
            }
        }

        std::string ignored;
        (void)runOnce(&ignored);

        std::unique_lock<std::mutex> lock(mu_);
        const auto interval = std::chrono::milliseconds(config_.worker_poll_interval_ms);
        worker_cv_.wait_for(lock, interval, [this] {
            return worker_stop_requested_ || !enabled_;
        });
        if (worker_stop_requested_ || !enabled_) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    worker_running_ = false;
}

void ActionOutcomeSupervisorRuntime::stopWorkerThread() {
    std::thread worker_to_join;
    {
        std::lock_guard<std::mutex> lock(mu_);
        worker_stop_requested_ = true;
        if (worker_.joinable()) {
            worker_to_join = std::move(worker_);
        }
    }
    worker_cv_.notify_all();
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }
}

} // namespace zeptodb::feeds
