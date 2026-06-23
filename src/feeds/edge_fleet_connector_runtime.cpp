// ============================================================================
// ZeptoDB: Experimental edge/fleet connector runtime lifecycle
// ============================================================================

#include "zeptodb/feeds/edge_fleet_connector_runtime.h"

#include <chrono>
#include <filesystem>
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

} // namespace

EdgeFleetConnectorRuntime::EdgeFleetConnectorRuntime() = default;

EdgeFleetConnectorRuntime::~EdgeFleetConnectorRuntime() {
    if (snapshot().configured) {
        std::string ignored;
        (void)stop(&ignored);
    } else {
        stopWorkerThread();
    }
}

EdgeFleetDeliveryResult EdgeFleetConnectorRuntime::lifecycleOnlySink(
    const EdgeFleetFeedEvent&) {
    return EdgeFleetDeliveryResult::PermanentFailure;
}

bool EdgeFleetConnectorRuntime::validateConfig(
    const EdgeFleetConnectorRuntimeConfig& config,
    std::string* error) {
    if (config.name.empty()) {
        if (error) *error = "connector name is required";
        return false;
    }
    if (config.edge_outbox_table.empty()) {
        if (error) *error = "edge_outbox_table is required";
        return false;
    }
    if (config.fleet_ack_table.empty()) {
        if (error) *error = "fleet_ack_table is required";
        return false;
    }
    if (!EdgeFleetFeedConnector::isValidConfig(config.feed)) {
        if (error) *error = "batch_limit, max_inflight, and max_retries_per_event must be positive";
        return false;
    }
    if (config.worker_poll_interval_ms == 0) {
        if (error) *error = "worker_poll_interval_ms must be positive";
        return false;
    }
    return true;
}

bool EdgeFleetConnectorRuntime::configure(EdgeFleetConnectorRuntimeConfig config,
                                          std::string* error) {
    if (snapshot().enabled) {
        std::string stop_error;
        if (!stop(&stop_error)) {
            if (error) *error = "failed to stop existing connector: " + stop_error;
            return false;
        }
    }

    if (!validateConfig(config, error)) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = error ? *error : "invalid connector config";
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    config_ = std::move(config);
    connector_ = std::make_unique<EdgeFleetFeedConnector>(
        config_.feed, currentSinkLocked());
    configured_ = true;
    enabled_ = false;
    last_error_.clear();
    ++configure_total_;
    return true;
}

bool EdgeFleetConnectorRuntime::setWorkerHooks(
    EdgeFleetConnectorRuntimeHooks hooks,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled_) {
        last_error_ = "stop connector before changing worker hooks";
        if (error) *error = last_error_;
        return false;
    }

    hooks_ = std::move(hooks);
    if (configured_) {
        connector_ = std::make_unique<EdgeFleetFeedConnector>(
            config_.feed, currentSinkLocked());
    }
    last_error_.clear();
    return true;
}

bool EdgeFleetConnectorRuntime::start(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_ || !connector_) {
            last_error_ = "connector is not configured";
            if (error) *error = last_error_;
            ++start_failures_total_;
            return false;
        }
        if (config_.worker_enabled && !workerHooksConfiguredLocked()) {
            last_error_ = "worker mode requires outbox loader and fleet sink hooks";
            if (error) *error = last_error_;
            ++start_failures_total_;
            return false;
        }
    }

    const std::string checkpoint_path = snapshot().checkpoint_path;
    if (!checkpoint_path.empty() && std::filesystem::exists(checkpoint_path)) {
        std::string load_error;
        std::lock_guard<std::mutex> lock(mu_);
        if (connector_ && !connector_->loadCheckpoint(&load_error)) {
            last_error_ = load_error.empty() ? "checkpoint load failed" : load_error;
            if (error) *error = last_error_;
            ++start_failures_total_;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        enabled_ = true;
        worker_stop_requested_ = false;
        last_error_.clear();
        ++start_total_;
        if (config_.worker_enabled) {
            worker_running_ = true;
            ++worker_start_total_;
            worker_ = std::thread(&EdgeFleetConnectorRuntime::workerLoop, this);
        }
    }
    return true;
}

bool EdgeFleetConnectorRuntime::stop(std::string* error) {
    stopWorkerThread();

    std::lock_guard<std::mutex> lock(mu_);
    if (!configured_ || !connector_) {
        last_error_ = "connector is not configured";
        if (error) *error = last_error_;
        ++stop_failures_total_;
        return false;
    }

    std::string save_error;
    if (enabled_ && !connector_->saveCheckpoint(&save_error)) {
        last_error_ = save_error.empty() ? "checkpoint save failed" : save_error;
        if (error) *error = last_error_;
        ++stop_failures_total_;
        return false;
    }

    enabled_ = false;
    last_error_.clear();
    ++stop_total_;
    return true;
}

bool EdgeFleetConnectorRuntime::clear(std::string* error) {
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
    connector_.reset();
    config_ = EdgeFleetConnectorRuntimeConfig{};
    configured_ = false;
    enabled_ = false;
    worker_running_ = false;
    worker_stop_requested_ = false;
    last_pass_ = EdgeFleetFeedPassResult{};
    last_error_.clear();
    return true;
}

bool EdgeFleetConnectorRuntime::runOnce(std::string* error) {
    EdgeFleetOutboxLoader loader;
    EdgeFleetPassObserver observer;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_ || !connector_) {
            last_error_ = "connector is not configured";
            if (error) *error = last_error_;
            return false;
        }
        if (!enabled_) {
            last_error_ = "connector is not enabled";
            if (error) *error = last_error_;
            return false;
        }
        if (!workerHooksConfiguredLocked()) {
            last_error_ = "worker hooks are not configured";
            if (error) *error = last_error_;
            return false;
        }
        loader = hooks_.load_outbox;
        observer = hooks_.observe_pass;
        ++worker_wakeups_total_;
    }

    EdgeFleetOutboxLoadResult loaded = loader();
    if (!loaded.ok) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = loaded.error.empty() ? "outbox load failed" : loaded.error;
        if (error) *error = last_error_;
        ++worker_load_errors_total_;
        return false;
    }

    EdgeFleetFeedPassResult pass;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!enabled_ || !connector_) {
            last_error_ = "connector stopped before worker pass";
            if (error) *error = last_error_;
            return false;
        }
        pass = connector_->processOnce(loaded.events);
        last_pass_ = pass;
        ++worker_passes_total_;
        if (pass.batch_event_count == 0) {
            ++worker_idle_passes_total_;
        }
    }

    if (observer) {
        std::string observe_error;
        if (!observer(pass, &observe_error)) {
            std::lock_guard<std::mutex> lock(mu_);
            last_error_ = observe_error.empty()
                ? "worker pass observer failed"
                : observe_error;
            if (error) *error = last_error_;
            ++worker_observer_errors_total_;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    last_error_.clear();
    return true;
}

EdgeFleetConnectorRuntimeSnapshot EdgeFleetConnectorRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    EdgeFleetConnectorRuntimeSnapshot snap;
    snap.configured = configured_;
    snap.enabled = enabled_;
    snap.name = config_.name;
    snap.edge_outbox_table = config_.edge_outbox_table;
    snap.fleet_ack_table = config_.fleet_ack_table;
    snap.checkpoint_path = config_.feed.checkpoint_path;
    snap.batch_limit = config_.feed.batch_limit;
    snap.max_inflight = config_.feed.max_inflight;
    snap.max_retries_per_event = config_.feed.max_retries_per_event;
    snap.allow_late_events = config_.feed.allow_late_events;
    snap.worker_enabled = config_.worker_enabled;
    snap.worker_hooks_configured = workerHooksConfiguredLocked();
    snap.worker_running = worker_running_;
    snap.worker_poll_interval_ms = config_.worker_poll_interval_ms;
    snap.configure_total = configure_total_;
    snap.start_total = start_total_;
    snap.stop_total = stop_total_;
    snap.start_failures_total = start_failures_total_;
    snap.stop_failures_total = stop_failures_total_;
    snap.worker_start_total = worker_start_total_;
    snap.worker_wakeups_total = worker_wakeups_total_;
    snap.worker_passes_total = worker_passes_total_;
    snap.worker_idle_passes_total = worker_idle_passes_total_;
    snap.worker_load_errors_total = worker_load_errors_total_;
    snap.worker_observer_errors_total = worker_observer_errors_total_;
    snap.last_pass = last_pass_;
    snap.last_error = last_error_;
    if (connector_) {
        snap.acked_count = connector_->ackedCount();
        snap.highest_acked_stream_seq = connector_->highestAckedStreamSeq();
        snap.feed_stats = connector_->stats();
    }
    return snap;
}

std::string EdgeFleetConnectorRuntime::formatPrometheus() const {
    const auto snap = snapshot();
    const std::string label = sanitize_label_value(snap.name.empty()
        ? std::string("physical_ai_edge_fleet")
        : snap.name);
    std::ostringstream os;
    os << "# HELP zepto_edge_fleet_connector_configured Experimental edge/fleet connector is configured\n";
    os << "# TYPE zepto_edge_fleet_connector_configured gauge\n";
    os << "zepto_edge_fleet_connector_configured{connector=\"" << label << "\"} "
       << (snap.configured ? 1 : 0) << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_enabled Experimental edge/fleet connector is enabled\n";
    os << "# TYPE zepto_edge_fleet_connector_enabled gauge\n";
    os << "zepto_edge_fleet_connector_enabled{connector=\"" << label << "\"} "
       << (snap.enabled ? 1 : 0) << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_configure_total Experimental edge/fleet connector configuration updates\n";
    os << "# TYPE zepto_edge_fleet_connector_configure_total counter\n";
    os << "zepto_edge_fleet_connector_configure_total{connector=\"" << label << "\"} "
       << snap.configure_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_start_total Experimental edge/fleet connector start attempts that succeeded\n";
    os << "# TYPE zepto_edge_fleet_connector_start_total counter\n";
    os << "zepto_edge_fleet_connector_start_total{connector=\"" << label << "\"} "
       << snap.start_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_stop_total Experimental edge/fleet connector stop attempts that succeeded\n";
    os << "# TYPE zepto_edge_fleet_connector_stop_total counter\n";
    os << "zepto_edge_fleet_connector_stop_total{connector=\"" << label << "\"} "
       << snap.stop_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_start_failures_total Experimental edge/fleet connector start failures\n";
    os << "# TYPE zepto_edge_fleet_connector_start_failures_total counter\n";
    os << "zepto_edge_fleet_connector_start_failures_total{connector=\"" << label << "\"} "
       << snap.start_failures_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_stop_failures_total Experimental edge/fleet connector stop failures\n";
    os << "# TYPE zepto_edge_fleet_connector_stop_failures_total counter\n";
    os << "zepto_edge_fleet_connector_stop_failures_total{connector=\"" << label << "\"} "
       << snap.stop_failures_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_worker_running Experimental edge/fleet connector worker is running\n";
    os << "# TYPE zepto_edge_fleet_connector_worker_running gauge\n";
    os << "zepto_edge_fleet_connector_worker_running{connector=\"" << label << "\"} "
       << (snap.worker_running ? 1 : 0) << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_worker_passes_total Experimental edge/fleet connector worker passes\n";
    os << "# TYPE zepto_edge_fleet_connector_worker_passes_total counter\n";
    os << "zepto_edge_fleet_connector_worker_passes_total{connector=\"" << label << "\"} "
       << snap.worker_passes_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_worker_load_errors_total Experimental edge/fleet connector worker outbox load errors\n";
    os << "# TYPE zepto_edge_fleet_connector_worker_load_errors_total counter\n";
    os << "zepto_edge_fleet_connector_worker_load_errors_total{connector=\"" << label << "\"} "
       << snap.worker_load_errors_total << "\n\n";

    os << "# HELP zepto_edge_fleet_connector_worker_observer_errors_total Experimental edge/fleet connector worker observer errors\n";
    os << "# TYPE zepto_edge_fleet_connector_worker_observer_errors_total counter\n";
    os << "zepto_edge_fleet_connector_worker_observer_errors_total{connector=\"" << label << "\"} "
       << snap.worker_observer_errors_total << "\n\n";

    os << EdgeFleetFeedConnector::formatPrometheus(snap.name, snap.feed_stats);
    return os.str();
}

bool EdgeFleetConnectorRuntime::workerHooksConfiguredLocked() const {
    return static_cast<bool>(hooks_.load_outbox) && static_cast<bool>(hooks_.sink);
}

EdgeFleetFeedSink EdgeFleetConnectorRuntime::currentSinkLocked() const {
    return hooks_.sink ? hooks_.sink : lifecycleOnlySink;
}

void EdgeFleetConnectorRuntime::workerLoop() {
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

void EdgeFleetConnectorRuntime::stopWorkerThread() {
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
