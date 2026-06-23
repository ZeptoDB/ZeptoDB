#pragma once
// ============================================================================
// ZeptoDB: Experimental edge/fleet connector runtime lifecycle
// ============================================================================

#include "zeptodb/feeds/edge_fleet_feed_connector.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zeptodb::feeds {

/// Result returned by a runtime worker outbox loader.
struct EdgeFleetOutboxLoadResult {
    bool ok = false;
    std::vector<EdgeFleetFeedEvent> events;
    std::string error;
};

using EdgeFleetOutboxLoader = std::function<EdgeFleetOutboxLoadResult()>;
using EdgeFleetPassObserver =
    std::function<bool(const EdgeFleetFeedPassResult&, std::string*)>;

/// Runtime hooks supplied by the embedding server/application.
///
/// `load_outbox` fetches the next bounded edge outbox snapshot. `sink` applies
/// one event to the fleet side. `observe_pass` is optional pass telemetry.
/// Hooks are intentionally injected so the feed layer stays transport-neutral.
struct EdgeFleetConnectorRuntimeHooks {
    EdgeFleetOutboxLoader load_outbox;
    EdgeFleetFeedSink sink;
    EdgeFleetPassObserver observe_pass;
};

/// Server-managed configuration for the experimental edge/fleet connector.
///
/// Thread-safety: value type. The runtime manager copies this configuration
/// under its own mutex. Table names are metadata for lifecycle/status; actual
/// polling and sink execution are supplied through runtime hooks.
struct EdgeFleetConnectorRuntimeConfig {
    std::string name = "physical_ai_edge_fleet";
    std::string edge_outbox_table = "physical_ai_edge_feed_outbox_016";
    std::string fleet_ack_table = "physical_ai_fleet_feed_ack_016";
    bool worker_enabled = false;
    uint64_t worker_poll_interval_ms = 1000;
    EdgeFleetFeedConfig feed;
};

/// Snapshot of server-managed experimental connector lifecycle state.
///
/// Thread-safety: value type. Counters are cumulative for the owning runtime
/// instance. `feed_stats` is the underlying connector's latest observed stats.
struct EdgeFleetConnectorRuntimeSnapshot {
    bool configured = false;
    bool enabled = false;
    std::string name;
    std::string edge_outbox_table;
    std::string fleet_ack_table;
    std::string checkpoint_path;
    size_t batch_limit = 0;
    size_t max_inflight = 0;
    uint32_t max_retries_per_event = 0;
    bool allow_late_events = true;
    bool worker_enabled = false;
    bool worker_hooks_configured = false;
    bool worker_running = false;
    uint64_t worker_poll_interval_ms = 0;
    size_t acked_count = 0;
    uint64_t highest_acked_stream_seq = 0;
    uint64_t configure_total = 0;
    uint64_t start_total = 0;
    uint64_t stop_total = 0;
    uint64_t start_failures_total = 0;
    uint64_t stop_failures_total = 0;
    uint64_t worker_start_total = 0;
    uint64_t worker_wakeups_total = 0;
    uint64_t worker_passes_total = 0;
    uint64_t worker_idle_passes_total = 0;
    uint64_t worker_load_errors_total = 0;
    uint64_t worker_observer_errors_total = 0;
    EdgeFleetFeedPassResult last_pass;
    std::string last_error;
    EdgeFleetFeedStats feed_stats;
};

/// Cold-path lifecycle manager for the experimental edge/fleet connector.
///
/// Thread-safety: all public methods are internally synchronized. The manager
/// owns the connector instance, optional checkpoint path, and bounded worker
/// loop. It remains transport-neutral: callers supply outbox/sink hooks until
/// product-promotion gates add a built-in SQL/HTTP adapter.
class EdgeFleetConnectorRuntime {
public:
    EdgeFleetConnectorRuntime();
    ~EdgeFleetConnectorRuntime();

    EdgeFleetConnectorRuntime(const EdgeFleetConnectorRuntime&) = delete;
    EdgeFleetConnectorRuntime& operator=(const EdgeFleetConnectorRuntime&) = delete;

    /// Store a new configuration and create a stopped connector instance.
    /// Returns false when limits or required metadata are invalid.
    bool configure(EdgeFleetConnectorRuntimeConfig config,
                   std::string* error = nullptr);

    /// Store worker hooks used by `runOnce` and background worker mode.
    ///
    /// This must be called while stopped when changing the sink because the
    /// underlying connector binds the sink callback at construction.
    bool setWorkerHooks(EdgeFleetConnectorRuntimeHooks hooks,
                        std::string* error = nullptr);

    /// Enable the configured connector and load checkpoint state when a
    /// checkpoint file already exists. Missing checkpoint files start empty.
    bool start(std::string* error = nullptr);

    /// Disable the connector and persist checkpoint state when configured.
    bool stop(std::string* error = nullptr);

    /// Disable and remove the current configuration.
    bool clear(std::string* error = nullptr);

    /// Execute one bounded worker pass using the installed hooks.
    bool runOnce(std::string* error = nullptr);

    EdgeFleetConnectorRuntimeSnapshot snapshot() const;
    std::string formatPrometheus() const;

private:
    static EdgeFleetDeliveryResult lifecycleOnlySink(const EdgeFleetFeedEvent&);
    static bool validateConfig(const EdgeFleetConnectorRuntimeConfig& config,
                               std::string* error);
    bool workerHooksConfiguredLocked() const;
    EdgeFleetFeedSink currentSinkLocked() const;
    void workerLoop();
    void stopWorkerThread();

    mutable std::mutex mu_;
    std::condition_variable worker_cv_;
    EdgeFleetConnectorRuntimeConfig config_;
    EdgeFleetConnectorRuntimeHooks hooks_;
    std::unique_ptr<EdgeFleetFeedConnector> connector_;
    std::thread worker_;
    bool configured_ = false;
    bool enabled_ = false;
    bool worker_running_ = false;
    bool worker_stop_requested_ = false;
    uint64_t configure_total_ = 0;
    uint64_t start_total_ = 0;
    uint64_t stop_total_ = 0;
    uint64_t start_failures_total_ = 0;
    uint64_t stop_failures_total_ = 0;
    uint64_t worker_start_total_ = 0;
    uint64_t worker_wakeups_total_ = 0;
    uint64_t worker_passes_total_ = 0;
    uint64_t worker_idle_passes_total_ = 0;
    uint64_t worker_load_errors_total_ = 0;
    uint64_t worker_observer_errors_total_ = 0;
    EdgeFleetFeedPassResult last_pass_;
    std::string last_error_;
};

} // namespace zeptodb::feeds
