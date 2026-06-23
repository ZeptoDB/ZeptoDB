// ============================================================================
// ZeptoDB: Experimental edge-to-fleet feed connector implementation
// ============================================================================

#include "zeptodb/feeds/edge_fleet_feed_connector.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace zeptodb::feeds {
namespace {

constexpr std::string_view kCheckpointHeader = "zeptodb_edge_fleet_feed_checkpoint_v1";

std::string sanitize_label_value(std::string_view value) {
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

EdgeFleetFeedConnector::EdgeFleetFeedConnector(EdgeFleetFeedConfig config,
                                               EdgeFleetFeedSink sink)
    : config_(std::move(config)), sink_(std::move(sink)) {
    if (config_.max_retries_per_event == 0) {
        config_.max_retries_per_event = 1;
    }
}

bool EdgeFleetFeedConnector::isValidConfig(const EdgeFleetFeedConfig& config) noexcept {
    return config.batch_limit > 0 &&
           config.max_inflight > 0 &&
           config.max_retries_per_event > 0;
}

std::optional<EdgeFleetEventKind> EdgeFleetFeedConnector::parseKind(std::string_view value) {
    if (value == "decision") return EdgeFleetEventKind::Decision;
    if (value == "retrieval") return EdgeFleetEventKind::Retrieval;
    if (value == "suppression") return EdgeFleetEventKind::Suppression;
    return std::nullopt;
}

std::string_view EdgeFleetFeedConnector::kindName(EdgeFleetEventKind kind) noexcept {
    switch (kind) {
        case EdgeFleetEventKind::Decision:
            return "decision";
        case EdgeFleetEventKind::Retrieval:
            return "retrieval";
        case EdgeFleetEventKind::Suppression:
            return "suppression";
    }
    return "unknown";
}

bool EdgeFleetFeedConnector::isAcked(std::string_view event_id) const {
    return acked_event_ids_.find(std::string(event_id)) != acked_event_ids_.end();
}

bool EdgeFleetFeedConnector::shouldDeliver(
    const EdgeFleetFeedEvent& event,
    std::unordered_set<std::string>* seen_this_pass,
    EdgeFleetFeedPassResult* result) {
    if (event.event_id.empty() || event.stream_seq == 0) {
        ++result->rejected_count;
        ++stats_.rejected_events;
        return false;
    }

    if (seen_this_pass->find(event.event_id) != seen_this_pass->end() ||
        acked_event_ids_.find(event.event_id) != acked_event_ids_.end()) {
        ++result->duplicate_count;
        ++stats_.duplicate_events;
        return false;
    }
    seen_this_pass->insert(event.event_id);

    if (event.stream_seq <= highest_acked_stream_seq_) {
        ++result->late_count;
        ++stats_.late_events;
        if (!config_.allow_late_events) {
            ++result->rejected_count;
            ++stats_.rejected_events;
            return false;
        }
    }
    return true;
}

bool EdgeFleetFeedConnector::ackEvent(const EdgeFleetFeedEvent& event) {
    const auto [_, inserted] = acked_event_ids_.insert(event.event_id);
    if (inserted && event.stream_seq > highest_acked_stream_seq_) {
        highest_acked_stream_seq_ = event.stream_seq;
    }
    acked_stream_seq_by_id_[event.event_id] = event.stream_seq;
    return inserted;
}

EdgeFleetFeedPassResult EdgeFleetFeedConnector::processOnce(
    const std::vector<EdgeFleetFeedEvent>& outbox) {
    EdgeFleetFeedPassResult result;
    result.outbox_events_seen = outbox.size();
    result.acked_before = acked_event_ids_.size();
    stats_.passes++;
    stats_.outbox_events_seen += outbox.size();

    if (!isValidConfig(config_) || !sink_) {
        result.rejected_count = outbox.size();
        stats_.rejected_events += outbox.size();
        result.acked_after = acked_event_ids_.size();
        result.highest_acked_stream_seq = highest_acked_stream_seq_;
        return result;
    }

    std::vector<EdgeFleetFeedEvent> ordered = outbox;
    std::sort(ordered.begin(), ordered.end(),
              [](const EdgeFleetFeedEvent& left, const EdgeFleetFeedEvent& right) {
                  if (left.stream_seq == right.stream_seq) {
                      return left.event_id < right.event_id;
                  }
                  return left.stream_seq < right.stream_seq;
              });

    std::vector<EdgeFleetFeedEvent> batch;
    batch.reserve(std::min(config_.batch_limit, config_.max_inflight));
    std::unordered_set<std::string> seen_this_pass;
    const size_t pass_limit = std::min(config_.batch_limit, config_.max_inflight);
    for (const auto& event : ordered) {
        if (batch.size() >= pass_limit) break;
        if (shouldDeliver(event, &seen_this_pass, &result)) {
            batch.push_back(event);
        }
    }

    result.batch_event_count = batch.size();
    stats_.max_inflight_observed = std::max<uint64_t>(
        stats_.max_inflight_observed, static_cast<uint64_t>(batch.size()));

    for (const auto& event : batch) {
        bool done = false;
        for (uint32_t attempt = 0; attempt < config_.max_retries_per_event && !done; ++attempt) {
            ++result.attempted_count;
            ++stats_.events_attempted;
            const EdgeFleetDeliveryResult delivery = sink_(event);
            switch (delivery) {
                case EdgeFleetDeliveryResult::Acked:
                    if (ackEvent(event)) {
                        ++result.acked_count;
                        ++stats_.events_acked;
                    } else {
                        ++result.duplicate_count;
                        ++stats_.duplicate_events;
                    }
                    done = true;
                    break;
                case EdgeFleetDeliveryResult::TransientFailure:
                    ++result.transient_failure_count;
                    ++stats_.transient_failures;
                    break;
                case EdgeFleetDeliveryResult::PermanentFailure:
                    ++result.permanent_failure_count;
                    ++stats_.permanent_failures;
                    done = true;
                    break;
                case EdgeFleetDeliveryResult::AppliedButAckFailed:
                    ++result.ack_boundary_failure_count;
                    ++stats_.ack_boundary_failures;
                    done = true;
                    break;
            }
        }
    }

    result.acked_after = acked_event_ids_.size();
    result.highest_acked_stream_seq = highest_acked_stream_seq_;
    if (!config_.checkpoint_path.empty() && result.acked_count > 0) {
        std::string ignored;
        (void)saveCheckpoint(&ignored);
    }
    return result;
}

bool EdgeFleetFeedConnector::loadCheckpoint(std::string* error) {
    if (config_.checkpoint_path.empty()) {
        return true;
    }

    std::ifstream in(config_.checkpoint_path);
    if (!in.good()) {
        if (error) *error = "checkpoint file is not readable";
        ++stats_.checkpoint_failures;
        return false;
    }

    std::string header;
    std::getline(in, header);
    if (header != kCheckpointHeader) {
        if (error) *error = "checkpoint header mismatch";
        ++stats_.checkpoint_failures;
        return false;
    }

    std::unordered_set<std::string> loaded_ids;
    std::unordered_map<std::string, uint64_t> loaded_seq_by_id;
    uint64_t loaded_highest = 0;
    std::string tag;
    while (in >> tag) {
        if (tag == "highest_acked_stream_seq") {
            in >> loaded_highest;
        } else if (tag == "acked") {
            std::string event_id;
            uint64_t stream_seq = 0;
            in >> stream_seq >> event_id;
            if (!event_id.empty()) {
                loaded_ids.insert(event_id);
                loaded_seq_by_id[event_id] = stream_seq;
                loaded_highest = std::max(loaded_highest, stream_seq);
            }
        } else {
            if (error) *error = "checkpoint contains unknown record";
            ++stats_.checkpoint_failures;
            return false;
        }
    }

    if (!in.eof()) {
        if (error) *error = "checkpoint parse failed";
        ++stats_.checkpoint_failures;
        return false;
    }

    acked_event_ids_ = std::move(loaded_ids);
    acked_stream_seq_by_id_ = std::move(loaded_seq_by_id);
    highest_acked_stream_seq_ = loaded_highest;
    ++stats_.checkpoint_loads;
    return true;
}

bool EdgeFleetFeedConnector::saveCheckpoint(std::string* error) {
    if (config_.checkpoint_path.empty()) {
        return true;
    }

    const std::filesystem::path path(config_.checkpoint_path);
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) *error = "failed to create checkpoint directory: " + ec.message();
            ++stats_.checkpoint_failures;
            return false;
        }
    }

    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.good()) {
            if (error) *error = "checkpoint file is not writable";
            ++stats_.checkpoint_failures;
            return false;
        }

        out << kCheckpointHeader << '\n';
        out << "highest_acked_stream_seq " << highest_acked_stream_seq_ << '\n';
        for (const auto& event_id : acked_event_ids_) {
            const auto seq_it = acked_stream_seq_by_id_.find(event_id);
            const uint64_t stream_seq =
                seq_it == acked_stream_seq_by_id_.end() ? 0 : seq_it->second;
            out << "acked " << stream_seq << ' ' << event_id << '\n';
        }
        if (!out.good()) {
            if (error) *error = "checkpoint write failed";
            ++stats_.checkpoint_failures;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        if (error) *error = "checkpoint rename failed: " + ec.message();
        ++stats_.checkpoint_failures;
        return false;
    }
    ++stats_.checkpoint_saves;
    return true;
}

std::string EdgeFleetFeedConnector::formatPrometheus(
    std::string_view connector_name,
    const EdgeFleetFeedStats& stats) {
    const std::string name = sanitize_label_value(connector_name);
    std::ostringstream out;
    auto line = [&](std::string_view metric, uint64_t value) {
        out << metric << "{connector=\"" << name << "\"} " << value << '\n';
    };

    out << "# TYPE zepto_edge_fleet_feed_passes_total counter\n";
    line("zepto_edge_fleet_feed_passes_total", stats.passes);
    out << "# TYPE zepto_edge_fleet_feed_events_attempted_total counter\n";
    line("zepto_edge_fleet_feed_events_attempted_total", stats.events_attempted);
    out << "# TYPE zepto_edge_fleet_feed_events_acked_total counter\n";
    line("zepto_edge_fleet_feed_events_acked_total", stats.events_acked);
    out << "# TYPE zepto_edge_fleet_feed_transient_failures_total counter\n";
    line("zepto_edge_fleet_feed_transient_failures_total", stats.transient_failures);
    out << "# TYPE zepto_edge_fleet_feed_permanent_failures_total counter\n";
    line("zepto_edge_fleet_feed_permanent_failures_total", stats.permanent_failures);
    out << "# TYPE zepto_edge_fleet_feed_ack_boundary_failures_total counter\n";
    line("zepto_edge_fleet_feed_ack_boundary_failures_total", stats.ack_boundary_failures);
    out << "# TYPE zepto_edge_fleet_feed_duplicate_events_total counter\n";
    line("zepto_edge_fleet_feed_duplicate_events_total", stats.duplicate_events);
    out << "# TYPE zepto_edge_fleet_feed_late_events_total counter\n";
    line("zepto_edge_fleet_feed_late_events_total", stats.late_events);
    out << "# TYPE zepto_edge_fleet_feed_rejected_events_total counter\n";
    line("zepto_edge_fleet_feed_rejected_events_total", stats.rejected_events);
    out << "# TYPE zepto_edge_fleet_feed_max_inflight_observed gauge\n";
    line("zepto_edge_fleet_feed_max_inflight_observed", stats.max_inflight_observed);
    return out.str();
}

} // namespace zeptodb::feeds
