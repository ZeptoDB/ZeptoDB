#pragma once
// ============================================================================
// ZeptoDB: Experimental edge-to-fleet feed connector
// ============================================================================
// Bounded runtime connector for Physical AI Action-Outcome evidence transfer.
// The connector owns delivery state, ACK checkpointing, retry accounting, and
// duplicate/late handling. Transport is injected as a sink callback so HTTP,
// RPC, or SQL-table adapters can be layered on without changing the state
// machine.
// ============================================================================

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zeptodb::feeds {

/// Edge-generated event kind accepted by the experimental edge/fleet feed.
enum class EdgeFleetEventKind {
    Decision,
    Retrieval,
    Suppression,
};

/// Sink result for one attempted feed delivery.
enum class EdgeFleetDeliveryResult {
    /// The fleet sink applied the event and durably acknowledged it.
    Acked,
    /// The delivery failed transiently; the connector may retry in a later pass.
    TransientFailure,
    /// The delivery is invalid for this sink; the event remains unacknowledged.
    PermanentFailure,
    /// The sink applied the final fleet row but failed before ACK persistence.
    /// The connector intentionally leaves the event unacknowledged so a later
    /// pass can replay it idempotently.
    AppliedButAckFailed,
};

/// One edge outbox event.
///
/// Thread-safety: value type; safe to copy between threads. The connector
/// treats `stream_seq` as a monotonically increasing per-edge sequence and
/// `ready_ts_ns` as an informational nanosecond timestamp used by adapters.
struct EdgeFleetFeedEvent {
    std::string event_id;
    uint64_t stream_seq = 0;
    EdgeFleetEventKind kind = EdgeFleetEventKind::Decision;
    int64_t ready_ts_ns = 0;
    std::string query_id;
    std::string payload_json;
};

/// Experimental connector limits and optional persistence.
///
/// `batch_limit` and `max_inflight` bound each process pass. `checkpoint_path`
/// stores acknowledged event ids and the highest acknowledged stream sequence.
struct EdgeFleetFeedConfig {
    size_t batch_limit = 128;
    size_t max_inflight = 128;
    uint32_t max_retries_per_event = 1;
    bool allow_late_events = true;
    std::string checkpoint_path;
};

/// Cumulative connector counters.
struct EdgeFleetFeedStats {
    uint64_t passes = 0;
    uint64_t outbox_events_seen = 0;
    uint64_t events_attempted = 0;
    uint64_t events_acked = 0;
    uint64_t transient_failures = 0;
    uint64_t permanent_failures = 0;
    uint64_t ack_boundary_failures = 0;
    uint64_t duplicate_events = 0;
    uint64_t late_events = 0;
    uint64_t rejected_events = 0;
    uint64_t checkpoint_loads = 0;
    uint64_t checkpoint_saves = 0;
    uint64_t checkpoint_failures = 0;
    uint64_t max_inflight_observed = 0;
};

/// Per-pass result returned by `processOnce`.
struct EdgeFleetFeedPassResult {
    size_t outbox_events_seen = 0;
    size_t batch_event_count = 0;
    size_t attempted_count = 0;
    size_t acked_count = 0;
    size_t transient_failure_count = 0;
    size_t permanent_failure_count = 0;
    size_t ack_boundary_failure_count = 0;
    size_t duplicate_count = 0;
    size_t late_count = 0;
    size_t rejected_count = 0;
    size_t acked_before = 0;
    size_t acked_after = 0;
    uint64_t highest_acked_stream_seq = 0;
};

using EdgeFleetFeedSink =
    std::function<EdgeFleetDeliveryResult(const EdgeFleetFeedEvent&)>;

/// Experimental bounded edge-to-fleet feed connector.
///
/// Thread-safety: not internally synchronized; use from one worker thread or
/// externally serialize calls. Ownership: the connector owns only ACK state and
/// an optional checkpoint file path. The sink callback is borrowed and invoked
/// without locks. Error conditions are reported through pass results and stats;
/// checkpoint parse/write errors return false and increment checkpoint_failures.
class EdgeFleetFeedConnector {
public:
    EdgeFleetFeedConnector(EdgeFleetFeedConfig config, EdgeFleetFeedSink sink);

    /// Process one bounded pass over caller-provided edge outbox events.
    ///
    /// The caller supplies events in any order; the connector sorts candidates
    /// by stream_seq before delivery. Already ACKed event ids are skipped as
    /// duplicates. Empty event ids and stream_seq=0 are rejected.
    EdgeFleetFeedPassResult processOnce(const std::vector<EdgeFleetFeedEvent>& outbox);

    /// Load ACK checkpoint state from config.checkpoint_path. Empty path is a
    /// no-op success.
    bool loadCheckpoint(std::string* error = nullptr);

    /// Persist ACK checkpoint state to config.checkpoint_path. Empty path is a
    /// no-op success.
    bool saveCheckpoint(std::string* error = nullptr);

    bool isAcked(std::string_view event_id) const;
    size_t ackedCount() const noexcept { return acked_event_ids_.size(); }
    uint64_t highestAckedStreamSeq() const noexcept { return highest_acked_stream_seq_; }
    EdgeFleetFeedStats stats() const noexcept { return stats_; }
    const EdgeFleetFeedConfig& config() const noexcept { return config_; }

    static bool isValidConfig(const EdgeFleetFeedConfig& config) noexcept;
    static std::optional<EdgeFleetEventKind> parseKind(std::string_view value);
    static std::string_view kindName(EdgeFleetEventKind kind) noexcept;
    static std::string formatPrometheus(std::string_view connector_name,
                                        const EdgeFleetFeedStats& stats);

private:
    bool shouldDeliver(const EdgeFleetFeedEvent& event,
                       std::unordered_set<std::string>* seen_this_pass,
                       EdgeFleetFeedPassResult* result);
    bool ackEvent(const EdgeFleetFeedEvent& event);

    EdgeFleetFeedConfig config_;
    EdgeFleetFeedSink sink_;
    std::unordered_set<std::string> acked_event_ids_;
    std::unordered_map<std::string, uint64_t> acked_stream_seq_by_id_;
    uint64_t highest_acked_stream_seq_ = 0;
    EdgeFleetFeedStats stats_;
};

} // namespace zeptodb::feeds
