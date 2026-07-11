#pragma once
// ============================================================================
// ZeptoDB: Experimental Action-Outcome supervisor runtime lifecycle
// ============================================================================

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zeptodb::feeds {

/// Pending Physical AI action proposal loaded from the configured source.
///
/// Thread-safety: value type. `proposal_id` is the idempotency key used by the
/// runtime and by DB-backed sinks.
struct ActionOutcomeProposal {
    std::string proposal_id;
    std::string source_type;
    std::string proposed_action;
    int64_t source_ts_ns = 0;
};

/// Advisory supervisor decision produced for one action proposal.
///
/// Thread-safety: value type. The runtime does not publish actuator commands;
/// it forwards this decision to the configured sink for audit/shadow use.
struct ActionOutcomeDecision {
    std::string proposal_id;
    std::string decision = "suppress_no_evidence";
    std::string final_action = "manual_review";
    std::string reason;
    uint64_t evidence_count = 0;
    bool fail_closed = false;
};

/// Result returned by a runtime proposal loader hook.
struct ActionOutcomeProposalLoadResult {
    bool ok = false;
    std::vector<ActionOutcomeProposal> proposals;
    std::string error;
};

/// Result returned by a runtime decision hook.
struct ActionOutcomeDecisionResult {
    bool ok = false;
    ActionOutcomeDecision decision;
    std::string error;
};

using ActionOutcomeProposalLoader =
    std::function<ActionOutcomeProposalLoadResult()>;
using ActionOutcomeAlreadyDecided =
    std::function<bool(const std::string& proposal_id)>;
using ActionOutcomeDecisionProvider =
    std::function<ActionOutcomeDecisionResult(const ActionOutcomeProposal&)>;
using ActionOutcomeDecisionSink =
    std::function<bool(const ActionOutcomeDecision&, std::string*)>;

/// Runtime hooks supplied by the embedding server/application.
///
/// `load_proposals` reads a bounded source snapshot. `already_decided` is
/// optional and should check the durable decision sink when available.
/// `decide` computes an advisory action decision. `sink_decision` persists the
/// decision/evidence record.
struct ActionOutcomeSupervisorRuntimeHooks {
    ActionOutcomeProposalLoader load_proposals;
    ActionOutcomeAlreadyDecided already_decided;
    ActionOutcomeDecisionProvider decide;
    ActionOutcomeDecisionSink sink_decision;
};

/// Server-managed configuration for the experimental Action-Outcome supervisor.
///
/// Thread-safety: value type. The supported rollout stage is an admin-gated
/// controlled shadow pilot; promoted operator use and actuator enforcement are
/// intentionally out of scope until the GA gates explicitly change.
struct ActionOutcomeSupervisorRuntimeConfig {
    std::string name = "physical_ai_action_outcome";
    std::string mode = "shadow";
    std::string rollout_stage = "controlled_shadow_pilot";
    std::string history_table = "physical_ai_action_history";
    std::string proposal_table = "physical_ai_action_proposals";
    std::string decision_table = "physical_ai_supervision_decisions";
    std::string evidence_table = "physical_ai_supervision_evidence";
    std::string fail_closed_action = "manual_review";
    bool worker_enabled = false;
    uint64_t worker_poll_interval_ms = 1000;
    size_t batch_limit = 128;
    uint32_t max_consecutive_failures = 3;
    uint32_t max_decision_errors_per_pass = 16;
    uint32_t max_sink_errors_per_pass = 16;
};

/// Per-pass worker result.
struct ActionOutcomeSupervisorPassResult {
    uint64_t proposals_seen = 0;
    uint64_t batch_proposals = 0;
    uint64_t processed_count = 0;
    uint64_t duplicate_count = 0;
    uint64_t rejected_count = 0;
    uint64_t decision_error_count = 0;
    uint64_t sink_error_count = 0;
    uint64_t allow_count = 0;
    uint64_t suppress_count = 0;
    uint64_t fail_closed_count = 0;
    uint64_t evidence_rows_written = 0;
    uint64_t latency_us = 0;
};

/// Snapshot of server-managed experimental supervisor lifecycle state.
///
/// Thread-safety: value type. Counters are cumulative for the owning runtime
/// instance.
struct ActionOutcomeSupervisorRuntimeSnapshot {
    bool configured = false;
    bool enabled = false;
    bool worker_running = false;
    bool worker_hooks_configured = false;
    bool failure_budget_exhausted = false;
    std::string name;
    std::string mode;
    std::string rollout_stage;
    std::string history_table;
    std::string proposal_table;
    std::string decision_table;
    std::string evidence_table;
    std::string fail_closed_action;
    uint64_t worker_poll_interval_ms = 0;
    size_t batch_limit = 0;
    uint32_t max_consecutive_failures = 0;
    uint32_t max_decision_errors_per_pass = 0;
    uint32_t max_sink_errors_per_pass = 0;
    uint32_t consecutive_failures = 0;
    uint64_t configure_total = 0;
    uint64_t start_total = 0;
    uint64_t stop_total = 0;
    uint64_t start_failures_total = 0;
    uint64_t stop_failures_total = 0;
    uint64_t worker_start_total = 0;
    uint64_t worker_wakeups_total = 0;
    uint64_t worker_passes_total = 0;
    uint64_t worker_idle_passes_total = 0;
    uint64_t worker_failures_total = 0;
    uint64_t proposals_processed_total = 0;
    uint64_t proposals_duplicate_total = 0;
    uint64_t proposals_rejected_total = 0;
    uint64_t decisions_allow_total = 0;
    uint64_t decisions_suppress_total = 0;
    uint64_t fail_closed_total = 0;
    uint64_t evidence_rows_written_total = 0;
    ActionOutcomeSupervisorPassResult last_pass;
    std::string last_error;
};

/// Cold-path lifecycle manager for the experimental Action-Outcome supervisor.
///
/// Thread-safety: all public methods are internally synchronized. This runtime
/// owns lifecycle, worker pacing, idempotent-skip checks, fail-closed fallback,
/// and metrics. Source readers and DB sinks are supplied through hooks.
class ActionOutcomeSupervisorRuntime {
public:
    ActionOutcomeSupervisorRuntime();
    ~ActionOutcomeSupervisorRuntime();

    ActionOutcomeSupervisorRuntime(const ActionOutcomeSupervisorRuntime&) = delete;
    ActionOutcomeSupervisorRuntime& operator=(const ActionOutcomeSupervisorRuntime&) = delete;

    /// Store a new configuration. Returns false when limits, mode, or table
    /// names are invalid.
    bool configure(ActionOutcomeSupervisorRuntimeConfig config,
                   std::string* error = nullptr);

    /// Store worker hooks used by `runOnce` and background worker mode.
    bool setWorkerHooks(ActionOutcomeSupervisorRuntimeHooks hooks,
                        std::string* error = nullptr);

    /// Enable the configured supervisor. Worker mode requires hooks.
    bool start(std::string* error = nullptr);

    /// Disable the configured supervisor.
    bool stop(std::string* error = nullptr);

    /// Disable and remove the current configuration.
    bool clear(std::string* error = nullptr);

    /// Execute one bounded shadow worker pass using the installed hooks.
    bool runOnce(std::string* error = nullptr);

    ActionOutcomeSupervisorRuntimeSnapshot snapshot() const;
    std::string formatPrometheus() const;

private:
    static bool validateConfig(const ActionOutcomeSupervisorRuntimeConfig& config,
                               std::string* error);
    bool workerHooksConfiguredLocked() const;
    static bool isSuppressDecision(const ActionOutcomeDecision& decision);
    static ActionOutcomeDecision failClosedDecision(const ActionOutcomeProposal& proposal,
                                                    const std::string& fail_closed_action,
                                                    const std::string& reason);
    void workerLoop();
    void stopWorkerThread();

    mutable std::mutex mu_;
    std::condition_variable worker_cv_;
    ActionOutcomeSupervisorRuntimeConfig config_;
    ActionOutcomeSupervisorRuntimeHooks hooks_;
    std::thread worker_;
    bool configured_ = false;
    bool enabled_ = false;
    bool worker_running_ = false;
    bool worker_stop_requested_ = false;
    bool failure_budget_exhausted_ = false;
    uint32_t consecutive_failures_ = 0;
    uint64_t configure_total_ = 0;
    uint64_t start_total_ = 0;
    uint64_t stop_total_ = 0;
    uint64_t start_failures_total_ = 0;
    uint64_t stop_failures_total_ = 0;
    uint64_t worker_start_total_ = 0;
    uint64_t worker_wakeups_total_ = 0;
    uint64_t worker_passes_total_ = 0;
    uint64_t worker_idle_passes_total_ = 0;
    uint64_t worker_failures_total_ = 0;
    uint64_t proposals_processed_total_ = 0;
    uint64_t proposals_duplicate_total_ = 0;
    uint64_t proposals_rejected_total_ = 0;
    uint64_t decisions_allow_total_ = 0;
    uint64_t decisions_suppress_total_ = 0;
    uint64_t fail_closed_total_ = 0;
    uint64_t evidence_rows_written_total_ = 0;
    ActionOutcomeSupervisorPassResult last_pass_;
    std::string last_error_;
};

} // namespace zeptodb::feeds
