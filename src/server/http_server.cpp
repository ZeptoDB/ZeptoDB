// ============================================================================
// ZeptoDB: HTTP API Server Implementation
// ============================================================================
// cpp-httplib based lightweight HTTP/HTTPS server.
// ClickHouse compatible port 8123.
// ============================================================================

// Enable TLS support if OpenSSL is available
#ifdef ZEPTO_TLS_ENABLED
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

// httplib is header-only — include only in .cpp (compile speed)
#include "third_party/httplib.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/ai/agent_memory.h"
#include "zeptodb/ai/agent_memory_wire.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/server/arrow_ipc.h"
#include "zeptodb/server/msgpack_ingest.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/auth/cancellation_token.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/auth/session_store.h"
#include "zeptodb/auth/oauth2_token_exchange.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/util/logger.h"
#include "zeptodb/auth/license_validator.h"

#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <cstdio>
#include <cmath>
#include <future>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <limits>
#include <random>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace zeptodb::server {

// ============================================================================
// Request logging helpers
// ============================================================================

static std::atomic<uint64_t> g_request_seq{0};

/// Generate a short request ID: "r" + hex(monotonic counter)
static std::string gen_request_id() {
    uint64_t seq = g_request_seq.fetch_add(1, std::memory_order_relaxed);
    char buf[20];
    std::snprintf(buf, sizeof(buf), "r%06lx", static_cast<unsigned long>(seq & 0xFFFFFF));
    return buf;
}

/// Current epoch-microseconds
static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Build a structured JSON access log line.
/// Fields follow the OpenTelemetry semantic conventions for HTTP spans.
static std::string build_access_log(
    const std::string& request_id,
    const std::string& method,
    const std::string& path,
    int status,
    int64_t duration_us,
    size_t request_bytes,
    size_t response_bytes,
    const std::string& remote_addr,
    const std::string& subject)
{
    std::ostringstream os;
    os << "{\"request_id\":\"" << request_id << "\""
       << ",\"method\":\"" << method << "\""
       << ",\"path\":\"" << path << "\""
       << ",\"status\":" << status
       << ",\"duration_us\":" << duration_us
       << ",\"request_bytes\":" << request_bytes
       << ",\"response_bytes\":" << response_bytes
       << ",\"remote_addr\":\"" << remote_addr << "\"";
    if (!subject.empty())
        os << ",\"subject\":\"" << subject << "\"";
    os << "}";
    return os.str();
}

/// Build a standard 402 JSON response for enterprise-gated features.
static std::string build_402_json(const std::string& feature_name) {
    return R"({"error":"enterprise_required","message":")" + feature_name +
           R"( requires Enterprise license","upgrade_url":"https://zeptodb.com/pricing"})";
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            std::ostringstream os;
            os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<int>(c);
            out += os.str();
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static std::string json_quote(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

static size_t find_json_value(const std::string& body, const std::string& key) {
    int depth = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '{' || c == '[') {
            ++depth;
            continue;
        }
        if (c == '}' || c == ']') {
            --depth;
            continue;
        }
        if (depth != 1 || c != '"') continue;

        std::string candidate;
        bool esc = false;
        size_t j = i + 1;
        for (; j < body.size(); ++j) {
            const char ch = body[j];
            if (esc) {
                candidate.push_back(ch);
                esc = false;
            } else if (ch == '\\') {
                esc = true;
            } else if (ch == '"') {
                break;
            } else {
                candidate.push_back(ch);
            }
        }
        if (j >= body.size()) return std::string::npos;

        size_t colon = j + 1;
        while (colon < body.size() &&
               std::isspace(static_cast<unsigned char>(body[colon]))) {
            ++colon;
        }
        if (colon >= body.size() || body[colon] != ':') {
            i = j;
            continue;
        }
        if (candidate != key) {
            i = j;
            continue;
        }
        size_t value = colon + 1;
        while (value < body.size() &&
               std::isspace(static_cast<unsigned char>(body[value]))) {
            ++value;
        }
        return value;
    }
    return std::string::npos;
}

static std::string read_json_string_at(const std::string& body, size_t pos) {
    if (pos == std::string::npos || pos >= body.size() || body[pos] != '"')
        return {};
    std::string out;
    bool esc = false;
    for (size_t i = pos + 1; i < body.size(); ++i) {
        const char c = body[i];
        if (esc) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return {};
}

static std::string json_string_field(const std::string& body,
                                     const std::string& key,
                                     const std::string& fallback = {}) {
    const size_t pos = find_json_value(body, key);
    const std::string value = read_json_string_at(body, pos);
    return value.empty() ? fallback : value;
}

static int64_t json_i64_field(const std::string& body,
                              const std::string& key,
                              int64_t fallback = 0) {
    const size_t pos = find_json_value(body, key);
    if (pos == std::string::npos) return fallback;
    char* end = nullptr;
    const long long value = std::strtoll(body.c_str() + pos, &end, 10);
    return end == body.c_str() + pos ? fallback : static_cast<int64_t>(value);
}

static bool json_nonnegative_size_field(const std::string& body,
                                        const std::string& key,
                                        size_t fallback,
                                        size_t* out,
                                        std::string* error) {
    const int64_t value = json_i64_field(body, key, static_cast<int64_t>(fallback));
    if (value < 0) {
        if (error) *error = key + " must be non-negative";
        return false;
    }
    *out = static_cast<size_t>(value);
    return true;
}

static double json_double_field(const std::string& body,
                                const std::string& key,
                                double fallback = 0.0) {
    const size_t pos = find_json_value(body, key);
    if (pos == std::string::npos) return fallback;
    char* end = nullptr;
    const double value = std::strtod(body.c_str() + pos, &end);
    return end == body.c_str() + pos ? fallback : value;
}

static bool json_bool_field(const std::string& body,
                            const std::string& key,
                            bool fallback = false) {
    const size_t pos = find_json_value(body, key);
    if (pos == std::string::npos) return fallback;
    if (body.compare(pos, 4, "true") == 0) return true;
    if (body.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

static std::string edge_fleet_connector_status_json(
    const zeptodb::feeds::EdgeFleetConnectorRuntimeSnapshot& snap) {
    std::ostringstream os;
    os << "{\"configured\":" << (snap.configured ? "true" : "false")
       << ",\"enabled\":" << (snap.enabled ? "true" : "false")
       << ",\"name\":" << json_quote(snap.name)
       << ",\"edge_outbox_table\":" << json_quote(snap.edge_outbox_table)
       << ",\"fleet_ack_table\":" << json_quote(snap.fleet_ack_table)
       << ",\"checkpoint_path\":" << json_quote(snap.checkpoint_path)
       << ",\"batch_limit\":" << snap.batch_limit
       << ",\"max_inflight\":" << snap.max_inflight
       << ",\"max_retries_per_event\":" << snap.max_retries_per_event
       << ",\"allow_late_events\":" << (snap.allow_late_events ? "true" : "false")
       << ",\"worker_enabled\":" << (snap.worker_enabled ? "true" : "false")
       << ",\"worker_hooks_configured\":"
       << (snap.worker_hooks_configured ? "true" : "false")
       << ",\"worker_running\":" << (snap.worker_running ? "true" : "false")
       << ",\"worker_poll_interval_ms\":" << snap.worker_poll_interval_ms
       << ",\"acked_count\":" << snap.acked_count
       << ",\"highest_acked_stream_seq\":" << snap.highest_acked_stream_seq
       << ",\"configure_total\":" << snap.configure_total
       << ",\"start_total\":" << snap.start_total
       << ",\"stop_total\":" << snap.stop_total
       << ",\"start_failures_total\":" << snap.start_failures_total
       << ",\"stop_failures_total\":" << snap.stop_failures_total
       << ",\"worker_start_total\":" << snap.worker_start_total
       << ",\"worker_passes_total\":" << snap.worker_passes_total
       << ",\"worker_load_errors_total\":" << snap.worker_load_errors_total
       << ",\"worker_observer_errors_total\":"
       << snap.worker_observer_errors_total
       << ",\"last_pass\":{\"outbox_events_seen\":"
       << snap.last_pass.outbox_events_seen
       << ",\"batch_event_count\":" << snap.last_pass.batch_event_count
       << ",\"attempted_count\":" << snap.last_pass.attempted_count
       << ",\"acked_count\":" << snap.last_pass.acked_count
       << ",\"transient_failure_count\":"
       << snap.last_pass.transient_failure_count
       << ",\"permanent_failure_count\":"
       << snap.last_pass.permanent_failure_count
       << ",\"duplicate_count\":" << snap.last_pass.duplicate_count
       << ",\"late_count\":" << snap.last_pass.late_count
       << ",\"rejected_count\":" << snap.last_pass.rejected_count
       << "}"
       << ",\"last_error\":" << json_quote(snap.last_error)
       << "}";
    return os.str();
}

static std::string action_outcome_supervisor_status_json(
    const zeptodb::feeds::ActionOutcomeSupervisorRuntimeSnapshot& snap) {
    std::ostringstream os;
    os << "{\"configured\":" << (snap.configured ? "true" : "false")
       << ",\"enabled\":" << (snap.enabled ? "true" : "false")
       << ",\"worker_running\":" << (snap.worker_running ? "true" : "false")
       << ",\"worker_hooks_configured\":"
       << (snap.worker_hooks_configured ? "true" : "false")
       << ",\"failure_budget_exhausted\":"
       << (snap.failure_budget_exhausted ? "true" : "false")
       << ",\"name\":" << json_quote(snap.name)
       << ",\"mode\":" << json_quote(snap.mode)
       << ",\"history_table\":" << json_quote(snap.history_table)
       << ",\"proposal_table\":" << json_quote(snap.proposal_table)
       << ",\"decision_table\":" << json_quote(snap.decision_table)
       << ",\"evidence_table\":" << json_quote(snap.evidence_table)
       << ",\"fail_closed_action\":" << json_quote(snap.fail_closed_action)
       << ",\"worker_poll_interval_ms\":" << snap.worker_poll_interval_ms
       << ",\"batch_limit\":" << snap.batch_limit
       << ",\"max_consecutive_failures\":"
       << snap.max_consecutive_failures
       << ",\"consecutive_failures\":" << snap.consecutive_failures
       << ",\"configure_total\":" << snap.configure_total
       << ",\"start_total\":" << snap.start_total
       << ",\"stop_total\":" << snap.stop_total
       << ",\"start_failures_total\":" << snap.start_failures_total
       << ",\"stop_failures_total\":" << snap.stop_failures_total
       << ",\"worker_start_total\":" << snap.worker_start_total
       << ",\"worker_wakeups_total\":" << snap.worker_wakeups_total
       << ",\"worker_passes_total\":" << snap.worker_passes_total
       << ",\"worker_idle_passes_total\":" << snap.worker_idle_passes_total
       << ",\"worker_failures_total\":" << snap.worker_failures_total
       << ",\"proposals_processed_total\":"
       << snap.proposals_processed_total
       << ",\"proposals_duplicate_total\":"
       << snap.proposals_duplicate_total
       << ",\"proposals_rejected_total\":"
       << snap.proposals_rejected_total
       << ",\"decisions_allow_total\":" << snap.decisions_allow_total
       << ",\"decisions_suppress_total\":"
       << snap.decisions_suppress_total
       << ",\"fail_closed_total\":" << snap.fail_closed_total
       << ",\"evidence_rows_written_total\":"
       << snap.evidence_rows_written_total
       << ",\"last_pass\":{\"proposals_seen\":"
       << snap.last_pass.proposals_seen
       << ",\"batch_proposals\":" << snap.last_pass.batch_proposals
       << ",\"processed_count\":" << snap.last_pass.processed_count
       << ",\"duplicate_count\":" << snap.last_pass.duplicate_count
       << ",\"rejected_count\":" << snap.last_pass.rejected_count
       << ",\"decision_error_count\":"
       << snap.last_pass.decision_error_count
       << ",\"sink_error_count\":" << snap.last_pass.sink_error_count
       << ",\"allow_count\":" << snap.last_pass.allow_count
       << ",\"suppress_count\":" << snap.last_pass.suppress_count
       << ",\"fail_closed_count\":" << snap.last_pass.fail_closed_count
       << ",\"evidence_rows_written\":"
       << snap.last_pass.evidence_rows_written
       << ",\"latency_us\":" << snap.last_pass.latency_us
       << "}"
       << ",\"last_error\":" << json_quote(snap.last_error)
       << "}";
    return os.str();
}

static bool json_float_array_field(const std::string& body,
                                   const std::string& key,
                                   std::vector<float>* out,
                                   std::string* error) {
    out->clear();
    const size_t pos = find_json_value(body, key);
    if (pos == std::string::npos) return true;
    if (pos >= body.size() || body[pos] != '[') {
        if (error) *error = key + " must be an array";
        return false;
    }
    size_t cur = pos + 1;
    while (cur < body.size()) {
        while (cur < body.size() &&
               std::isspace(static_cast<unsigned char>(body[cur]))) ++cur;
        if (cur < body.size() && body[cur] == ']') return true;
        char* end = nullptr;
        const double value = std::strtod(body.c_str() + cur, &end);
        if (end == body.c_str() + cur) {
            if (error) *error = "Invalid float in " + key;
            return false;
        }
        out->push_back(static_cast<float>(value));
        cur = static_cast<size_t>(end - body.c_str());
        while (cur < body.size() &&
               std::isspace(static_cast<unsigned char>(body[cur]))) ++cur;
        if (cur < body.size() && body[cur] == ',') {
            ++cur;
            continue;
        }
        if (cur < body.size() && body[cur] == ']') return true;
        if (error) *error = "Invalid array syntax in " + key;
        return false;
    }
    if (error) *error = "Unterminated array in " + key;
    return false;
}

static std::string memory_json(const zeptodb::ai::MemorySearchResult& match) {
    const auto& r = match.record;
    std::ostringstream os;
    os << "{\"memory_id\":" << json_quote(r.memory_id)
       << ",\"tenant_id\":" << json_quote(r.tenant_id)
       << ",\"namespace\":" << json_quote(r.namespace_id)
       << ",\"user_id\":" << json_quote(r.user_id)
       << ",\"session_id\":" << json_quote(r.session_id)
       << ",\"agent_id\":" << json_quote(r.agent_id)
       << ",\"type\":" << json_quote(r.type)
       << ",\"content\":" << json_quote(r.content)
       << ",\"metadata_json\":" << json_quote(r.metadata_json)
       << ",\"token_count\":" << r.token_count
       << ",\"importance\":" << r.importance
       << ",\"created_at_ns\":" << r.created_at_ns
       << ",\"last_accessed_ns\":" << r.last_accessed_ns
       << ",\"expires_at_ns\":" << r.expires_at_ns
       << ",\"pinned\":" << (r.pinned ? "true" : "false")
       << ",\"access_count\":" << r.access_count
       << ",\"score\":" << match.score
       << ",\"similarity\":" << match.similarity
       << "}";
    return os.str();
}

static bool memory_match_better(const zeptodb::ai::MemorySearchResult& a,
                                const zeptodb::ai::MemorySearchResult& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.record.created_at_ns > b.record.created_at_ns;
}

static void trim_memory_matches(std::vector<zeptodb::ai::MemorySearchResult>* matches,
                                size_t limit) {
    std::sort(matches->begin(), matches->end(), memory_match_better);
    if (matches->size() > limit) {
        matches->resize(limit);
    }
}

static zeptodb::ai::ContextResult assemble_context_from_matches(
    std::vector<zeptodb::ai::MemorySearchResult> matches,
    int64_t token_budget) {
    if (token_budget < 0) return {};
    zeptodb::ai::ContextResult out;
    std::unordered_set<std::string> seen_content;
    for (auto& match : matches) {
        if (!match.record.content.empty() &&
            !seen_content.insert(match.record.content).second) {
            continue;
        }
        const int64_t tokens = match.record.token_count > 0
            ? match.record.token_count
            : zeptodb::ai::AgentMemoryStore::estimate_tokens(match.record.content);
        if (token_budget > 0 && tokens > token_budget - out.token_count) {
            continue;
        }
        out.token_count += tokens;
        out.memories.push_back(std::move(match));
    }
    return out;
}

static std::string cache_entry_json(const zeptodb::ai::CacheEntry& entry) {
    std::ostringstream os;
    os << "{\"cache_id\":" << json_quote(entry.cache_id)
       << ",\"tenant_id\":" << json_quote(entry.tenant_id)
       << ",\"namespace\":" << json_quote(entry.namespace_id)
       << ",\"prompt\":" << json_quote(entry.prompt)
       << ",\"response\":" << json_quote(entry.response)
       << ",\"metadata_json\":" << json_quote(entry.metadata_json)
       << ",\"token_count\":" << entry.token_count
       << ",\"created_at_ns\":" << entry.created_at_ns
       << ",\"last_accessed_ns\":" << entry.last_accessed_ns
       << ",\"expires_at_ns\":" << entry.expires_at_ns
       << ",\"access_count\":" << entry.access_count
       << "}";
    return os.str();
}

static bool parse_owner_scoped_memory_id(
    const std::string& memory_id,
    zeptodb::ai::AgentMemoryNodeId* node_id) {
    constexpr std::string_view prefix = "mem_";
    if (!node_id || memory_id.rfind(prefix, 0) != 0) return false;
    const size_t start = prefix.size();
    const size_t end = memory_id.find('_', start);
    if (end == std::string::npos || end == start) return false;
    uint64_t parsed = 0;
    for (size_t i = start; i < end; ++i) {
        const char c = memory_id[i];
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
        if (parsed > std::numeric_limits<zeptodb::ai::AgentMemoryNodeId>::max()) {
            return false;
        }
    }
    *node_id = static_cast<zeptodb::ai::AgentMemoryNodeId>(parsed);
    return true;
}

static const char* agent_memory_ann_mode_name(
    zeptodb::ai::AgentMemoryAnnMode mode) {
    switch (mode) {
        case zeptodb::ai::AgentMemoryAnnMode::Auto:
            return "auto";
        case zeptodb::ai::AgentMemoryAnnMode::SparseProjection:
            return "sparse_projection";
        case zeptodb::ai::AgentMemoryAnnMode::Hnsw:
            return "hnsw";
        case zeptodb::ai::AgentMemoryAnnMode::Ivf:
            return "ivf";
        case zeptodb::ai::AgentMemoryAnnMode::Off:
        default:
            return "off";
    }
}

static void append_agent_memory_stats_object(
    std::ostringstream& os,
    const zeptodb::ai::AgentMemoryStats& stats) {
    os << "{\"memory_count\":" << stats.memory_count
       << ",\"cache_count\":" << stats.cache_count
       << ",\"embedding_dim\":" << stats.embedding_dim
       << ",\"evicted_memory_count\":" << stats.evicted_memory_count
       << ",\"evicted_cache_count\":" << stats.evicted_cache_count
       << ",\"snapshot_records_bytes\":" << stats.snapshot_records_bytes
       << ",\"snapshot_vectors_bytes\":" << stats.snapshot_vectors_bytes
       << ",\"snapshot_total_bytes\":" << stats.snapshot_total_bytes
       << ",\"snapshot_latency_seconds\":" << stats.snapshot_latency_seconds
       << ",\"snapshot_failures_total\":" << stats.snapshot_failures_total
       << ",\"tenant_quota_count\":" << stats.tenant_quota_count
       << ",\"ann\":{"
       << "\"enabled\":" << (stats.ann_enabled ? "true" : "false")
       << ",\"indexed_vectors\":" << stats.ann_indexed_vectors
       << ",\"partitions\":" << stats.ann_partitions
       << ",\"buckets\":" << stats.ann_buckets
       << ",\"max_bucket_size\":" << stats.ann_max_bucket_size
       << ",\"memory_bytes\":" << stats.ann_memory_bytes
       << ",\"tombstone_entries\":" << stats.ann_tombstone_entries
       << ",\"rebuild_count\":" << stats.ann_rebuild_count
       << ",\"last_rebuild_ms\":" << stats.ann_last_rebuild_ms
       << ",\"search_count\":" << stats.ann_search_count
       << ",\"fallback_count\":" << stats.ann_fallback_count
       << "}}";
}

static zeptodb::ai::AgentMemoryStats aggregate_agent_memory_stats(
    const std::vector<zeptodb::ai::AgentMemoryStats>& stats) {
    zeptodb::ai::AgentMemoryStats aggregate;
    for (const auto& node : stats) {
        aggregate.memory_count += node.memory_count;
        aggregate.cache_count += node.cache_count;
        aggregate.embedding_dim = std::max(aggregate.embedding_dim,
                                           node.embedding_dim);
        aggregate.evicted_memory_count += node.evicted_memory_count;
        aggregate.evicted_cache_count += node.evicted_cache_count;
        aggregate.ann_enabled = aggregate.ann_enabled || node.ann_enabled;
        aggregate.ann_indexed_vectors += node.ann_indexed_vectors;
        aggregate.ann_partitions += node.ann_partitions;
        aggregate.ann_buckets += node.ann_buckets;
        aggregate.ann_max_bucket_size =
            std::max(aggregate.ann_max_bucket_size, node.ann_max_bucket_size);
        aggregate.ann_memory_bytes += node.ann_memory_bytes;
        aggregate.ann_tombstone_entries += node.ann_tombstone_entries;
        aggregate.ann_rebuild_count += node.ann_rebuild_count;
        aggregate.ann_last_rebuild_ms =
            std::max(aggregate.ann_last_rebuild_ms, node.ann_last_rebuild_ms);
        aggregate.ann_search_count += node.ann_search_count;
        aggregate.ann_fallback_count += node.ann_fallback_count;
        aggregate.snapshot_latency_seconds =
            std::max(aggregate.snapshot_latency_seconds,
                     node.snapshot_latency_seconds);
        aggregate.snapshot_failures_total += node.snapshot_failures_total;
        aggregate.snapshot_records_bytes += node.snapshot_records_bytes;
        aggregate.snapshot_vectors_bytes += node.snapshot_vectors_bytes;
        aggregate.snapshot_total_bytes += node.snapshot_total_bytes;
        aggregate.tenant_quota_count += node.tenant_quota_count;
    }
    return aggregate;
}

struct AgentMemoryClusterStatsNode {
    zeptodb::ai::AgentMemoryNodeId node_id = 0;
    bool local = false;
    zeptodb::ai::AgentMemoryStats stats;
    std::string error;
};

static std::string agent_memory_cluster_stats_json(
    const std::vector<AgentMemoryClusterStatsNode>& nodes) {
    std::vector<zeptodb::ai::AgentMemoryStats> successful;
    successful.reserve(nodes.size());
    size_t partial_failures = 0;
    for (const auto& node : nodes) {
        if (node.error.empty()) {
            successful.push_back(node.stats);
        } else {
            ++partial_failures;
        }
    }
    const auto aggregate = aggregate_agent_memory_stats(successful);
    std::ostringstream os;
    os << "{\"scope\":\"cluster\""
       << ",\"partial_failures\":" << partial_failures
       << ",\"aggregate\":";
    append_agent_memory_stats_object(os, aggregate);
    os << ",\"nodes\":[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (i > 0) os << ",";
        os << "{\"node_id\":" << node.node_id
           << ",\"local\":" << (node.local ? "true" : "false");
        if (node.error.empty()) {
            os << ",\"stats\":";
            append_agent_memory_stats_object(os, node.stats);
        } else {
            os << ",\"error\":" << json_quote(node.error);
        }
        os << "}";
    }
    os << "]}";
    return os.str();
}

static void append_agent_memory_failover_status_object(
    std::ostringstream& os,
    const AgentMemoryOwnerFailoverResult& status) {
    os << "{\"source_node_id\":" << status.source_node_id
       << ",\"replacement_node_id\":" << status.replacement_node_id
       << ",\"source_ring_epoch\":" << status.source_ring_epoch
       << ",\"new_ring_epoch\":" << status.new_ring_epoch
       << ",\"ok\":" << (status.ok ? "true" : "false")
       << ",\"adopted\":" << (status.adopted ? "true" : "false")
       << ",\"replica_promoted\":"
       << (status.replica_promoted ? "true" : "false")
       << ",\"degraded\":" << (status.degraded ? "true" : "false")
       << ",\"replay_source_missing\":"
       << (status.replay_source_missing ? "true" : "false")
       << ",\"degraded_reason\":" << json_quote(status.degraded_reason)
       << ",\"error\":" << json_quote(status.error)
       << "}";
}

static std::string agent_memory_stats_json(
    const zeptodb::ai::AgentMemoryStore& store,
    const AgentMemoryOwnerFailoverResult& failover_status) {
    const auto stats = store.stats();
    const auto eviction = store.eviction_config();
    const auto ann = store.ann_config();
    std::ostringstream os;
    os << "{\"memory_count\":" << stats.memory_count
       << ",\"cache_count\":" << stats.cache_count
       << ",\"embedding_dim\":" << stats.embedding_dim
       << ",\"evicted_memory_count\":" << stats.evicted_memory_count
       << ",\"evicted_cache_count\":" << stats.evicted_cache_count
       << ",\"snapshot_records_bytes\":" << stats.snapshot_records_bytes
       << ",\"snapshot_vectors_bytes\":" << stats.snapshot_vectors_bytes
       << ",\"snapshot_total_bytes\":" << stats.snapshot_total_bytes
       << ",\"snapshot_latency_seconds\":" << stats.snapshot_latency_seconds
       << ",\"snapshot_failures_total\":" << stats.snapshot_failures_total
       << ",\"tenant_quota_count\":" << stats.tenant_quota_count
       << ",\"ann\":{"
       << "\"mode\":" << json_quote(agent_memory_ann_mode_name(ann.mode))
       << ",\"enabled\":" << (stats.ann_enabled ? "true" : "false")
       << ",\"min_records\":" << ann.min_records
       << ",\"oversample\":" << ann.oversample
       << ",\"indexed_vectors\":" << stats.ann_indexed_vectors
       << ",\"partitions\":" << stats.ann_partitions
       << ",\"buckets\":" << stats.ann_buckets
       << ",\"max_bucket_size\":" << stats.ann_max_bucket_size
       << ",\"memory_bytes\":" << stats.ann_memory_bytes
       << ",\"tombstone_entries\":" << stats.ann_tombstone_entries
       << ",\"rebuild_count\":" << stats.ann_rebuild_count
       << ",\"last_rebuild_ms\":" << stats.ann_last_rebuild_ms
       << ",\"search_count\":" << stats.ann_search_count
       << ",\"fallback_count\":" << stats.ann_fallback_count
       << "}"
       << ",\"eviction_config\":{"
       << "\"max_memories\":" << eviction.max_memories
       << ",\"max_cache_entries\":" << eviction.max_cache_entries
       << ",\"tenant_quota_count\":" << stats.tenant_quota_count
       << ",\"evict_expired_on_write\":"
       << (eviction.evict_expired_on_write ? "true" : "false")
       << ",\"protect_pinned\":" << (eviction.protect_pinned ? "true" : "false")
       << "},\"failover\":";
    append_agent_memory_failover_status_object(os, failover_status);
    os << "}";
    return os.str();
}

static bool apply_tenant_header(const httplib::Request& req,
                                std::string* tenant_id,
                                std::string* error) {
    if (!req.has_header("X-Zepto-Tenant-Id")) return true;
    const std::string header_tenant = req.get_header_value("X-Zepto-Tenant-Id");
    if (header_tenant.empty()) return true;
    if (!tenant_id->empty() && *tenant_id != header_tenant) {
        if (error) *error = "Tenant header does not match request tenant_id";
        return false;
    }
    *tenant_id = header_tenant;
    return true;
}

static std::filesystem::path agent_memory_effective_snapshot_dir(
    const std::string& base_dir,
    bool routed,
    zeptodb::ai::AgentMemoryNodeId node_id,
    uint32_t shard_id) {
    std::filesystem::path path(base_dir);
    if (!routed) return path;
    path /= "node-" + std::to_string(node_id);
    path /= "shard-" + std::to_string(shard_id);
    return path;
}

static bool read_agent_memory_manifest_u64(const std::string& text,
                                           const std::string& key,
                                           uint64_t* value) {
    const std::string quoted_key = "\"" + key + "\"";
    const size_t key_pos = text.find(quoted_key);
    if (key_pos == std::string::npos) return false;
    const size_t colon = text.find(':', key_pos + quoted_key.size());
    if (colon == std::string::npos) return false;
    size_t pos = colon + 1;
    while (pos < text.size() &&
           std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str() + pos, &end, 10);
    if (end == text.c_str() + pos) return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

static bool validate_agent_memory_shard_manifest(
    const std::filesystem::path& dir,
    bool routed,
    zeptodb::ai::AgentMemoryNodeId node_id,
    uint64_t ring_epoch,
    uint32_t shard_id,
    bool allow_epoch_forward,
    std::string* error) {
    namespace fs = std::filesystem;
    if (!routed) return true;

    const fs::path manifest_path = dir / "manifest.json";
    const bool has_records = fs::exists(dir / "records.bin");
    const bool has_vectors = fs::exists(dir / "vectors.bin");
    const bool has_wal = fs::exists(dir / "wal.log");
    if (!fs::exists(manifest_path)) {
        if (!has_records && !has_vectors && !has_wal) return true;
        if (error) *error = "missing agent memory shard manifest";
        return false;
    }

    std::ifstream manifest(manifest_path);
    if (!manifest.is_open()) {
        if (error) *error = "failed to open agent memory shard manifest";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(manifest)),
                           std::istreambuf_iterator<char>());

    uint64_t version = 0;
    uint64_t manifest_node_id = 0;
    uint64_t manifest_ring_epoch = 0;
    uint64_t manifest_shard_id = 0;
    if (!read_agent_memory_manifest_u64(text, "version", &version) ||
        !read_agent_memory_manifest_u64(text, "node_id", &manifest_node_id) ||
        !read_agent_memory_manifest_u64(text, "ring_epoch", &manifest_ring_epoch) ||
        !read_agent_memory_manifest_u64(text, "shard_id", &manifest_shard_id)) {
        if (error) *error = "invalid agent memory shard manifest";
        return false;
    }
    if (version != 1) {
        if (error) *error = "unsupported agent memory shard manifest version";
        return false;
    }
    const bool epoch_matches = manifest_ring_epoch == ring_epoch ||
        (allow_epoch_forward && manifest_ring_epoch <= ring_epoch);
    if (manifest_node_id != node_id || manifest_shard_id != shard_id ||
        !epoch_matches) {
        if (error) {
            *error = "agent memory shard manifest does not match current owner";
        }
        return false;
    }
    return true;
}

static bool write_agent_memory_shard_manifest(
    const std::filesystem::path& dir,
    bool routed,
    zeptodb::ai::AgentMemoryNodeId node_id,
    uint64_t ring_epoch,
    uint32_t shard_id,
    std::string* error) {
    namespace fs = std::filesystem;
    if (!routed) return true;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "failed to create shard manifest directory: " + ec.message();
        return false;
    }

    const fs::path tmp_path = dir / "manifest.json.tmp";
    const fs::path manifest_path = dir / "manifest.json";
    std::ofstream manifest(tmp_path, std::ios::trunc);
    if (!manifest.is_open()) {
        if (error) *error = "failed to open shard manifest for writing";
        return false;
    }
    manifest << "{\n"
             << "  \"format\": \"zeptodb.agent_memory.shard_snapshot\",\n"
             << "  \"version\": 1,\n"
             << "  \"node_id\": " << node_id << ",\n"
             << "  \"shard_id\": " << shard_id << ",\n"
             << "  \"ring_epoch\": " << ring_epoch << "\n"
             << "}\n";
    manifest.close();
    if (!manifest) {
        if (error) *error = "failed to flush shard manifest";
        return false;
    }

    fs::remove(manifest_path, ec);
    ec.clear();
    fs::rename(tmp_path, manifest_path, ec);
    if (ec) {
        if (error) *error = "failed to publish shard manifest: " + ec.message();
        return false;
    }
    return true;
}

constexpr uint32_t kAgentMemoryWalMagic = 0x4c574d41u; // "AMWL", little-endian
constexpr uint32_t kAgentMemoryWalVersion = 1;
constexpr uint32_t kAgentMemoryWalPutMemory = 1;
constexpr uint32_t kAgentMemoryWalStoreCache = 2;
constexpr uint32_t kAgentMemoryWalPrepareMemory = 3;
constexpr uint32_t kAgentMemoryWalPrepareCache = 4;
constexpr uint32_t kAgentMemoryWalCommit = 5;
constexpr uint32_t kAgentMemoryWalDeleteMemory = 6;
constexpr uint32_t kAgentMemoryWalDeleteCache = 7;
constexpr uint32_t kAgentMemoryWalPrepareDeleteMemory = 8;
constexpr uint32_t kAgentMemoryWalPrepareDeleteCache = 9;
constexpr uint32_t kAgentMemoryWalMaxPayload = 64u * 1024u * 1024u;

static void append_u32_le(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8));
    out->push_back(static_cast<uint8_t>(value >> 16));
    out->push_back(static_cast<uint8_t>(value >> 24));
}

static void append_u64_le(std::vector<uint8_t>* out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out->push_back(static_cast<uint8_t>(value >> shift));
    }
}

static bool read_u32_le(std::istream& in, uint32_t* value) {
    uint8_t bytes[4] = {};
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!in) return false;
    *value = static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}

static bool read_u32_le_bytes(const uint8_t*& ptr, size_t& remaining,
                              uint32_t* value) {
    if (remaining < 4) return false;
    *value = static_cast<uint32_t>(ptr[0])
        | (static_cast<uint32_t>(ptr[1]) << 8)
        | (static_cast<uint32_t>(ptr[2]) << 16)
        | (static_cast<uint32_t>(ptr[3]) << 24);
    ptr += 4;
    remaining -= 4;
    return true;
}

static bool read_u64_le_bytes(const uint8_t*& ptr, size_t& remaining,
                              uint64_t* value) {
    if (remaining < 8) return false;
    uint64_t out = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        out |= static_cast<uint64_t>(*ptr++) << shift;
    }
    remaining -= 8;
    *value = out;
    return true;
}

static void append_string_le(std::vector<uint8_t>* out,
                             const std::string& value) {
    append_u32_le(out, static_cast<uint32_t>(value.size()));
    out->insert(out->end(), value.begin(), value.end());
}

static bool read_string_le_bytes(const uint8_t*& ptr,
                                 size_t& remaining,
                                 std::string* value) {
    uint32_t len = 0;
    if (!read_u32_le_bytes(ptr, remaining, &len) || remaining < len) {
        return false;
    }
    value->assign(reinterpret_cast<const char*>(ptr), len);
    ptr += len;
    remaining -= len;
    return true;
}

static bool replay_agent_memory_wal_into_store(
    const std::string& directory,
    zeptodb::ai::AgentMemoryStore& store,
    std::string* error);

static std::vector<uint8_t> serialize_agent_memory_wal_prepare(
    uint64_t tx_id,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(8 + payload.size());
    append_u64_le(&out, tx_id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

static std::vector<uint8_t> serialize_agent_memory_wal_tx_id(uint64_t tx_id) {
    std::vector<uint8_t> out;
    out.reserve(8);
    append_u64_le(&out, tx_id);
    return out;
}

static std::vector<uint8_t> serialize_agent_memory_wal_delete_memory(
    const std::string& memory_id,
    const std::string& tenant_id) {
    std::vector<uint8_t> out;
    out.reserve(8 + memory_id.size() + tenant_id.size());
    append_string_le(&out, memory_id);
    append_string_le(&out, tenant_id);
    return out;
}

static std::vector<uint8_t> serialize_agent_memory_wal_delete_cache(
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt) {
    std::vector<uint8_t> out;
    out.reserve(12 + tenant_id.size() + namespace_id.size() + prompt.size());
    append_string_le(&out, tenant_id);
    append_string_le(&out, namespace_id);
    append_string_le(&out, prompt);
    return out;
}

struct AgentMemoryWalPreparedMutation {
    uint32_t committed_type = 0;
    std::vector<uint8_t> payload;
};

static bool deserialize_agent_memory_wal_prepare(
    uint32_t type,
    const std::vector<uint8_t>& payload,
    uint64_t* tx_id,
    AgentMemoryWalPreparedMutation* mutation,
    std::string* error) {
    const uint8_t* ptr = payload.data();
    size_t remaining = payload.size();
    uint64_t parsed_tx_id = 0;
    if (!read_u64_le_bytes(ptr, remaining, &parsed_tx_id)) {
        if (error) *error = "invalid agent memory WAL prepare payload";
        return false;
    }
    AgentMemoryWalPreparedMutation parsed;
    if (type == kAgentMemoryWalPrepareMemory) {
        parsed.committed_type = kAgentMemoryWalPutMemory;
    } else if (type == kAgentMemoryWalPrepareCache) {
        parsed.committed_type = kAgentMemoryWalStoreCache;
    } else if (type == kAgentMemoryWalPrepareDeleteMemory) {
        parsed.committed_type = kAgentMemoryWalDeleteMemory;
    } else if (type == kAgentMemoryWalPrepareDeleteCache) {
        parsed.committed_type = kAgentMemoryWalDeleteCache;
    } else {
        if (error) *error = "invalid agent memory WAL prepare type";
        return false;
    }
    parsed.payload.assign(ptr, ptr + remaining);
    *tx_id = parsed_tx_id;
    *mutation = std::move(parsed);
    return true;
}

static bool deserialize_agent_memory_wal_tx_id(
    const std::vector<uint8_t>& payload,
    uint64_t* tx_id,
    std::string* error) {
    const uint8_t* ptr = payload.data();
    size_t remaining = payload.size();
    if (!read_u64_le_bytes(ptr, remaining, tx_id) || remaining != 0) {
        if (error) *error = "invalid agent memory WAL transaction payload";
        return false;
    }
    return true;
}

static bool apply_agent_memory_wal_mutation(
    uint32_t type,
    const std::vector<uint8_t>& payload,
    zeptodb::ai::AgentMemoryStore& store,
    std::string* error) {
    if (type == kAgentMemoryWalPutMemory) {
        zeptodb::ai::MemoryRecord record;
        if (!zeptodb::ai::deserialize_memory_record(
                payload.data(), payload.size(), &record, error)) {
            return false;
        }
        auto stored = store.put_memory(std::move(record));
        if (!stored.ok) {
            if (error) *error = stored.error;
            return false;
        }
        return true;
    }
    if (type == kAgentMemoryWalStoreCache) {
        zeptodb::ai::CacheEntry entry;
        if (!zeptodb::ai::deserialize_cache_entry(
                payload.data(), payload.size(), &entry, error)) {
            return false;
        }
        auto stored = store.store_cache(std::move(entry));
        if (!stored.ok) {
            if (error) *error = stored.error;
            return false;
        }
        return true;
    }
    if (type == kAgentMemoryWalDeleteMemory) {
        const uint8_t* ptr = payload.data();
        size_t remaining = payload.size();
        std::string memory_id;
        std::string tenant_id;
        if (!read_string_le_bytes(ptr, remaining, &memory_id) ||
            !read_string_le_bytes(ptr, remaining, &tenant_id) ||
            remaining != 0) {
            if (error) *error = "invalid agent memory WAL memory delete payload";
            return false;
        }
        (void)store.remove_memory(memory_id, tenant_id);
        return true;
    }
    if (type == kAgentMemoryWalDeleteCache) {
        const uint8_t* ptr = payload.data();
        size_t remaining = payload.size();
        std::string tenant_id;
        std::string namespace_id;
        std::string prompt;
        if (!read_string_le_bytes(ptr, remaining, &tenant_id) ||
            !read_string_le_bytes(ptr, remaining, &namespace_id) ||
            !read_string_le_bytes(ptr, remaining, &prompt) ||
            remaining != 0) {
            if (error) *error = "invalid agent memory WAL cache delete payload";
            return false;
        }
        (void)store.remove_cache(tenant_id, namespace_id, prompt);
        return true;
    }
    if (error) *error = "unknown agent memory WAL record type";
    return false;
}

// ============================================================================
// Constructors
// ============================================================================
HttpServer::HttpServer(zeptodb::sql::QueryExecutor& executor, uint16_t port)
    : executor_(executor)
    , port_(port)
    , tls_{}
    , auth_(nullptr)
    , svr_(std::make_unique<httplib::Server>())
    , agent_memory_(std::make_unique<zeptodb::ai::AgentMemoryStore>())
    , agent_memory_wal_tx_counter_(
          static_cast<uint64_t>(zeptodb::ai::AgentMemoryStore::now_ns()))
{
    setup_routes();
    setup_auth_middleware();
    setup_admin_routes();
    setup_session_tracking();
}

HttpServer::HttpServer(zeptodb::sql::QueryExecutor& executor,
                       uint16_t port,
                       zeptodb::auth::TlsConfig tls,
                       std::shared_ptr<zeptodb::auth::AuthManager> auth)
    : executor_(executor)
    , port_(port)
    , tls_(std::move(tls))
    , auth_(std::move(auth))
    , agent_memory_(std::make_unique<zeptodb::ai::AgentMemoryStore>())
    , agent_memory_wal_tx_counter_(
          static_cast<uint64_t>(zeptodb::ai::AgentMemoryStore::now_ns()))
{
#ifdef ZEPTO_TLS_ENABLED
    if (tls_.enabled) {
        svr_ = std::make_unique<httplib::SSLServer>(
            tls_.cert_path.c_str(), tls_.key_path.c_str());
        port_ = tls_.https_port;
    } else {
        svr_ = std::make_unique<httplib::Server>();
    }
#else
    if (tls_.enabled) {
        // TLS requested but not compiled in — fall back to HTTP with a warning
        fprintf(stderr, "[ZeptoDB] WARNING: TLS requested but not compiled in "
                        "(rebuild with ZEPTO_TLS_ENABLED). Falling back to HTTP.\n");
    }
    svr_ = std::make_unique<httplib::Server>();
#endif
    setup_routes();
    setup_auth_middleware();
    setup_admin_routes();
    setup_session_tracking();
}

HttpServer::~HttpServer() {
    if (running_.load()) stop();
}

zeptodb::ai::AgentMemoryStore& HttpServer::agent_memory_store() {
    return *agent_memory_;
}

bool HttpServer::set_edge_fleet_connector_runtime_hooks(
    zeptodb::feeds::EdgeFleetConnectorRuntimeHooks hooks,
    std::string* error) {
    return edge_fleet_connector_runtime_.setWorkerHooks(std::move(hooks), error);
}

bool HttpServer::set_action_outcome_supervisor_runtime_hooks(
    zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks hooks,
    std::string* error) {
    return action_outcome_supervisor_runtime_.setWorkerHooks(std::move(hooks), error);
}

bool HttpServer::set_agent_memory_persistence(const std::string& directory,
                                              std::string* error,
                                              size_t flush_every_mutations) {
    if (directory.empty()) {
        if (error) *error = "agent memory persistence directory is required";
        return false;
    }

    bool routed = false;
    zeptodb::ai::AgentMemoryNodeId node_id = 0;
    uint64_t ring_epoch = 0;
    uint32_t shard_id = 0;
    {
        std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
        routed = agent_memory_persist_routed_;
        node_id = agent_memory_persist_node_id_;
        ring_epoch = agent_memory_persist_ring_epoch_;
        shard_id = agent_memory_persist_shard_id_;
    }

    const auto effective_dir = agent_memory_effective_snapshot_dir(
        directory, routed, node_id, shard_id);
    if (!validate_agent_memory_shard_manifest(
            effective_dir, routed, node_id, ring_epoch, shard_id,
            true, error)) {
        return false;
    }
    auto loaded = agent_memory_->load_from_directory(effective_dir.string());
    if (!loaded.ok) {
        if (error) *error = loaded.error;
        return false;
    }
    if (!replay_agent_memory_wal_(effective_dir.string(), error)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
    agent_memory_persist_base_dir_ = directory;
    agent_memory_persist_dir_ = effective_dir.string();
    agent_memory_flush_every_mutations_ = flush_every_mutations;
    agent_memory_dirty_mutations_ = 0;
    return true;
}

bool HttpServer::set_agent_memory_routing(
    zeptodb::ai::AgentMemoryRouterConfig config,
    const std::vector<zeptodb::ai::AgentMemoryNodeId>& nodes,
    std::unordered_map<zeptodb::ai::AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes,
    std::string* error) {
    if (config.virtual_nodes_per_node == 0) {
        config.virtual_nodes_per_node = 1;
    }
    auto router = std::make_unique<zeptodb::ai::AgentMemoryRouter>(config);
    for (const auto node_id : nodes) {
        router->add_node(node_id);
    }
    for (auto& [node_id, client] : remotes) {
        (void)node_id;
        if (client) client->set_epoch(config.ring_epoch);
    }

    const bool routed = config.mode == zeptodb::ai::AgentMemoryRoutingMode::Routed;
    constexpr uint32_t shard_id = 0;
    std::string base_dir;
    {
        std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
        base_dir = agent_memory_persist_base_dir_;
    }
    const auto effective_dir = agent_memory_effective_snapshot_dir(
        base_dir, routed, config.self_node_id, shard_id);
    if (!base_dir.empty()) {
        if (!validate_agent_memory_shard_manifest(
                effective_dir, routed, config.self_node_id, config.ring_epoch,
                shard_id, true, error)) {
            return false;
        }
        auto loaded = agent_memory_->load_from_directory(effective_dir.string());
        if (!loaded.ok) {
            if (error) *error = loaded.error;
            return false;
        }
        if (!replay_agent_memory_wal_(effective_dir.string(), error)) {
            return false;
        }
    }

    zeptodb::ai::AgentMemoryIdConfig ids;
    ids.owner_scoped = routed;
    ids.node_id = config.self_node_id;
    ids.ring_epoch = config.ring_epoch;
    agent_memory_->set_id_config(ids);

    std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
    agent_memory_router_ = std::move(router);
    agent_memory_remotes_ = std::move(remotes);
    {
        std::lock_guard<std::mutex> persist_lock(agent_memory_persist_mu_);
        agent_memory_persist_routed_ = routed;
        agent_memory_persist_node_id_ = config.self_node_id;
        agent_memory_persist_ring_epoch_ = config.ring_epoch;
        agent_memory_persist_shard_id_ = shard_id;
        if (!agent_memory_persist_base_dir_.empty()) {
            agent_memory_persist_dir_ = effective_dir.string();
        }
        agent_memory_dirty_mutations_ = 0;
    }
    return true;
}

void HttpServer::record_agent_memory_failover_status_(
    const AgentMemoryOwnerFailoverResult& result) {
    std::lock_guard<std::mutex> lock(agent_memory_failover_mu_);
    agent_memory_last_failover_ = result;
}

AgentMemoryOwnerFailoverResult HttpServer::agent_memory_failover_status_() const {
    std::lock_guard<std::mutex> lock(agent_memory_failover_mu_);
    return agent_memory_last_failover_;
}

std::string HttpServer::build_agent_memory_stats_json(bool cluster_scope) const {
    if (!cluster_scope) {
        return agent_memory_stats_json(*agent_memory_,
                                       agent_memory_failover_status_());
    }

    std::vector<AgentMemoryClusterStatsNode> nodes;
    nodes.push_back({0, true, agent_memory_->stats(), {}});

    std::vector<std::pair<zeptodb::ai::AgentMemoryNodeId,
                          std::shared_ptr<zeptodb::cluster::TcpRpcClient>>>
        remote_targets;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_ &&
            agent_memory_router_->config().mode ==
                zeptodb::ai::AgentMemoryRoutingMode::Routed) {
            const auto config = agent_memory_router_->config();
            nodes.front().node_id = config.self_node_id;
            for (const auto node_id : agent_memory_router_->nodes()) {
                if (node_id == config.self_node_id) continue;
                auto it = agent_memory_remotes_.find(node_id);
                remote_targets.push_back({
                    node_id,
                    it == agent_memory_remotes_.end() ? nullptr : it->second});
            }
        }
    }

    for (const auto& target : remote_targets) {
        AgentMemoryClusterStatsNode node;
        node.node_id = target.first;
        node.local = false;
        if (!target.second) {
            node.error = "missing Agent Memory stats RPC client for node " +
                std::to_string(target.first);
            nodes.push_back(std::move(node));
            continue;
        }

        std::string rpc_error;
        const auto response = target.second->request_binary(
            zeptodb::cluster::RpcType::AGENT_MEMORY_STATS,
            zeptodb::cluster::RpcType::AGENT_MEMORY_STATS_RESULT,
            {},
            false,
            &rpc_error);
        if (response.empty() && !rpc_error.empty()) {
            node.error = rpc_error;
        } else if (!zeptodb::ai::deserialize_agent_memory_stats(
                       response.data(), response.size(), &node.stats,
                       &node.error)) {
            if (node.error.empty()) {
                node.error = "invalid Agent Memory stats RPC response";
            }
        }
        nodes.push_back(std::move(node));
    }

    return agent_memory_cluster_stats_json(nodes);
}

AgentMemoryOwnerFailoverResult HttpServer::handle_agent_memory_owner_failover(
    zeptodb::ai::AgentMemoryNodeId source_node_id,
    uint64_t source_ring_epoch,
    uint64_t new_ring_epoch,
    const std::vector<zeptodb::ai::AgentMemoryNodeId>& live_nodes) {
    AgentMemoryOwnerFailoverResult result;
    result.source_node_id = source_node_id;
    result.source_ring_epoch = source_ring_epoch;
    result.new_ring_epoch = new_ring_epoch;

    if (source_node_id == 0) {
        result.error = "agent memory failover source node is required";
        return result;
    }
    if (new_ring_epoch < source_ring_epoch) {
        result.error = "agent memory failover ring epoch is stale";
        return result;
    }

    std::vector<zeptodb::ai::AgentMemoryNodeId> survivors;
    survivors.reserve(live_nodes.size());
    for (const auto node_id : live_nodes) {
        if (node_id != 0 && node_id != source_node_id) {
            survivors.push_back(node_id);
        }
    }
    std::sort(survivors.begin(), survivors.end());
    survivors.erase(std::unique(survivors.begin(), survivors.end()),
                    survivors.end());
    if (survivors.empty()) {
        result.error = "agent memory failover requires at least one live node";
        return result;
    }

    const auto successor = std::upper_bound(
        survivors.begin(), survivors.end(), source_node_id);
    result.replacement_node_id = successor == survivors.end()
        ? survivors.front()
        : *successor;

    zeptodb::ai::AgentMemoryRouterConfig config;
    std::unordered_map<zeptodb::ai::AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (!agent_memory_router_) {
            result.error = "agent memory failover requires routing to be configured";
            return result;
        }
        config = agent_memory_router_->config();
        if (config.mode != zeptodb::ai::AgentMemoryRoutingMode::Routed) {
            result.error = "agent memory failover requires routed mode";
            return result;
        }
        if (config.self_node_id == 0) {
            result.error = "agent memory failover requires self node id";
            return result;
        }
        if (!std::binary_search(
                survivors.begin(), survivors.end(), config.self_node_id)) {
            result.error = "agent memory failover live nodes exclude this node";
            return result;
        }
        if (new_ring_epoch < config.ring_epoch) {
            result.error = "agent memory failover ring epoch is older than local routing";
            return result;
        }
        remotes = agent_memory_remotes_;
    }

    config.mode = zeptodb::ai::AgentMemoryRoutingMode::Routed;
    config.ring_epoch = new_ring_epoch;
    remotes.erase(source_node_id);

    std::string routing_error;
    if (!set_agent_memory_routing(config, survivors, std::move(remotes),
                                  &routing_error)) {
        result.error = routing_error;
        record_agent_memory_failover_status_(result);
        return result;
    }

    std::string persist_error;
    if (!persist_agent_memory_snapshot_(&persist_error, true)) {
        result.error = persist_error;
        record_agent_memory_failover_status_(result);
        return result;
    }

    if (config.self_node_id != result.replacement_node_id) {
        result.ok = true;
        record_agent_memory_failover_status_(result);
        return result;
    }

    std::string adopt_error;
    if (!adopt_agent_memory_owner_shard(source_node_id, source_ring_epoch,
                                        &adopt_error)) {
        if (adopt_error == "source agent memory shard is empty") {
            result.ok = true;
            result.degraded = true;
            result.replay_source_missing = true;
            result.degraded_reason = adopt_error;
            record_agent_memory_failover_status_(result);
            return result;
        }
        result.degraded = true;
        result.degraded_reason = adopt_error;
        result.error = adopt_error;
        record_agent_memory_failover_status_(result);
        return result;
    }

    result.ok = true;
    result.adopted = true;
    result.replica_promoted = true;
    record_agent_memory_failover_status_(result);
    return result;
}

bool HttpServer::adopt_agent_memory_owner_shard(
    zeptodb::ai::AgentMemoryNodeId source_node_id,
    uint64_t source_ring_epoch,
    std::string* error) {
    namespace fs = std::filesystem;

    std::string base_dir;
    uint32_t shard_id = 0;
    {
        std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
        if (agent_memory_persist_base_dir_.empty() ||
            agent_memory_persist_dir_.empty()) {
            if (error) *error = "agent memory persistence is not enabled";
            return false;
        }
        if (!agent_memory_persist_routed_) {
            if (error) *error = "agent memory shard adoption requires routed mode";
            return false;
        }
        base_dir = agent_memory_persist_base_dir_;
        shard_id = agent_memory_persist_shard_id_;
    }

    const auto source_dir = agent_memory_effective_snapshot_dir(
        base_dir, true, source_node_id, shard_id);
    const bool has_records = fs::exists(source_dir / "records.bin");
    const bool has_vectors = fs::exists(source_dir / "vectors.bin");
    const bool has_wal = fs::exists(source_dir / "wal.log");
    if (!has_records && !has_vectors && !has_wal) {
        if (error) *error = "source agent memory shard is empty";
        return false;
    }
    if (!validate_agent_memory_shard_manifest(
            source_dir, true, source_node_id, source_ring_epoch, shard_id,
            false, error)) {
        return false;
    }

    zeptodb::ai::AgentMemoryStore source_store;
    auto loaded = source_store.load_from_directory(source_dir.string());
    if (!loaded.ok) {
        if (error) *error = loaded.error;
        return false;
    }
    if (!replay_agent_memory_wal_into_store(source_dir.string(), source_store,
                                            error)) {
        return false;
    }

    for (auto record : source_store.memory_records_snapshot()) {
        auto stored = agent_memory_->put_memory(std::move(record));
        if (!stored.ok) {
            if (error) *error = stored.error;
            return false;
        }
    }
    for (auto entry : source_store.cache_entries_snapshot()) {
        auto stored = agent_memory_->store_cache(std::move(entry));
        if (!stored.ok) {
            if (error) *error = stored.error;
            return false;
        }
    }
    return persist_agent_memory_snapshot_(error, true);
}

bool HttpServer::persist_agent_memory_snapshot_(std::string* error, bool force) {
    std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
    if (agent_memory_persist_dir_.empty()) return true;
    if (!force && agent_memory_dirty_mutations_ == 0) return true;
    auto saved = agent_memory_->save_to_directory(agent_memory_persist_dir_);
    if (!saved.ok) {
        if (error) *error = saved.error;
        return false;
    }
    if (!write_agent_memory_shard_manifest(
            agent_memory_persist_dir_, agent_memory_persist_routed_,
            agent_memory_persist_node_id_, agent_memory_persist_ring_epoch_,
            agent_memory_persist_shard_id_, error)) {
        return false;
    }
    if (!truncate_agent_memory_wal_locked_(error)) {
        return false;
    }
    agent_memory_dirty_mutations_ = 0;
    return true;
}

bool HttpServer::mark_agent_memory_dirty_(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
        if (agent_memory_persist_dir_.empty()) return true;
        ++agent_memory_dirty_mutations_;
        if (agent_memory_flush_every_mutations_ == 0 ||
            agent_memory_dirty_mutations_ < agent_memory_flush_every_mutations_) {
            return true;
        }
    }
    return persist_agent_memory_snapshot_(error, true);
}

static bool replay_agent_memory_wal_into_store(
    const std::string& directory,
    zeptodb::ai::AgentMemoryStore& store,
    std::string* error) {
    namespace fs = std::filesystem;
    const fs::path wal_path = fs::path(directory) / "wal.log";
    if (!fs::exists(wal_path)) return true;

    std::ifstream wal(wal_path, std::ios::binary);
    if (!wal.is_open()) {
        if (error) *error = "failed to open agent memory WAL for replay";
        return false;
    }

    std::unordered_map<uint64_t, AgentMemoryWalPreparedMutation> prepared;
    for (;;) {
        if (wal.peek() == std::char_traits<char>::eof()) return true;

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t type = 0;
        uint32_t payload_len = 0;
        if (!read_u32_le(wal, &magic) ||
            !read_u32_le(wal, &version) ||
            !read_u32_le(wal, &type) ||
            !read_u32_le(wal, &payload_len)) {
            if (error) *error = "truncated agent memory WAL header";
            return false;
        }
        if (magic != kAgentMemoryWalMagic ||
            version != kAgentMemoryWalVersion ||
            payload_len > kAgentMemoryWalMaxPayload) {
            if (error) *error = "invalid agent memory WAL header";
            return false;
        }

        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0) {
            wal.read(reinterpret_cast<char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
            if (!wal) {
                if (error) *error = "truncated agent memory WAL payload";
                return false;
            }
        }

        if (type == kAgentMemoryWalPutMemory ||
            type == kAgentMemoryWalStoreCache ||
            type == kAgentMemoryWalDeleteMemory ||
            type == kAgentMemoryWalDeleteCache) {
            if (!apply_agent_memory_wal_mutation(type, payload, store, error)) {
                return false;
            }
            continue;
        }
        if (type == kAgentMemoryWalPrepareMemory ||
            type == kAgentMemoryWalPrepareCache ||
            type == kAgentMemoryWalPrepareDeleteMemory ||
            type == kAgentMemoryWalPrepareDeleteCache) {
            uint64_t tx_id = 0;
            AgentMemoryWalPreparedMutation mutation;
            if (!deserialize_agent_memory_wal_prepare(
                    type, payload, &tx_id, &mutation, error)) {
                return false;
            }
            prepared[tx_id] = std::move(mutation);
            continue;
        }
        if (type == kAgentMemoryWalCommit) {
            uint64_t tx_id = 0;
            if (!deserialize_agent_memory_wal_tx_id(payload, &tx_id, error)) {
                return false;
            }
            const auto it = prepared.find(tx_id);
            if (it == prepared.end()) {
                if (error) *error = "agent memory WAL commit without prepare";
                return false;
            }
            if (!apply_agent_memory_wal_mutation(
                    it->second.committed_type, it->second.payload, store,
                    error)) {
                return false;
            }
            prepared.erase(it);
            continue;
        }
        if (error) *error = "unknown agent memory WAL record type";
        return false;
    }
}

bool HttpServer::replay_agent_memory_wal_(const std::string& directory,
                                          std::string* error) {
    return replay_agent_memory_wal_into_store(directory, *agent_memory_, error);
}

static bool append_agent_memory_wal_record_to_dir(
    const std::filesystem::path& dir,
    bool routed,
    zeptodb::ai::AgentMemoryNodeId node_id,
    uint64_t ring_epoch,
    uint32_t shard_id,
    uint32_t type,
    const std::vector<uint8_t>& payload,
    std::string* error) {
    if (payload.size() > kAgentMemoryWalMaxPayload) {
        if (error) *error = "agent memory WAL payload too large";
        return false;
    }

    if (!validate_agent_memory_shard_manifest(
            dir, routed, node_id, ring_epoch, shard_id, false, error)) {
        return false;
    }
    if (!write_agent_memory_shard_manifest(
            dir, routed, node_id, ring_epoch, shard_id, error)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "failed to create agent memory WAL directory: " + ec.message();
        return false;
    }

    std::vector<uint8_t> header;
    header.reserve(16);
    append_u32_le(&header, kAgentMemoryWalMagic);
    append_u32_le(&header, kAgentMemoryWalVersion);
    append_u32_le(&header, type);
    append_u32_le(&header, static_cast<uint32_t>(payload.size()));

    std::ofstream wal(dir / "wal.log", std::ios::binary | std::ios::app);
    if (!wal.is_open()) {
        if (error) *error = "failed to open agent memory WAL for append";
        return false;
    }
    wal.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    if (!payload.empty()) {
        wal.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
    }
    wal.flush();
    if (!wal) {
        if (error) *error = "failed to append agent memory WAL record";
        return false;
    }
    return true;
}

bool HttpServer::append_agent_memory_wal_record_(
    uint32_t type,
    const std::vector<uint8_t>& payload,
    std::string* error) {
    std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
    if (agent_memory_persist_dir_.empty()) return true;
    return append_agent_memory_wal_record_to_dir(
        std::filesystem::path(agent_memory_persist_dir_),
        agent_memory_persist_routed_,
        agent_memory_persist_node_id_,
        agent_memory_persist_ring_epoch_,
        agent_memory_persist_shard_id_,
        type,
        payload,
        error);
}

uint64_t HttpServer::next_agent_memory_wal_tx_id_() {
    return agent_memory_wal_tx_counter_.fetch_add(1, std::memory_order_relaxed);
}

struct AgentMemoryReplicaAppendRecord {
    zeptodb::ai::AgentMemoryNodeId source_node_id = 0;
    uint64_t source_ring_epoch = 0;
    uint32_t shard_id = 0;
    uint32_t wal_type = 0;
    std::vector<uint8_t> payload;
};

static std::vector<uint8_t> serialize_agent_memory_replica_append(
    const AgentMemoryReplicaAppendRecord& record) {
    std::vector<uint8_t> out;
    out.reserve(24 + record.payload.size());
    append_u32_le(&out, record.source_node_id);
    append_u64_le(&out, record.source_ring_epoch);
    append_u32_le(&out, record.shard_id);
    append_u32_le(&out, record.wal_type);
    append_u32_le(&out, static_cast<uint32_t>(record.payload.size()));
    out.insert(out.end(), record.payload.begin(), record.payload.end());
    return out;
}

static bool deserialize_agent_memory_replica_append(
    const uint8_t* data,
    size_t len,
    AgentMemoryReplicaAppendRecord* record,
    std::string* error) {
    if (!record) {
        if (error) *error = "agent memory replica record output is null";
        return false;
    }
    const uint8_t* ptr = data;
    size_t remaining = len;
    AgentMemoryReplicaAppendRecord out;
    uint32_t source_node_id = 0;
    uint32_t payload_len = 0;
    if (!read_u32_le_bytes(ptr, remaining, &source_node_id) ||
        !read_u64_le_bytes(ptr, remaining, &out.source_ring_epoch) ||
        !read_u32_le_bytes(ptr, remaining, &out.shard_id) ||
        !read_u32_le_bytes(ptr, remaining, &out.wal_type) ||
        !read_u32_le_bytes(ptr, remaining, &payload_len)) {
        if (error) *error = "invalid agent memory replica append header";
        return false;
    }
    if (payload_len > kAgentMemoryWalMaxPayload || remaining != payload_len) {
        if (error) *error = "invalid agent memory replica append payload";
        return false;
    }
    out.source_node_id = source_node_id;
    out.payload.assign(ptr, ptr + remaining);
    *record = std::move(out);
    return true;
}

bool HttpServer::replicate_agent_memory_wal_record_(
    uint32_t type,
    const std::vector<uint8_t>& payload,
    bool local_record_counts,
    std::string* error) {
    struct Target {
        zeptodb::ai::AgentMemoryNodeId node_id = 0;
        std::shared_ptr<zeptodb::cluster::TcpRpcClient> client;
    };

    AgentMemoryReplicationMode mode = AgentMemoryReplicationMode::Routed;
    zeptodb::ai::AgentMemoryNodeId self_node_id = 0;
    std::vector<Target> targets;
    size_t total_nodes = 1;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        mode = agent_memory_replication_mode_;
        if (mode == AgentMemoryReplicationMode::Routed) return true;
        if (agent_memory_router_) {
            const auto config = agent_memory_router_->config();
            self_node_id = config.self_node_id;
            const auto nodes = agent_memory_router_->nodes();
            total_nodes = std::max<size_t>(nodes.size(), 1);
            for (const auto node_id : nodes) {
                if (node_id == config.self_node_id) continue;
                auto it = agent_memory_remotes_.find(node_id);
                targets.push_back({node_id,
                    it == agent_memory_remotes_.end() ? nullptr : it->second});
            }
        }
    }

    zeptodb::ai::AgentMemoryNodeId source_node_id = 0;
    uint64_t source_ring_epoch = 0;
    uint32_t shard_id = 0;
    {
        std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
        if (agent_memory_persist_dir_.empty() || !agent_memory_persist_routed_) {
            if (error) {
                *error = "agent memory replicated durability requires routed persistence";
            }
            return false;
        }
        source_node_id = agent_memory_persist_node_id_;
        source_ring_epoch = agent_memory_persist_ring_epoch_;
        shard_id = agent_memory_persist_shard_id_;
    }
    if (self_node_id != 0 && source_node_id != self_node_id) {
        if (error) *error = "agent memory replication source node mismatch";
        return false;
    }

    AgentMemoryReplicaAppendRecord record;
    record.source_node_id = source_node_id;
    record.source_ring_epoch = source_ring_epoch;
    record.shard_id = shard_id;
    record.wal_type = type;
    record.payload = payload;
    const auto wire = serialize_agent_memory_replica_append(record);

    size_t successful_nodes = local_record_counts ? 1 : 0;
    std::string first_error;
    for (const auto& target : targets) {
        if (!target.client) {
            if (first_error.empty()) {
                first_error = "missing Agent Memory replica RPC client for node " +
                    std::to_string(target.node_id);
            }
            continue;
        }
        std::string rpc_error;
        const auto response = target.client->request_binary(
            zeptodb::cluster::RpcType::AGENT_MEMORY_REPLICA_APPEND,
            zeptodb::cluster::RpcType::AGENT_MEMORY_REPLICA_ACK,
            wire,
            true,
            &rpc_error);
        if (response.size() == 1 && response[0] == 1u) {
            ++successful_nodes;
            continue;
        }
        if (first_error.empty()) {
            first_error = rpc_error.empty()
                ? "Agent Memory replica append rejected by node " +
                    std::to_string(target.node_id)
                : rpc_error;
        }
    }

    const size_t required_total_nodes =
        mode == AgentMemoryReplicationMode::Sync
            ? total_nodes
            : (total_nodes / 2) + 1;
    const size_t required_nodes = local_record_counts
        ? required_total_nodes
        : (required_total_nodes == 0 ? 0 : required_total_nodes - 1);
    if (successful_nodes >= required_nodes) return true;
    if (error) {
        *error = "Agent Memory replica acknowledgements " +
            std::to_string(successful_nodes) + "/" +
            std::to_string(local_record_counts ? total_nodes : total_nodes - 1) +
            " below required " +
            std::to_string(required_nodes);
        if (!first_error.empty()) *error += ": " + first_error;
    }
    return false;
}

bool HttpServer::truncate_agent_memory_wal_locked_(std::string* error) {
    namespace fs = std::filesystem;
    if (agent_memory_persist_dir_.empty()) return true;
    std::error_code ec;
    fs::remove(fs::path(agent_memory_persist_dir_) / "wal.log", ec);
    if (ec) {
        if (error) *error = "failed to truncate agent memory WAL: " + ec.message();
        return false;
    }
    return true;
}

bool HttpServer::persist_agent_memory_record_mutation_(
    const std::string& memory_id,
    const std::string& tenant_id,
    std::string* error) {
    const auto record = agent_memory_->get_memory(memory_id, tenant_id);
    if (!record.has_value()) {
        if (error) *error = "stored memory not found for WAL append";
        return false;
    }
    const auto payload = zeptodb::ai::serialize_memory_record(*record);
    const uint64_t tx_id = next_agent_memory_wal_tx_id_();
    const auto prepare_payload =
        serialize_agent_memory_wal_prepare(tx_id, payload);
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalPrepareMemory,
            prepare_payload,
            error)) {
        return false;
    }
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalPrepareMemory,
            prepare_payload,
            true,
            error)) {
        return false;
    }
    const auto commit_payload = serialize_agent_memory_wal_tx_id(tx_id);
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            false,
            error)) {
        return false;
    }
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            error)) {
        return false;
    }
    return mark_agent_memory_dirty_(error);
}

bool HttpServer::persist_agent_cache_entry_mutation_(
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt,
    std::string* error) {
    const auto entry = agent_memory_->get_cache(tenant_id, namespace_id, prompt);
    if (!entry.has_value()) {
        if (error) *error = "stored cache entry not found for WAL append";
        return false;
    }
    const auto payload = zeptodb::ai::serialize_cache_entry(*entry);
    const uint64_t tx_id = next_agent_memory_wal_tx_id_();
    const auto prepare_payload =
        serialize_agent_memory_wal_prepare(tx_id, payload);
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalPrepareCache,
            prepare_payload,
            error)) {
        return false;
    }
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalPrepareCache,
            prepare_payload,
            true,
            error)) {
        return false;
    }
    const auto commit_payload = serialize_agent_memory_wal_tx_id(tx_id);
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            false,
            error)) {
        return false;
    }
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            error)) {
        return false;
    }
    return mark_agent_memory_dirty_(error);
}

bool HttpServer::persist_agent_memory_delete_mutation_(
    const std::string& memory_id,
    const std::string& tenant_id,
    std::string* error) {
    const auto payload =
        serialize_agent_memory_wal_delete_memory(memory_id, tenant_id);
    const uint64_t tx_id = next_agent_memory_wal_tx_id_();
    const auto prepare_payload =
        serialize_agent_memory_wal_prepare(tx_id, payload);
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalPrepareDeleteMemory,
            prepare_payload,
            error)) {
        return false;
    }
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalPrepareDeleteMemory,
            prepare_payload,
            true,
            error)) {
        return false;
    }
    const auto commit_payload = serialize_agent_memory_wal_tx_id(tx_id);
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            false,
            error)) {
        return false;
    }
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            error)) {
        return false;
    }
    return mark_agent_memory_dirty_(error);
}

bool HttpServer::persist_agent_cache_delete_mutation_(
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt,
    std::string* error) {
    const auto payload =
        serialize_agent_memory_wal_delete_cache(tenant_id, namespace_id, prompt);
    const uint64_t tx_id = next_agent_memory_wal_tx_id_();
    const auto prepare_payload =
        serialize_agent_memory_wal_prepare(tx_id, payload);
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalPrepareDeleteCache,
            prepare_payload,
            error)) {
        return false;
    }
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalPrepareDeleteCache,
            prepare_payload,
            true,
            error)) {
        return false;
    }
    const auto commit_payload = serialize_agent_memory_wal_tx_id(tx_id);
    if (!replicate_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            false,
            error)) {
        return false;
    }
    if (!append_agent_memory_wal_record_(
            kAgentMemoryWalCommit,
            commit_payload,
            error)) {
        return false;
    }
    return mark_agent_memory_dirty_(error);
}

bool HttpServer::persist_agent_memory_eviction_tombstones_(
    const std::vector<zeptodb::ai::AgentMemoryEvictionEvent>& evictions,
    std::string* error,
    size_t* failed_index) {
    if (failed_index) *failed_index = evictions.size();
    for (size_t i = 0; i < evictions.size(); ++i) {
        const auto& eviction = evictions[i];
        if (eviction.target == zeptodb::ai::AgentMemoryEvictionTarget::Memory) {
            if (!persist_agent_memory_delete_mutation_(
                    eviction.memory_id, eviction.tenant_id, error)) {
                if (failed_index) *failed_index = i;
                return false;
            }
            continue;
        }
        if (!persist_agent_cache_delete_mutation_(
                eviction.tenant_id, eviction.namespace_id, eviction.prompt,
                error)) {
            if (failed_index) *failed_index = i;
            return false;
        }
    }
    return true;
}

static std::vector<zeptodb::ai::AgentMemoryEvictionEvent>
evictions_from_index(
    const std::vector<zeptodb::ai::AgentMemoryEvictionEvent>& evictions,
    size_t index) {
    if (index >= evictions.size()) return {};
    return {evictions.begin() + static_cast<std::ptrdiff_t>(index),
            evictions.end()};
}

static bool store_result_evicted_memory(
    const zeptodb::ai::StoreResult& result,
    const std::string& memory_id,
    const std::string& tenant_id) {
    return std::any_of(result.evictions.begin(), result.evictions.end(),
        [&](const zeptodb::ai::AgentMemoryEvictionEvent& eviction) {
            return eviction.target == zeptodb::ai::AgentMemoryEvictionTarget::Memory &&
                   eviction.memory_id == memory_id &&
                   (tenant_id.empty() || eviction.tenant_id == tenant_id);
        });
}

static void remove_evicted_memory(
    std::vector<zeptodb::ai::AgentMemoryEvictionEvent>* evictions,
    const std::string& memory_id,
    const std::string& tenant_id) {
    if (!evictions) return;
    evictions->erase(std::remove_if(evictions->begin(), evictions->end(),
        [&](const zeptodb::ai::AgentMemoryEvictionEvent& eviction) {
            return eviction.target == zeptodb::ai::AgentMemoryEvictionTarget::Memory &&
                   eviction.memory_id == memory_id &&
                   (tenant_id.empty() || eviction.tenant_id == tenant_id);
        }), evictions->end());
}

static bool store_result_evicted_cache(
    const zeptodb::ai::StoreResult& result,
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt) {
    const std::string normalized_prompt =
        zeptodb::ai::AgentMemoryStore::normalize_prompt(prompt);
    const std::string scope = namespace_id.empty() ? "default" : namespace_id;
    return std::any_of(result.evictions.begin(), result.evictions.end(),
        [&](const zeptodb::ai::AgentMemoryEvictionEvent& eviction) {
            return eviction.target == zeptodb::ai::AgentMemoryEvictionTarget::Cache &&
                   eviction.tenant_id == tenant_id &&
                   eviction.namespace_id == scope &&
                   zeptodb::ai::AgentMemoryStore::normalize_prompt(
                       eviction.prompt) == normalized_prompt;
        });
}

static void remove_evicted_cache(
    std::vector<zeptodb::ai::AgentMemoryEvictionEvent>* evictions,
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt) {
    if (!evictions) return;
    const std::string normalized_prompt =
        zeptodb::ai::AgentMemoryStore::normalize_prompt(prompt);
    const std::string scope = namespace_id.empty() ? "default" : namespace_id;
    evictions->erase(std::remove_if(evictions->begin(), evictions->end(),
        [&](const zeptodb::ai::AgentMemoryEvictionEvent& eviction) {
            return eviction.target == zeptodb::ai::AgentMemoryEvictionTarget::Cache &&
                   eviction.tenant_id == tenant_id &&
                   eviction.namespace_id == scope &&
                   zeptodb::ai::AgentMemoryStore::normalize_prompt(
                       eviction.prompt) == normalized_prompt;
        }), evictions->end());
}

std::vector<uint8_t> HttpServer::handle_agent_memory_put_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::MemoryRecord record;
    std::string error;
    if (!zeptodb::ai::deserialize_memory_record(data, len, &record, &error)) {
        return zeptodb::ai::serialize_store_result({false, {}, error});
    }
    const auto previous = record.memory_id.empty()
        ? std::optional<zeptodb::ai::MemoryRecord>{}
        : agent_memory_->get_memory(record.memory_id, record.tenant_id);
    const std::string tenant_id = record.tenant_id;
    auto stored = agent_memory_->put_memory(std::move(record));
    const bool stored_evicted = stored.ok &&
        store_result_evicted_memory(stored, stored.id, tenant_id);
    if (stored.ok && !stored_evicted &&
        !persist_agent_memory_record_mutation_(stored.id, tenant_id, &error)) {
        if (previous.has_value()) {
            (void)agent_memory_->put_memory(*previous);
        } else {
            (void)agent_memory_->remove_memory(stored.id, tenant_id);
        }
        (void)agent_memory_->restore_evicted_entries(stored.evictions);
        stored = {false, {}, error};
    }
    if (stored.ok) {
        size_t failed_index = stored.evictions.size();
        if (!persist_agent_memory_eviction_tombstones_(
                stored.evictions, &error, &failed_index)) {
            auto rollback_evictions =
                evictions_from_index(stored.evictions, failed_index);
            if (stored_evicted) {
                remove_evicted_memory(&rollback_evictions, stored.id, tenant_id);
            }
            (void)agent_memory_->restore_evicted_entries(rollback_evictions);
            stored = {false, {}, error};
        }
    }
    return zeptodb::ai::serialize_store_result(stored);
}

std::vector<uint8_t> HttpServer::handle_agent_cache_store_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::CacheEntry entry;
    std::string error;
    if (!zeptodb::ai::deserialize_cache_entry(data, len, &entry, &error)) {
        return zeptodb::ai::serialize_store_result({false, {}, error});
    }
    const std::string tenant_id = entry.tenant_id;
    const std::string namespace_id = entry.namespace_id;
    const std::string prompt = entry.prompt;
    const auto previous = agent_memory_->get_cache(
        tenant_id, namespace_id, prompt);
    auto stored = agent_memory_->store_cache(std::move(entry));
    const bool stored_evicted = stored.ok &&
        store_result_evicted_cache(stored, tenant_id, namespace_id, prompt);
    if (stored.ok && !stored_evicted &&
        !persist_agent_cache_entry_mutation_(tenant_id, namespace_id, prompt,
                                             &error)) {
        if (previous.has_value()) {
            (void)agent_memory_->store_cache(*previous);
        } else {
            (void)agent_memory_->remove_cache(tenant_id, namespace_id, prompt);
        }
        (void)agent_memory_->restore_evicted_entries(stored.evictions);
        stored = {false, {}, error};
    }
    if (stored.ok) {
        size_t failed_index = stored.evictions.size();
        if (!persist_agent_memory_eviction_tombstones_(
                stored.evictions, &error, &failed_index)) {
            auto rollback_evictions =
                evictions_from_index(stored.evictions, failed_index);
            if (stored_evicted) {
                remove_evicted_cache(&rollback_evictions, tenant_id,
                                     namespace_id, prompt);
            }
            (void)agent_memory_->restore_evicted_entries(rollback_evictions);
            stored = {false, {}, error};
        }
    }
    return zeptodb::ai::serialize_store_result(stored);
}

std::vector<uint8_t> HttpServer::handle_agent_memory_delete_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::MemoryDeleteRequest request;
    std::string error;
    if (!zeptodb::ai::deserialize_memory_delete_request(
            data, len, &request, &error)) {
        return zeptodb::ai::serialize_store_result({false, {}, error});
    }
    const auto previous =
        agent_memory_->get_memory(request.memory_id, request.tenant_id);
    if (!previous.has_value()) {
        return zeptodb::ai::serialize_store_result(
            {false, {}, "memory not found"});
    }
    if (!agent_memory_->remove_memory(request.memory_id, request.tenant_id)) {
        return zeptodb::ai::serialize_store_result(
            {false, {}, "memory not found"});
    }
    if (!persist_agent_memory_delete_mutation_(
            request.memory_id, request.tenant_id, &error)) {
        (void)agent_memory_->put_memory(*previous);
        return zeptodb::ai::serialize_store_result({false, {}, error});
    }
    return zeptodb::ai::serialize_store_result({true, request.memory_id, {}});
}

std::vector<uint8_t> HttpServer::handle_agent_cache_delete_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::CacheDeleteRequest request;
    std::string error;
    if (!zeptodb::ai::deserialize_cache_delete_request(
            data, len, &request, &error)) {
        return zeptodb::ai::serialize_store_result({false, {}, error});
    }
    const auto previous = agent_memory_->get_cache(
        request.tenant_id, request.namespace_id, request.prompt);
    if (!previous.has_value()) {
        return zeptodb::ai::serialize_store_result(
            {false, {}, "cache entry not found"});
    }
    if (!agent_memory_->remove_cache(
            request.tenant_id, request.namespace_id, request.prompt)) {
        return zeptodb::ai::serialize_store_result(
            {false, {}, "cache entry not found"});
    }
    if (!persist_agent_cache_delete_mutation_(
            request.tenant_id, request.namespace_id, request.prompt, &error)) {
        (void)agent_memory_->store_cache(*previous);
        return zeptodb::ai::serialize_store_result({false, {}, error});
    }
    return zeptodb::ai::serialize_store_result(
        {true, previous->cache_id, {}});
}

zeptodb::ai::StoreResult HttpServer::put_agent_memory_routed_(
    zeptodb::ai::MemoryRecord record,
    bool* local_write) {
    if (local_write) *local_write = true;

    std::shared_ptr<zeptodb::cluster::TcpRpcClient> remote;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_) {
            const auto key = zeptodb::ai::AgentMemoryRouter::memory_key(
                record.tenant_id, record.namespace_id, record.session_id,
                record.agent_id, record.user_id, record.memory_id);
            const auto owner = agent_memory_router_->route(key);
            if (!owner.local) {
                auto it = agent_memory_remotes_.find(owner.node_id);
                if (it == agent_memory_remotes_.end() || !it->second) {
                    return {false, {}, "agent memory owner has no RPC client"};
                }
                remote = it->second;
                if (local_write) *local_write = false;
            }
        }
    }

    if (!remote) {
        return agent_memory_->put_memory(std::move(record));
    }

    const auto payload = zeptodb::ai::serialize_memory_record(record);
    std::string error;
    const auto response = remote->request_binary(
        zeptodb::cluster::RpcType::AGENT_MEMORY_PUT,
        zeptodb::cluster::RpcType::AGENT_MEMORY_RESULT,
        payload,
        true,
        &error);
    if (response.empty() && !error.empty()) {
        return {false, {}, error};
    }
    zeptodb::ai::StoreResult stored;
    if (!zeptodb::ai::deserialize_store_result(response.data(), response.size(),
                                               &stored, &error)) {
        return {false, {}, error};
    }
    return stored;
}

zeptodb::ai::StoreResult HttpServer::store_agent_cache_routed_(
    zeptodb::ai::CacheEntry entry,
    bool* local_write) {
    if (local_write) *local_write = true;

    std::shared_ptr<zeptodb::cluster::TcpRpcClient> remote;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_) {
            const auto key = zeptodb::ai::AgentMemoryRouter::cache_key(
                entry.tenant_id, entry.namespace_id,
                zeptodb::ai::AgentMemoryStore::normalize_prompt(entry.prompt));
            const auto owner = agent_memory_router_->route(key);
            if (!owner.local) {
                auto it = agent_memory_remotes_.find(owner.node_id);
                if (it == agent_memory_remotes_.end() || !it->second) {
                    return {false, {}, "agent cache owner has no RPC client"};
                }
                remote = it->second;
                if (local_write) *local_write = false;
            }
        }
    }

    if (!remote) {
        return agent_memory_->store_cache(std::move(entry));
    }

    const auto payload = zeptodb::ai::serialize_cache_entry(entry);
    std::string error;
    const auto response = remote->request_binary(
        zeptodb::cluster::RpcType::AGENT_CACHE_STORE,
        zeptodb::cluster::RpcType::AGENT_CACHE_RESULT,
        payload,
        true,
        &error);
    if (response.empty() && !error.empty()) {
        return {false, {}, error};
    }
    zeptodb::ai::StoreResult stored;
    if (!zeptodb::ai::deserialize_store_result(response.data(), response.size(),
                                               &stored, &error)) {
        return {false, {}, error};
    }
    return stored;
}

std::vector<uint8_t> HttpServer::handle_agent_memory_get_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::MemoryGetRequest request;
    std::string error;
    if (!zeptodb::ai::deserialize_memory_get_request(data, len, &request, &error)) {
        return zeptodb::ai::serialize_memory_get_result({false, {}, error});
    }
    const auto record = agent_memory_->get_memory(request.memory_id,
                                                  request.tenant_id);
    zeptodb::ai::MemoryGetResult result;
    result.found = record.has_value();
    if (record.has_value()) result.record = *record;
    return zeptodb::ai::serialize_memory_get_result(result);
}

std::vector<uint8_t> HttpServer::handle_agent_memory_search_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::MemoryQuery query;
    std::string error;
    if (!zeptodb::ai::deserialize_memory_query(data, len, &query, &error)) {
        return zeptodb::ai::serialize_memory_search_result({{}, error});
    }
    auto matches = agent_memory_->search(std::move(query));
    return zeptodb::ai::serialize_memory_search_result({std::move(matches), {}});
}

std::vector<uint8_t> HttpServer::handle_agent_cache_lookup_rpc(
    const uint8_t* data,
    size_t len) {
    zeptodb::ai::CacheLookup lookup;
    std::string error;
    if (!zeptodb::ai::deserialize_cache_lookup(data, len, &lookup, &error)) {
        return zeptodb::ai::serialize_cache_lookup_result({});
    }
    const auto hit = agent_memory_->lookup_cache(std::move(lookup));
    return zeptodb::ai::serialize_cache_lookup_result(hit);
}

std::vector<uint8_t> HttpServer::handle_agent_memory_stats_rpc(
    const uint8_t* /*data*/,
    size_t /*len*/) {
    return zeptodb::ai::serialize_agent_memory_stats(agent_memory_->stats());
}

std::vector<uint8_t> HttpServer::handle_agent_memory_replica_append_rpc(
    const uint8_t* data,
    size_t len) {
    AgentMemoryReplicaAppendRecord record;
    std::string error;
    if (!deserialize_agent_memory_replica_append(data, len, &record, &error)) {
        return {0u};
    }
    if (record.wal_type != kAgentMemoryWalPutMemory &&
        record.wal_type != kAgentMemoryWalStoreCache &&
        record.wal_type != kAgentMemoryWalDeleteMemory &&
        record.wal_type != kAgentMemoryWalDeleteCache &&
        record.wal_type != kAgentMemoryWalPrepareMemory &&
        record.wal_type != kAgentMemoryWalPrepareCache &&
        record.wal_type != kAgentMemoryWalPrepareDeleteMemory &&
        record.wal_type != kAgentMemoryWalPrepareDeleteCache &&
        record.wal_type != kAgentMemoryWalCommit) {
        return {0u};
    }

    std::lock_guard<std::mutex> lock(agent_memory_persist_mu_);
    if (agent_memory_persist_base_dir_.empty()) return {0u};
    const auto source_dir = agent_memory_effective_snapshot_dir(
        agent_memory_persist_base_dir_, true, record.source_node_id,
        record.shard_id);
    const bool ok = append_agent_memory_wal_record_to_dir(
        source_dir,
        true,
        record.source_node_id,
        record.source_ring_epoch,
        record.shard_id,
        record.wal_type,
        record.payload,
        &error);
    return {static_cast<uint8_t>(ok ? 1u : 0u)};
}

zeptodb::ai::StoreResult HttpServer::delete_agent_memory_routed_(
    const std::string& memory_id,
    const std::string& namespace_id,
    const std::string& tenant_id,
    bool* local_write) {
    if (local_write) *local_write = true;

    std::shared_ptr<zeptodb::cluster::TcpRpcClient> remote;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_) {
            zeptodb::ai::AgentMemoryNodeId owner_node = 0;
            const bool resolved_from_id = parse_owner_scoped_memory_id(
                memory_id, &owner_node);
            bool local = true;
            const auto nodes = agent_memory_router_->nodes();
            const bool parsed_owner_is_live = resolved_from_id &&
                std::find(nodes.begin(), nodes.end(), owner_node) != nodes.end();
            if (parsed_owner_is_live) {
                local = owner_node == agent_memory_router_->config().self_node_id;
            } else {
                const auto key = zeptodb::ai::AgentMemoryRouter::memory_key(
                    tenant_id, namespace_id, "", "", "", memory_id);
                const auto owner = agent_memory_router_->route(key);
                owner_node = owner.node_id;
                local = owner.local;
            }
            if (!local) {
                auto it = agent_memory_remotes_.find(owner_node);
                if (it == agent_memory_remotes_.end() || !it->second) {
                    return {false, {}, "agent memory owner has no RPC client"};
                }
                remote = it->second;
                if (local_write) *local_write = false;
            }
        }
    }

    if (!remote) {
        if (!agent_memory_->remove_memory(memory_id, tenant_id)) {
            return {false, {}, "memory not found"};
        }
        return {true, memory_id, {}};
    }

    const zeptodb::ai::MemoryDeleteRequest request{memory_id, tenant_id};
    const auto payload = zeptodb::ai::serialize_memory_delete_request(request);
    std::string error;
    const auto response = remote->request_binary(
        zeptodb::cluster::RpcType::AGENT_MEMORY_DELETE,
        zeptodb::cluster::RpcType::AGENT_MEMORY_RESULT,
        payload,
        true,
        &error);
    if (response.empty() && !error.empty()) {
        return {false, {}, error};
    }
    zeptodb::ai::StoreResult deleted;
    if (!zeptodb::ai::deserialize_store_result(response.data(), response.size(),
                                               &deleted, &error)) {
        return {false, {}, error};
    }
    return deleted;
}

zeptodb::ai::StoreResult HttpServer::delete_agent_cache_routed_(
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt,
    bool* local_write) {
    if (local_write) *local_write = true;

    std::shared_ptr<zeptodb::cluster::TcpRpcClient> remote;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_) {
            const auto key = zeptodb::ai::AgentMemoryRouter::cache_key(
                tenant_id, namespace_id,
                zeptodb::ai::AgentMemoryStore::normalize_prompt(prompt));
            const auto owner = agent_memory_router_->route(key);
            if (!owner.local) {
                auto it = agent_memory_remotes_.find(owner.node_id);
                if (it == agent_memory_remotes_.end() || !it->second) {
                    return {false, {}, "agent cache owner has no RPC client"};
                }
                remote = it->second;
                if (local_write) *local_write = false;
            }
        }
    }

    if (!remote) {
        const auto previous = agent_memory_->get_cache(
            tenant_id, namespace_id, prompt);
        if (!previous.has_value() ||
            !agent_memory_->remove_cache(tenant_id, namespace_id, prompt)) {
            return {false, {}, "cache entry not found"};
        }
        return {true, previous->cache_id, {}};
    }

    const zeptodb::ai::CacheDeleteRequest request{
        tenant_id, namespace_id, prompt};
    const auto payload = zeptodb::ai::serialize_cache_delete_request(request);
    std::string error;
    const auto response = remote->request_binary(
        zeptodb::cluster::RpcType::AGENT_CACHE_DELETE,
        zeptodb::cluster::RpcType::AGENT_CACHE_RESULT,
        payload,
        true,
        &error);
    if (response.empty() && !error.empty()) {
        return {false, {}, error};
    }
    zeptodb::ai::StoreResult deleted;
    if (!zeptodb::ai::deserialize_store_result(response.data(), response.size(),
                                               &deleted, &error)) {
        return {false, {}, error};
    }
    return deleted;
}

zeptodb::ai::MemoryGetResult HttpServer::get_agent_memory_routed_(
    const std::string& memory_id,
    const std::string& namespace_id,
    const std::string& tenant_id) {
    std::shared_ptr<zeptodb::cluster::TcpRpcClient> remote;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_) {
            zeptodb::ai::AgentMemoryNodeId owner_node = 0;
            bool resolved_from_id = parse_owner_scoped_memory_id(
                memory_id, &owner_node);
            bool local = true;
            const auto nodes = agent_memory_router_->nodes();
            const bool parsed_owner_is_live = resolved_from_id &&
                std::find(nodes.begin(), nodes.end(), owner_node) != nodes.end();
            if (parsed_owner_is_live) {
                local = owner_node == agent_memory_router_->config().self_node_id;
            } else {
                const auto key = zeptodb::ai::AgentMemoryRouter::memory_key(
                    tenant_id, namespace_id, "", "", "", memory_id);
                const auto owner = agent_memory_router_->route(key);
                owner_node = owner.node_id;
                local = owner.local;
            }
            if (!local) {
                auto it = agent_memory_remotes_.find(owner_node);
                if (it == agent_memory_remotes_.end() || !it->second) {
                    return {false, {}, "agent memory owner has no RPC client"};
                }
                remote = it->second;
            }
        }
    }

    if (!remote) {
        const auto record = agent_memory_->get_memory(memory_id, tenant_id);
        zeptodb::ai::MemoryGetResult result;
        result.found = record.has_value();
        if (record.has_value()) result.record = *record;
        return result;
    }

    const zeptodb::ai::MemoryGetRequest request{memory_id, tenant_id};
    const auto payload = zeptodb::ai::serialize_memory_get_request(request);
    std::string error;
    const auto response = remote->request_binary(
        zeptodb::cluster::RpcType::AGENT_MEMORY_GET,
        zeptodb::cluster::RpcType::AGENT_MEMORY_GET_RESULT,
        payload,
        false,
        &error);
    if (response.empty() && !error.empty()) {
        return {false, {}, error};
    }
    zeptodb::ai::MemoryGetResult result;
    if (!zeptodb::ai::deserialize_memory_get_result(
            response.data(), response.size(), &result, &error)) {
        return {false, {}, error};
    }
    return result;
}

std::vector<zeptodb::ai::MemorySearchResult> HttpServer::search_agent_memory_routed_(
    zeptodb::ai::MemoryQuery query,
    std::string* error) {
    if (query.limit == 0) return {};

    struct RemoteSearch {
        zeptodb::ai::AgentMemoryNodeId node_id = 0;
        std::shared_ptr<zeptodb::cluster::TcpRpcClient> client;
    };
    struct RemoteSearchResult {
        zeptodb::ai::AgentMemoryNodeId node_id = 0;
        std::vector<zeptodb::ai::MemorySearchResult> matches;
        std::string error;
    };

    std::vector<RemoteSearch> remotes;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (!agent_memory_router_ ||
            agent_memory_router_->config().mode !=
                zeptodb::ai::AgentMemoryRoutingMode::Routed) {
            return agent_memory_->search(std::move(query));
        }

        const auto config = agent_memory_router_->config();
        const auto nodes = agent_memory_router_->nodes();
        for (const auto node_id : nodes) {
            if (node_id == config.self_node_id) continue;
            auto it = agent_memory_remotes_.find(node_id);
            if (it == agent_memory_remotes_.end() || !it->second) {
                if (error) {
                    *error = "agent memory search node has no RPC client";
                }
                return {};
            }
            remotes.push_back({node_id, it->second});
        }
    }

    std::vector<std::future<RemoteSearchResult>> futures;
    futures.reserve(remotes.size());
    for (const auto& remote : remotes) {
        futures.push_back(std::async(
            std::launch::async,
            [remote, query] {
                RemoteSearchResult out;
                out.node_id = remote.node_id;
                const auto payload = zeptodb::ai::serialize_memory_query(query);
                std::string rpc_error;
                const auto response = remote.client->request_binary(
                    zeptodb::cluster::RpcType::AGENT_MEMORY_SEARCH,
                    zeptodb::cluster::RpcType::AGENT_MEMORY_SEARCH_RESULT,
                    payload,
                    false,
                    &rpc_error);
                if (response.empty()) {
                    out.error = rpc_error.empty()
                        ? "empty agent memory search response"
                        : rpc_error;
                    return out;
                }
                zeptodb::ai::MemorySearchRpcResult decoded;
                if (!zeptodb::ai::deserialize_memory_search_result(
                        response.data(), response.size(), &decoded,
                        &rpc_error)) {
                    out.error = rpc_error;
                    return out;
                }
                out.matches = std::move(decoded.matches);
                out.error = std::move(decoded.error);
                return out;
            }));
    }

    auto merged = agent_memory_->search(query);
    for (auto& future : futures) {
        auto result = future.get();
        if (!result.error.empty()) {
            if (error) {
                *error = "agent memory search failed on node " +
                    std::to_string(result.node_id) + ": " + result.error;
            }
            return {};
        }
        merged.insert(merged.end(),
                      std::make_move_iterator(result.matches.begin()),
                      std::make_move_iterator(result.matches.end()));
    }
    trim_memory_matches(&merged, query.limit);
    return merged;
}

zeptodb::ai::CacheLookupResult HttpServer::lookup_agent_cache_routed_(
    zeptodb::ai::CacheLookup lookup) {
    std::shared_ptr<zeptodb::cluster::TcpRpcClient> remote;
    std::vector<std::shared_ptr<zeptodb::cluster::TcpRpcClient>> semantic_remotes;
    {
        std::lock_guard<std::mutex> lock(agent_memory_routing_mu_);
        if (agent_memory_router_) {
            const auto key = zeptodb::ai::AgentMemoryRouter::cache_key(
                lookup.tenant_id, lookup.namespace_id,
                zeptodb::ai::AgentMemoryStore::normalize_prompt(lookup.prompt));
            const auto owner = agent_memory_router_->route(key);
            if (!owner.local) {
                auto it = agent_memory_remotes_.find(owner.node_id);
                if (it != agent_memory_remotes_.end() && it->second) {
                    remote = it->second;
                }
            }
            if (agent_memory_router_->config().mode ==
                zeptodb::ai::AgentMemoryRoutingMode::Routed) {
                const auto config = agent_memory_router_->config();
                for (const auto node_id : agent_memory_router_->nodes()) {
                    if (node_id == config.self_node_id) continue;
                    auto it = agent_memory_remotes_.find(node_id);
                    if (it != agent_memory_remotes_.end() && it->second) {
                        semantic_remotes.push_back(it->second);
                    }
                }
            }
        } else {
            return agent_memory_->lookup_cache(std::move(lookup));
        }
    }

    auto exact_lookup = lookup;
    exact_lookup.embedding.clear();
    zeptodb::ai::CacheLookupResult exact_hit;
    if (remote) {
        const auto payload = zeptodb::ai::serialize_cache_lookup(exact_lookup);
        std::string error;
        const auto response = remote->request_binary(
            zeptodb::cluster::RpcType::AGENT_CACHE_LOOKUP_EXACT,
            zeptodb::cluster::RpcType::AGENT_CACHE_LOOKUP_RESULT,
            payload,
            false,
            &error);
        if (!response.empty()) {
            zeptodb::ai::deserialize_cache_lookup_result(
                response.data(), response.size(), &exact_hit, nullptr);
        }
    } else {
        exact_hit = agent_memory_->lookup_cache(exact_lookup);
    }
    if (exact_hit.hit) return exact_hit;

    if (lookup.embedding.empty()) return {};
    lookup.prompt.clear();
    auto best = agent_memory_->lookup_cache(lookup);
    const auto payload = zeptodb::ai::serialize_cache_lookup(lookup);
    for (const auto& semantic_remote : semantic_remotes) {
        std::string error;
        const auto response = semantic_remote->request_binary(
            zeptodb::cluster::RpcType::AGENT_CACHE_LOOKUP_EXACT,
            zeptodb::cluster::RpcType::AGENT_CACHE_LOOKUP_RESULT,
            payload,
            false,
            &error);
        if (response.empty()) continue;
        zeptodb::ai::CacheLookupResult candidate;
        if (!zeptodb::ai::deserialize_cache_lookup_result(
                response.data(), response.size(), &candidate, &error)) {
            continue;
        }
        if (candidate.hit && (!best.hit || candidate.score > best.score)) {
            best = std::move(candidate);
        }
    }
    return best;
}

// ============================================================================
// setup_auth_middleware — pre-routing handler for authentication
// ============================================================================
void HttpServer::setup_auth_middleware() {
    svr_->set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res)
        -> httplib::Server::HandlerResponse
    {
        // Stamp request start time + request ID for access logging
        auto& mutable_req = const_cast<httplib::Request&>(req);
        mutable_req.set_header("X-Zepto-Start-Us", std::to_string(now_us()));
        mutable_req.set_header("X-Zepto-Request-Id", gen_request_id());

        if (!auth_) {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        std::string auth_header;
        if (req.has_header("Authorization"))
            auth_header = req.get_header_value("Authorization");

        std::string remote_addr = req.remote_addr;

        auto decision = auth_->check(req.method, req.path,
                                     auth_header, remote_addr);

        if (decision.status == zeptodb::auth::AuthStatus::OK) {
            // Stash subject for access log
            mutable_req.set_header("X-Zepto-Subject", decision.context.subject);
            // Stash allowed_tables for table-level ACL enforcement
            if (!decision.context.allowed_tables.empty()) {
                std::string tables;
                for (size_t i = 0; i < decision.context.allowed_tables.size(); ++i) {
                    if (i > 0) tables += ',';
                    tables += decision.context.allowed_tables[i];
                }
                mutable_req.set_header("X-Zepto-Allowed-Tables", tables);
            }
            // Stash tenant_id for namespace enforcement
            if (!decision.context.tenant_id.empty())
                mutable_req.set_header("X-Zepto-Tenant-Id", decision.context.tenant_id);
            return httplib::Server::HandlerResponse::Unhandled;
        }

        int status_code = (decision.status == zeptodb::auth::AuthStatus::UNAUTHORIZED)
                          ? 401 : 403;
        res.status = status_code;
        if (status_code == 401)
            res.set_header("WWW-Authenticate", "Bearer realm=\"ZeptoDB\"");
        res.set_content(build_error_json(decision.reason), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
}

// ============================================================================
// setup_session_tracking — httplib logger fires after every request
// ============================================================================
void HttpServer::setup_session_tracking() {
    // Post-routing: inject X-Request-Id into response before it's sent
    svr_->set_post_routing_handler([](const httplib::Request& req,
                                       httplib::Response& res) {
        if (req.has_header("X-Zepto-Request-Id"))
            res.set_header("X-Request-Id",
                           req.get_header_value("X-Zepto-Request-Id"));
    });

    svr_->set_logger([this](const httplib::Request& req,
                            const httplib::Response& res) {
        // ── Access log ──────────────────────────────────────────────
        std::string request_id;
        if (req.has_header("X-Zepto-Request-Id"))
            request_id = req.get_header_value("X-Zepto-Request-Id");

        int64_t duration_us = 0;
        if (req.has_header("X-Zepto-Start-Us")) {
            int64_t start = std::stoll(req.get_header_value("X-Zepto-Start-Us"));
            duration_us = now_us() - start;
        }

        std::string subject;
        if (req.has_header("X-Zepto-Subject"))
            subject = req.get_header_value("X-Zepto-Subject");

        auto log_line = build_access_log(
            request_id, req.method, req.path,
            res.status, duration_us,
            req.body.size(), res.body.size(),
            req.remote_addr, subject);

        // Emit via structured logger if initialized, else silent
        auto& logger = zeptodb::util::Logger::instance();
        if (res.status >= 500) {
            logger.error(log_line, "http");
        } else if (res.status >= 400) {
            logger.warn(log_line, "http");
        } else if (req.path == "/health" || req.path == "/ready" ||
                   req.path == "/ping" || req.path == "/stats" ||
                   req.path == "/metrics" ||
                   req.path.rfind("/admin/", 0) == 0) {
            logger.debug(log_line, "http");
        } else {
            logger.info(log_line, "http");
        }

        // ── Session tracking ────────────────────────────────────────
        bool closing = req.has_header("Connection") &&
                       req.get_header_value("Connection") == "close";
        track_session(req.remote_addr, closing);
    });
}

void HttpServer::track_session(const std::string& remote_addr, bool is_closing) {
    using namespace std::chrono;
    int64_t now = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();

    std::unique_lock<std::mutex> lk(sessions_mu_);
    auto it = sessions_.find(remote_addr);
    if (it == sessions_.end()) {
        // New session — fire on_connect
        ConnectionInfo info;
        info.remote_addr     = remote_addr;
        info.user            = remote_addr;  // overridden by auth subject when available
        info.connected_at_ns = now;
        info.last_active_ns  = now;
        info.query_count     = 1;
        sessions_[remote_addr] = info;
        ConnectionInfo copy = sessions_[remote_addr];
        lk.unlock();
        if (on_connect_) on_connect_(copy);
    } else {
        it->second.last_active_ns = now;
        it->second.query_count++;
        if (is_closing) {
            // Client signalled connection close — fire on_disconnect
            ConnectionInfo copy = it->second;
            sessions_.erase(it);
            lk.unlock();
            if (on_disconnect_) on_disconnect_(copy);
        }
    }
}

std::vector<ConnectionInfo> HttpServer::list_sessions() const {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    std::vector<ConnectionInfo> result;
    result.reserve(sessions_.size());
    for (const auto& [_, info] : sessions_)
        result.push_back(info);
    return result;
}

size_t HttpServer::evict_idle_sessions(int64_t timeout_ms) {
    using namespace std::chrono;
    int64_t now = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t timeout_ns = timeout_ms * 1'000'000LL;

    std::unique_lock<std::mutex> lk(sessions_mu_);
    std::vector<ConnectionInfo> evicted;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (now - it->second.last_active_ns > timeout_ns) {
            evicted.push_back(it->second);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    lk.unlock();

    for (const auto& info : evicted)
        if (on_disconnect_) on_disconnect_(info);
    return evicted.size();
}

// ============================================================================
// setup_routes
// ============================================================================
void HttpServer::setup_routes() {
    // GET /ping — health check (ClickHouse compatible)
    svr_->Get("/ping", [](const httplib::Request& /*req*/,
                           httplib::Response& res) {
        res.set_content("Ok\n", "text/plain");
    });

    // GET /health — Kubernetes liveness probe
    svr_->Get("/health", [this](const httplib::Request& /*req*/,
                                 httplib::Response& res) {
        if (running_.load()) {
            res.set_content(R"({"status":"healthy"})", "application/json");
        } else {
            res.status = 503;
            res.set_content(R"({"status":"unhealthy"})", "application/json");
        }
    });

    // GET /ready — Kubernetes readiness probe
    svr_->Get("/ready", [this](const httplib::Request& /*req*/,
                                httplib::Response& res) {
        if (ready_.load()) {
            res.set_content(R"({"status":"ready"})", "application/json");
        } else {
            res.status = 503;
            res.set_content(R"({"status":"not_ready"})", "application/json");
        }
    });

    // GET /whoami — return authenticated identity and role
    svr_->Get("/whoami", [this](const httplib::Request& req,
                                 httplib::Response& res) {
        if (!auth_) {
            res.set_content(R"({"role":"admin","subject":"anonymous"})",
                            "application/json");
            return;
        }
        std::string auth_hdr;
        if (req.has_header("Authorization"))
            auth_hdr = req.get_header_value("Authorization");
        auto decision = auth_->check(req.method, req.path, auth_hdr, req.remote_addr);
        if (decision.status != zeptodb::auth::AuthStatus::OK) {
            res.status = 401;
            res.set_content(build_error_json(decision.reason), "application/json");
            return;
        }
        std::string role = "reader";
        if (decision.context.has_permission(zeptodb::auth::Permission::ADMIN))
            role = "admin";
        else if (decision.context.has_permission(zeptodb::auth::Permission::WRITE))
            role = "writer";
        else if (decision.context.has_permission(zeptodb::auth::Permission::METRICS))
            role = "metrics";
        res.set_content("{\"role\":\"" + role + "\",\"subject\":\""
                        + decision.context.subject + "\"}", "application/json");
    });

    // GET /metrics — Prometheus metrics (OpenMetrics format)
    svr_->Get("/metrics", [this](const httplib::Request& /*req*/,
                                  httplib::Response& res) {
        res.set_content(build_prometheus_metrics(), "text/plain; version=0.0.4");
    });

    // GET /stats — pipeline statistics
    svr_->Get("/stats", [this](const httplib::Request& /*req*/,
                                httplib::Response& res) {
        auto json = build_stats_json(executor_.stats());
        if (coordinator_ && json.size() >= 1 && json.back() == '}') {
            const auto st = coordinator_->small_table_join_stats();
            json.pop_back();
            json += ",\"small_table_join\":{";
            json += "\"candidates\":" + std::to_string(st.candidates);
            json += ",\"accepted\":" + std::to_string(st.accepted);
            json += ",\"rejected_row_cap\":" +
                    std::to_string(st.rejected_row_cap);
            json += ",\"errors\":" + std::to_string(st.errors);
            json += ",\"rows_materialized\":" +
                    std::to_string(st.rows_materialized);
            json += ",\"last_left_rows\":" +
                    std::to_string(st.last_left_rows);
            json += ",\"last_right_rows\":" +
                    std::to_string(st.last_right_rows);
            json += "}}";
        }
        res.set_content(json, "application/json");
    });

    // GET /api/ai/stats — Agent Memory counts, counters, and retention config.
    // Does not expose memory content, prompts, responses, or metadata.
    svr_->Get("/api/ai/stats", [this](const httplib::Request& req,
                                       httplib::Response& res) {
        const std::string scope = req.has_param("scope")
            ? req.get_param_value("scope")
            : "local";
        if (scope != "local" && scope != "cluster") {
            res.status = 400;
            res.set_content(build_error_json("scope must be local or cluster"),
                            "application/json");
            return;
        }
        res.set_content(build_agent_memory_stats_json(scope == "cluster"),
                        "application/json");
    });

    // GET /api/ai/memories/:id — point lookup, routed by owner-scoped id when enabled.
    svr_->Get(R"(/api/ai/memories/([^/]+))", [this](const httplib::Request& req,
                                                     httplib::Response& res) {
        const std::string memory_id = req.matches[1];
        const std::string namespace_id = req.has_param("namespace")
            ? req.get_param_value("namespace")
            : "default";
        std::string tenant_id = req.has_param("tenant_id")
            ? req.get_param_value("tenant_id")
            : "";
        std::string error;
        if (!apply_tenant_header(req, &tenant_id, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }

        const auto result = get_agent_memory_routed_(memory_id, namespace_id,
                                                     tenant_id);
        if (!result.error.empty()) {
            res.status = 502;
            res.set_content(build_error_json(result.error), "application/json");
            return;
        }
        if (!result.found) {
            res.status = 404;
            res.set_content(build_error_json("memory not found"), "application/json");
            return;
        }
        zeptodb::ai::MemorySearchResult match;
        match.record = result.record;
        std::ostringstream os;
        os << "{\"found\":true,\"memory\":" << memory_json(match) << "}";
        res.set_content(os.str(), "application/json");
    });

    // DELETE /api/ai/memories/:id — remove one memory and append a WAL tombstone.
    svr_->Delete(R"(/api/ai/memories/([^/]+))", [this](
        const httplib::Request& req, httplib::Response& res) {
        const std::string memory_id = req.matches[1];
        const std::string namespace_id = req.has_param("namespace")
            ? req.get_param_value("namespace")
            : "default";
        std::string tenant_id = req.has_param("tenant_id")
            ? req.get_param_value("tenant_id")
            : "";
        std::string error;
        if (!apply_tenant_header(req, &tenant_id, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }

        const auto previous = agent_memory_->get_memory(memory_id, tenant_id);
        bool local_write = true;
        auto deleted = delete_agent_memory_routed_(
            memory_id, namespace_id, tenant_id, &local_write);
        if (!deleted.ok) {
            res.status = deleted.error.find("not found") != std::string::npos
                ? 404
                : 400;
            res.set_content(build_error_json(deleted.error), "application/json");
            return;
        }
        if (local_write &&
            !persist_agent_memory_delete_mutation_(memory_id, tenant_id, &error)) {
            if (previous.has_value()) {
                (void)agent_memory_->put_memory(*previous);
            }
            res.status = 500;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        res.set_content(R"({"ok":true,"memory_id":")" +
                        json_escape(memory_id) + R"("})",
                        "application/json");
    });

    // GET /api/license — public license info
    svr_->Get("/api/license", [](const httplib::Request& /*req*/,
                                  httplib::Response& res) {
        auto& lic = zeptodb::auth::license();
        std::ostringstream os;
        os << "{\"edition\":\"" << (lic.edition() == zeptodb::auth::Edition::ENTERPRISE ? "enterprise" : "community") << "\"";
        os << ",\"features\":[";
        static const struct { zeptodb::auth::Feature f; const char* name; } feat_map[] = {
            {zeptodb::auth::Feature::CLUSTER, "cluster"},
            {zeptodb::auth::Feature::SSO, "sso"},
            {zeptodb::auth::Feature::AUDIT_EXPORT, "audit_export"},
            {zeptodb::auth::Feature::ADVANCED_RBAC, "advanced_rbac"},
            {zeptodb::auth::Feature::KAFKA, "kafka"},
            {zeptodb::auth::Feature::MIGRATION, "migration"},
            {zeptodb::auth::Feature::GEO_REPLICATION, "geo_replication"},
            {zeptodb::auth::Feature::ROLLING_UPGRADE, "rolling_upgrade"},
            {zeptodb::auth::Feature::IOT_CONNECTORS, "iot_connectors"},
        };
        bool first = true;
        for (auto& [f, name] : feat_map) {
            if (lic.hasFeature(f)) {
                if (!first) os << ",";
                os << "\"" << name << "\"";
                first = false;
            }
        }
        os << "]";
        os << ",\"max_nodes\":" << lic.maxNodes();
        os << ",\"trial\":" << (lic.isTrial() ? "true" : "false");
        os << ",\"expired\":" << (lic.isExpired() ? "true" : "false");
        if (lic.edition() == zeptodb::auth::Edition::ENTERPRISE) {
            if (!lic.claims().company.empty())
                os << ",\"company\":\"" << lic.claims().company << "\"";
            if (lic.claims().expiry > 0) {
                std::time_t t = static_cast<std::time_t>(lic.claims().expiry);
                std::tm tm{}; gmtime_r(&t, &tm);
                char buf[16]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
                os << ",\"expires\":\"" << buf << "\"";
            }
        } else {
            os << ",\"upgrade_url\":\"https://zeptodb.com/pricing\"";
        }
        os << "}";
        res.set_content(os.str(), "application/json");
    });

    // POST /api/ai/memories — upsert an agent memory object.
    svr_->Post("/api/ai/memories", [this](const httplib::Request& req,
                                           httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content(build_error_json("Empty JSON body"), "application/json");
            return;
        }
        zeptodb::ai::MemoryRecord record;
        record.memory_id      = json_string_field(req.body, "memory_id");
        record.tenant_id      = json_string_field(req.body, "tenant_id");
        record.namespace_id   = json_string_field(req.body, "namespace", "default");
        record.user_id        = json_string_field(req.body, "user_id");
        record.session_id     = json_string_field(req.body, "session_id");
        record.agent_id       = json_string_field(req.body, "agent_id");
        record.type           = json_string_field(req.body, "type", "memory");
        record.content        = json_string_field(req.body, "content");
        record.metadata_json  = json_string_field(req.body, "metadata_json", "{}");
        record.token_count    = json_i64_field(req.body, "token_count", 0);
        record.importance     = json_double_field(req.body, "importance", 0.0);
        record.created_at_ns  = json_i64_field(req.body, "created_at_ns", 0);
        record.expires_at_ns  = json_i64_field(req.body, "expires_at_ns", 0);
        record.pinned         = json_bool_field(req.body, "pinned", false);

        std::string error;
        if (!apply_tenant_header(req, &record.tenant_id, &error) ||
            !json_float_array_field(req.body, "embedding", &record.embedding, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        const std::string tenant_id = record.tenant_id;
        const auto previous = record.memory_id.empty()
            ? std::optional<zeptodb::ai::MemoryRecord>{}
            : agent_memory_->get_memory(record.memory_id, tenant_id);
        bool local_write = true;
        auto stored = put_agent_memory_routed_(std::move(record), &local_write);
        if (!stored.ok) {
            res.status = 400;
            res.set_content(build_error_json(stored.error), "application/json");
            return;
        }
        const bool stored_evicted = local_write &&
            store_result_evicted_memory(stored, stored.id, tenant_id);
        if (local_write && !stored_evicted &&
            !persist_agent_memory_record_mutation_(stored.id, tenant_id, &error)) {
            if (previous.has_value()) {
                (void)agent_memory_->put_memory(*previous);
            } else {
                (void)agent_memory_->remove_memory(stored.id, tenant_id);
            }
            (void)agent_memory_->restore_evicted_entries(stored.evictions);
            res.status = 500;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        if (local_write) {
            size_t failed_index = stored.evictions.size();
            if (!persist_agent_memory_eviction_tombstones_(
                    stored.evictions, &error, &failed_index)) {
                auto rollback_evictions =
                    evictions_from_index(stored.evictions, failed_index);
                if (stored_evicted) {
                    remove_evicted_memory(&rollback_evictions, stored.id,
                                          tenant_id);
                }
                (void)agent_memory_->restore_evicted_entries(rollback_evictions);
                res.status = 500;
                res.set_content(build_error_json(error), "application/json");
                return;
            }
        }
        res.set_content(R"({"ok":true,"memory_id":")" + json_escape(stored.id) + R"("})",
                        "application/json");
    });

    // POST /api/ai/memories/search — filtered vector/metadata memory search.
    svr_->Post("/api/ai/memories/search", [this](const httplib::Request& req,
                                                  httplib::Response& res) {
        zeptodb::ai::MemoryQuery query;
        query.tenant_id    = json_string_field(req.body, "tenant_id");
        query.namespace_id = json_string_field(req.body, "namespace", "default");
        query.user_id      = json_string_field(req.body, "user_id");
        query.session_id   = json_string_field(req.body, "session_id");
        query.agent_id     = json_string_field(req.body, "agent_id");
        query.type         = json_string_field(req.body, "type");
        std::string error;
        if (!json_nonnegative_size_field(req.body, "limit", 10, &query.limit, &error) ||
            !apply_tenant_header(req, &query.tenant_id, &error) ||
            !json_float_array_field(req.body, "query_embedding",
                                    &query.query_embedding, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        auto matches = search_agent_memory_routed_(query, &error);
        if (!error.empty()) {
            res.status = 502;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        std::ostringstream os;
        os << "{\"matches\":[";
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i) os << ",";
            os << memory_json(matches[i]);
        }
        os << "],\"rows\":" << matches.size() << "}";
        res.set_content(os.str(), "application/json");
    });

    // POST /api/ai/context — assemble memories under a token budget.
    svr_->Post("/api/ai/context", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        zeptodb::ai::ContextRequest query;
        query.tenant_id    = json_string_field(req.body, "tenant_id");
        query.namespace_id = json_string_field(req.body, "namespace", "default");
        query.user_id      = json_string_field(req.body, "user_id");
        query.session_id   = json_string_field(req.body, "session_id");
        query.agent_id     = json_string_field(req.body, "agent_id");
        query.type         = json_string_field(req.body, "type");
        query.token_budget = json_i64_field(req.body, "token_budget", 0);

        std::string error;
        if (!json_nonnegative_size_field(req.body, "limit", 10, &query.limit, &error) ||
            !apply_tenant_header(req, &query.tenant_id, &error) ||
            !json_float_array_field(req.body, "query_embedding",
                                    &query.query_embedding, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        auto matches = search_agent_memory_routed_(query, &error);
        if (!error.empty()) {
            res.status = 502;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        auto context = assemble_context_from_matches(std::move(matches),
                                                     query.token_budget);
        std::ostringstream os;
        os << "{\"memories\":[";
        for (size_t i = 0; i < context.memories.size(); ++i) {
            if (i) os << ",";
            os << memory_json(context.memories[i]);
        }
        os << "],\"token_count\":" << context.token_count
           << ",\"rows\":" << context.memories.size() << "}";
        res.set_content(os.str(), "application/json");
    });

    // POST /api/ai/cache/store — store exact/semantic LLM cache entry.
    svr_->Post("/api/ai/cache/store", [this](const httplib::Request& req,
                                              httplib::Response& res) {
        zeptodb::ai::CacheEntry entry;
        entry.cache_id      = json_string_field(req.body, "cache_id");
        entry.tenant_id     = json_string_field(req.body, "tenant_id");
        entry.namespace_id  = json_string_field(req.body, "namespace", "default");
        entry.prompt        = json_string_field(req.body, "prompt");
        entry.response      = json_string_field(req.body, "response");
        entry.metadata_json = json_string_field(req.body, "metadata_json", "{}");
        entry.token_count   = json_i64_field(req.body, "token_count", 0);
        entry.expires_at_ns = json_i64_field(req.body, "expires_at_ns", 0);

        std::string error;
        if (!apply_tenant_header(req, &entry.tenant_id, &error) ||
            !json_float_array_field(req.body, "embedding", &entry.embedding, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        const std::string tenant_id = entry.tenant_id;
        const std::string namespace_id = entry.namespace_id;
        const std::string prompt = entry.prompt;
        const auto previous = agent_memory_->get_cache(
            tenant_id, namespace_id, prompt);
        bool local_write = true;
        auto stored = store_agent_cache_routed_(std::move(entry), &local_write);
        if (!stored.ok) {
            res.status = 400;
            res.set_content(build_error_json(stored.error), "application/json");
            return;
        }
        const bool stored_evicted = local_write &&
            store_result_evicted_cache(stored, tenant_id, namespace_id, prompt);
        if (local_write && !stored_evicted &&
            !persist_agent_cache_entry_mutation_(tenant_id, namespace_id, prompt,
                                                 &error)) {
            if (previous.has_value()) {
                (void)agent_memory_->store_cache(*previous);
            } else {
                (void)agent_memory_->remove_cache(tenant_id, namespace_id, prompt);
            }
            (void)agent_memory_->restore_evicted_entries(stored.evictions);
            res.status = 500;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        if (local_write) {
            size_t failed_index = stored.evictions.size();
            if (!persist_agent_memory_eviction_tombstones_(
                    stored.evictions, &error, &failed_index)) {
                auto rollback_evictions =
                    evictions_from_index(stored.evictions, failed_index);
                if (stored_evicted) {
                    remove_evicted_cache(&rollback_evictions, tenant_id,
                                         namespace_id, prompt);
                }
                (void)agent_memory_->restore_evicted_entries(rollback_evictions);
                res.status = 500;
                res.set_content(build_error_json(error), "application/json");
                return;
            }
        }
        res.set_content(R"({"ok":true,"cache_id":")" + json_escape(stored.id) + R"("})",
                        "application/json");
    });

    // DELETE /api/ai/cache — remove one exact cache entry and append a WAL tombstone.
    svr_->Delete("/api/ai/cache", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        std::string tenant_id = req.has_param("tenant_id")
            ? req.get_param_value("tenant_id")
            : "";
        const std::string namespace_id = req.has_param("namespace")
            ? req.get_param_value("namespace")
            : "default";
        const std::string prompt = req.has_param("prompt")
            ? req.get_param_value("prompt")
            : "";
        std::string error;
        if (prompt.empty()) {
            res.status = 400;
            res.set_content(build_error_json("prompt is required"), "application/json");
            return;
        }
        if (!apply_tenant_header(req, &tenant_id, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }

        const auto previous = agent_memory_->get_cache(
            tenant_id, namespace_id, prompt);
        bool local_write = true;
        auto deleted = delete_agent_cache_routed_(
            tenant_id, namespace_id, prompt, &local_write);
        if (!deleted.ok) {
            res.status = deleted.error.find("not found") != std::string::npos
                ? 404
                : 400;
            res.set_content(build_error_json(deleted.error), "application/json");
            return;
        }
        if (local_write &&
            !persist_agent_cache_delete_mutation_(
                tenant_id, namespace_id, prompt, &error)) {
            if (previous.has_value()) {
                (void)agent_memory_->store_cache(*previous);
            }
            res.status = 500;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        res.set_content(R"({"ok":true,"cache_id":")" +
                        json_escape(deleted.id) + R"("})",
                        "application/json");
    });

    // POST /api/ai/cache/lookup — exact prompt cache first, semantic fallback.
    svr_->Post("/api/ai/cache/lookup", [this](const httplib::Request& req,
                                               httplib::Response& res) {
        zeptodb::ai::CacheLookup lookup;
        lookup.tenant_id = json_string_field(req.body, "tenant_id");
        lookup.namespace_id = json_string_field(req.body, "namespace", "default");
        lookup.prompt = json_string_field(req.body, "prompt");
        lookup.semantic_threshold =
            json_double_field(req.body, "semantic_threshold", 0.92);

        std::string error;
        if (!apply_tenant_header(req, &lookup.tenant_id, &error) ||
            !json_float_array_field(req.body, "embedding", &lookup.embedding, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }
        auto hit = lookup_agent_cache_routed_(std::move(lookup));
        if (!hit.hit) {
            res.set_content(R"({"hit":false})", "application/json");
            return;
        }
        std::ostringstream os;
        os << "{\"hit\":true,\"kind\":\"" << (hit.exact ? "exact" : "semantic")
           << "\",\"score\":" << hit.score
           << ",\"entry\":" << cache_entry_json(hit.entry) << "}";
        res.set_content(os.str(), "application/json");
    });

    // POST /insert/arrow — ingest Arrow IPC RecordBatchStream payloads.
    svr_->Post("/insert/arrow", [this](const httplib::Request& req,
                                        httplib::Response& res) {
        if (!arrow_ipc_available()) {
            res.status = 406;
            res.set_content(
                R"({"error":"Arrow IPC not available in this build"})",
                "application/json");
            return;
        }

        auto param = [&req](const std::string& name,
                            const std::string& fallback = {}) {
            return req.has_param(name) ? req.get_param_value(name) : fallback;
        };

        auto parse_double_param = [&req](const std::string& name,
                                         double fallback,
                                         double* out,
                                         std::string* error) {
            if (!req.has_param(name)) {
                *out = fallback;
                return true;
            }
            const std::string value = req.get_param_value(name);
            char* end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
                if (error) *error = name + " must be a finite number";
                return false;
            }
            *out = parsed;
            return true;
        };

        ArrowIpcIngestOptions opts;
        opts.table_name = param("table", param("table_name"));
        opts.symbol_column = param("sym_col", param("symbol_col", opts.symbol_column));
        opts.price_column = param("price_col", opts.price_column);
        opts.volume_column = param("vol_col", param("volume_col", opts.volume_column));
        opts.timestamp_column = param("ts_col", param("timestamp_col", opts.timestamp_column));
        opts.msg_type_column = param("msg_type_col", opts.msg_type_column);

        std::string error;
        const std::string volume_scale_param =
            req.has_param("volume_scale") ? "volume_scale" : "vol_scale";
        if (!parse_double_param("price_scale", 1.0, &opts.price_scale, &error) ||
            !parse_double_param(volume_scale_param, 1.0, &opts.volume_scale, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }

        if (req.has_header("X-Zepto-Allowed-Tables")) {
            if (opts.table_name.empty()) {
                res.status = 403;
                res.set_content(build_error_json(
                    "Arrow insert requires table= when table ACL is restricted"),
                    "application/json");
                return;
            }
            std::string allowed = req.get_header_value("X-Zepto-Allowed-Tables");
            std::istringstream ss(allowed);
            std::string t;
            bool allowed_table = false;
            while (std::getline(ss, t, ',')) {
                if (t == opts.table_name) {
                    allowed_table = true;
                    break;
                }
            }
            if (!allowed_table) {
                res.status = 403;
                res.set_content(build_error_json(
                    "Access denied: table '" + opts.table_name +
                    "' not in allowed list"),
                    "application/json");
                return;
            }
        }

        if (tenant_mgr_ && req.has_header("X-Zepto-Tenant-Id")) {
            std::string tid = req.get_header_value("X-Zepto-Tenant-Id");
            if (!tid.empty()) {
                if (opts.table_name.empty()) {
                    res.status = 400;
                    res.set_content(build_error_json(
                        "Arrow insert requires table= for tenant-scoped requests"),
                        "application/json");
                    return;
                }
                if (!tenant_mgr_->can_access_table(tid, opts.table_name)) {
                    res.status = 403;
                    res.set_content(build_error_json(
                        "Tenant '" + tid + "' cannot access table '" +
                        opts.table_name + "'"),
                        "application/json");
                    return;
                }
            }
        }

        auto ingested = ingest_arrow_ipc_stream(executor_, req.body, opts);
        if (!ingested.ok) {
            res.status = ingested.error.find("support not compiled") != std::string::npos
                ? 406
                : 400;
            res.set_content(build_error_json(ingested.error), "application/json");
            return;
        }

        res.set_header("X-Zepto-Format", "arrow-stream");
        res.set_content("{\"inserted\":" + std::to_string(ingested.rows) +
                        ",\"failed\":" + std::to_string(ingested.failed) + "}",
                        "application/json");
    });

    // POST /insert/msgpack — ingest MessagePack map-of-column-arrays payloads.
    svr_->Post("/insert/msgpack", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        auto param = [&req](const std::string& name,
                            const std::string& fallback = {}) {
            return req.has_param(name) ? req.get_param_value(name) : fallback;
        };

        auto parse_double_param = [&req](const std::string& name,
                                         double fallback,
                                         double* out,
                                         std::string* error) {
            if (!req.has_param(name)) {
                *out = fallback;
                return true;
            }
            const std::string value = req.get_param_value(name);
            char* end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
                if (error) *error = name + " must be a finite number";
                return false;
            }
            *out = parsed;
            return true;
        };

        MsgpackIngestOptions opts;
        opts.table_name = param("table", param("table_name"));
        opts.symbol_column = param("sym_col", param("symbol_col", opts.symbol_column));
        opts.price_column = param("price_col", opts.price_column);
        opts.volume_column = param("vol_col", param("volume_col", opts.volume_column));
        opts.timestamp_column = param("ts_col", param("timestamp_col", opts.timestamp_column));
        opts.msg_type_column = param("msg_type_col", opts.msg_type_column);

        std::string error;
        const std::string volume_scale_param =
            req.has_param("volume_scale") ? "volume_scale" : "vol_scale";
        if (!parse_double_param("price_scale", 1.0, &opts.price_scale, &error) ||
            !parse_double_param(volume_scale_param, 1.0, &opts.volume_scale, &error)) {
            res.status = 400;
            res.set_content(build_error_json(error), "application/json");
            return;
        }

        if (req.has_header("X-Zepto-Allowed-Tables")) {
            if (opts.table_name.empty()) {
                res.status = 403;
                res.set_content(build_error_json(
                    "MessagePack insert requires table= when table ACL is restricted"),
                    "application/json");
                return;
            }
            std::string allowed = req.get_header_value("X-Zepto-Allowed-Tables");
            std::istringstream ss(allowed);
            std::string t;
            bool allowed_table = false;
            while (std::getline(ss, t, ',')) {
                if (t == opts.table_name) {
                    allowed_table = true;
                    break;
                }
            }
            if (!allowed_table) {
                res.status = 403;
                res.set_content(build_error_json(
                    "Access denied: table '" + opts.table_name +
                    "' not in allowed list"),
                    "application/json");
                return;
            }
        }

        if (tenant_mgr_ && req.has_header("X-Zepto-Tenant-Id")) {
            std::string tid = req.get_header_value("X-Zepto-Tenant-Id");
            if (!tid.empty()) {
                if (opts.table_name.empty()) {
                    res.status = 400;
                    res.set_content(build_error_json(
                        "MessagePack insert requires table= for tenant-scoped requests"),
                        "application/json");
                    return;
                }
                if (!tenant_mgr_->can_access_table(tid, opts.table_name)) {
                    res.status = 403;
                    res.set_content(build_error_json(
                        "Tenant '" + tid + "' cannot access table '" +
                        opts.table_name + "'"),
                        "application/json");
                    return;
                }
            }
        }

        auto ingested = ingest_msgpack_columns(executor_, req.body, opts);
        if (!ingested.ok) {
            res.status = 400;
            res.set_content(build_error_json(ingested.error), "application/json");
            return;
        }

        res.set_header("X-Zepto-Format", "msgpack-columnar");
        res.set_content("{\"inserted\":" + std::to_string(ingested.rows) +
                        ",\"failed\":" + std::to_string(ingested.failed) + "}",
                        "application/json");
    });

    // POST / — execute SQL query (ClickHouse compatible)
    svr_->Post("/", [this](const httplib::Request& req,
                            httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content(build_error_json("Empty query body"), "application/json");
            return;
        }

        // Table-level ACL + tenant namespace enforcement
        // Parse once; resolve the touched table name across SELECT/DML/DDL/DESCRIBE.
        std::string touched_table;
        zeptodb::sql::ParsedStatement cached_ps;
        bool have_parsed = false;
        {
            try {
                zeptodb::sql::Parser parser;
                cached_ps = parser.parse_statement(req.body);
                have_parsed = true;
                auto& ps = cached_ps;
                if (ps.select)             touched_table = ps.select->from_table;
                else if (ps.insert)        touched_table = ps.insert->table_name;
                else if (ps.update)        touched_table = ps.update->table_name;
                else if (ps.del)           touched_table = ps.del->table_name;
                else if (ps.create_table)  touched_table = ps.create_table->table_name;
                else if (ps.drop_table)    touched_table = ps.drop_table->table_name;
                else if (ps.alter_table)   touched_table = ps.alter_table->table_name;
                else if (ps.kind == zeptodb::sql::ParsedStatement::Kind::DESCRIBE_TABLE)
                    touched_table = ps.describe_table_name;
            } catch (...) {} // parse failure → let executor surface the error
        }

        // allowed_tables ACL (from API key / JWT claims)
        if (!touched_table.empty() && req.has_header("X-Zepto-Allowed-Tables")) {
            std::string allowed = req.get_header_value("X-Zepto-Allowed-Tables");
            std::vector<std::string> allowed_tables;
            std::istringstream ss(allowed);
            std::string t;
            while (std::getline(ss, t, ','))
                if (!t.empty()) allowed_tables.push_back(t);

            if (!allowed_tables.empty()) {
                bool ok = false;
                for (const auto& a : allowed_tables)
                    if (a == touched_table) { ok = true; break; }
                if (!ok) {
                    res.status = 403;
                    res.set_content(build_error_json(
                        "Access denied: table '" + touched_table + "' not in allowed list"),
                        "application/json");
                    return;
                }
            }
        }

        // Tenant namespace enforcement
        if (!touched_table.empty() && tenant_mgr_ &&
            req.has_header("X-Zepto-Tenant-Id")) {
            std::string tid = req.get_header_value("X-Zepto-Tenant-Id");
            if (!tid.empty() && !tenant_mgr_->can_access_table(tid, touched_table)) {
                res.status = 403;
                res.set_content(build_error_json(
                    "Tenant '" + tid + "' cannot access table '" + touched_table + "'"),
                    "application/json");
                return;
            }
        }

        // Prime prepared-statement cache with the ACL parse (devlog 091 F4)
        // so run_query_with_tracking → execute() hits the cache instead of
        // re-parsing the same SQL.
        if (have_parsed) {
            executor_.cache_prepared(req.body, std::move(cached_ps));
        }

        auto result = run_query_with_tracking(req.body, req.remote_addr);

        if (!result.ok()) {
            res.status = (result.error == "Query cancelled" ||
                          result.error == "Query timed out") ? 408 : 400;
            res.set_content(build_error_json(result.error), "application/json");
            return;
        }

        // DDL replication to remote pods (fire-and-forget — devlog 112).
        // Only runs in cluster mode (coordinator_ wired).  We rely on the
        // ACL-path pre-parse (`cached_ps`) to classify DDL — no extra parse,
        // no string matching.
        if (coordinator_ && have_parsed &&
            (cached_ps.create_table || cached_ps.drop_table ||
             cached_ps.alter_table)) {
            coordinator_->forward_ddl_to_remotes(req.body);
        }

        // ----------------------------------------------------------------
        // Arrow IPC content negotiation (devlog 119).
        // Triggered by:
        //   - Accept header containing `application/vnd.apache.arrow.stream`
        //   - `?default_format=Arrow` or `Arrow`/`ArrowStream` (ClickHouse)
        //   - `?format=arrow`
        // Errors (parse, executor, ACL) always stay JSON — only successful
        // result sets get encoded as Arrow. Matches ClickHouse semantics.
        // ----------------------------------------------------------------
        bool want_arrow = false;
        {
            auto ieq = [](const std::string& a, const std::string& b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(a[i])) !=
                        std::tolower(static_cast<unsigned char>(b[i])))
                        return false;
                return true;
            };
            std::string default_fmt = req.get_param_value("default_format");
            std::string fmt         = req.get_param_value("format");
            if (ieq(default_fmt, "Arrow") || ieq(default_fmt, "ArrowStream") ||
                ieq(fmt, "arrow") || ieq(fmt, "arrowstream")) {
                want_arrow = true;
            } else if (req.has_header("Accept")) {
                std::string accept = req.get_header_value("Accept");
                std::string accept_lc = accept;
                std::transform(accept_lc.begin(), accept_lc.end(),
                               accept_lc.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                if (accept_lc.find("application/vnd.apache.arrow.stream") !=
                    std::string::npos) {
                    want_arrow = true;
                }
            }
        }

        if (want_arrow) {
            if (!arrow_ipc_available()) {
                res.status = 406;
                res.set_content(
                    R"({"error":"Arrow IPC not available in this build"})",
                    "application/json");
                return;
            }
            std::string body, err;
            if (!encode_result_set_ipc(result, &body, &err)) {
                res.status = 500;
                res.set_content(build_error_json("Arrow IPC encode failed: " + err),
                                "application/json");
                return;
            }
            res.set_header("X-Zepto-Format", "arrow-stream");
            res.set_content(std::move(body),
                            "application/vnd.apache.arrow.stream");
            return;
        }

        res.set_content(build_json_response(result), "application/json");
    });

    // GET / — execute SQL query via query parameter
    svr_->Get("/", [this](const httplib::Request& req,
                           httplib::Response& res) {
        auto q = req.get_param_value("query");
        if (q.empty()) {
            res.set_content(R"({"status":"ok","engine":"ZeptoDB"})", "application/json");
            return;
        }

        auto result = run_query_with_tracking(q, req.remote_addr);
        if (!result.ok()) {
            res.status = (result.error == "Query cancelled" ||
                          result.error == "Query timed out") ? 408 : 400;
            res.set_content(build_error_json(result.error), "application/json");
            return;
        }

        res.set_content(build_json_response(result), "application/json");
    });
}

// ============================================================================
// run_query_with_tracking — executes SQL with timeout + QueryTracker
// ============================================================================
zeptodb::sql::QueryResultSet HttpServer::run_query_with_tracking(
    const std::string& sql,
    const std::string& subject)
{
    auto token = std::make_shared<zeptodb::auth::CancellationToken>();
    std::string query_id = query_tracker_.register_query(subject, sql, token);

    int64_t start = now_us();
    zeptodb::sql::QueryResultSet result;
    auto is_cluster_select = [this, &sql]() {
        if (!coordinator_) return false;
        try {
            zeptodb::sql::Parser parser;
            auto ps = parser.parse_statement(sql);
            return ps.kind == zeptodb::sql::ParsedStatement::Kind::SELECT;
        } catch (...) {
            return false;
        }
    };
    const bool route_select_via_coordinator = is_cluster_select();
    auto execute_query = [this, &sql, &token, route_select_via_coordinator]() {
        if (route_select_via_coordinator) {
            return coordinator_->execute_sql(sql);
        }
        return executor_.execute(sql, token.get());
    };

    if (query_timeout_ms_ > 0) {
        // Run the query on a separate thread; cancel after timeout
        auto future = std::async(std::launch::async, execute_query);

        auto status = future.wait_for(std::chrono::milliseconds(query_timeout_ms_));
        if (status == std::future_status::timeout) {
            token->cancel();
            future.wait();
            result.error = "Query timed out";
        } else {
            result = future.get();
        }
    } else {
        result = execute_query();
    }

    int64_t duration_us = now_us() - start;
    query_tracker_.complete(query_id);

    // Slow query log (>100ms)
    auto& logger = zeptodb::util::Logger::instance();
    if (duration_us > 100'000 || !result.ok()) {
        auto level = !result.ok() ? zeptodb::util::LogLevel::WARN
                                  : zeptodb::util::LogLevel::INFO;
        std::ostringstream os;
        os << "{\"query_id\":\"" << query_id << "\""
           << ",\"subject\":\"" << subject << "\""
           << ",\"duration_us\":" << duration_us
           << ",\"rows\":" << result.rows.size()
           << ",\"ok\":" << (result.ok() ? "true" : "false");
        if (!result.ok())
            os << ",\"error\":\"" << result.error << "\"";
        // Truncate SQL to 200 chars for log safety
        std::string sql_trunc = sql.substr(0, 200);
        // Escape quotes
        for (size_t p = 0; (p = sql_trunc.find('"', p)) != std::string::npos; p += 2)
            sql_trunc.insert(p, "\\");
        os << ",\"sql\":\"" << sql_trunc << "\""
           << "}";
        logger.log(level, os.str(), "query");
    }

    return result;
}

// ============================================================================
// setup_admin_routes — admin endpoints (require ADMIN permission)
// ============================================================================
void HttpServer::setup_admin_routes() {
    // Helper: check admin permission from request (inline — auth_ may be null)
    auto require_admin = [this](const httplib::Request& req,
                                httplib::Response& res) -> bool {
        if (!auth_) return true;  // auth disabled
        std::string auth_hdr;
        if (req.has_header("Authorization"))
            auth_hdr = req.get_header_value("Authorization");
        auto decision = auth_->check(req.method, req.path, auth_hdr, req.remote_addr);
        if (decision.status != zeptodb::auth::AuthStatus::OK) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer realm=\"ZeptoDB\"");
            res.set_content(build_error_json(decision.reason), "application/json");
            return false;
        }
        if (!decision.context.has_permission(zeptodb::auth::Permission::ADMIN)) {
            res.status = 403;
            res.set_content(build_error_json("Admin permission required"),
                            "application/json");
            return false;
        }
        return true;
    };

    // Feature gate helper — returns 402 if feature not available
    auto require_feature = [](httplib::Response& res, zeptodb::auth::Feature f, const std::string& name) -> bool {
        if (!zeptodb::auth::license().hasFeature(f)) {
            res.status = 402;
            res.set_content(build_402_json(name), "application/json");
            return false;
        }
        return true;
    };

    // -------------------------------------------------------------------------
    // GET /admin/license — full license details (admin only)
    // -------------------------------------------------------------------------
    svr_->Get("/admin/license", [require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        auto& lic = zeptodb::auth::license();
        std::ostringstream os;
        os << "{\"edition\":\"" << (lic.edition() == zeptodb::auth::Edition::ENTERPRISE ? "enterprise" : "community") << "\"";
        os << ",\"features\":[";
        static const struct { zeptodb::auth::Feature f; const char* name; } feat_map[] = {
            {zeptodb::auth::Feature::CLUSTER, "cluster"},
            {zeptodb::auth::Feature::SSO, "sso"},
            {zeptodb::auth::Feature::AUDIT_EXPORT, "audit_export"},
            {zeptodb::auth::Feature::ADVANCED_RBAC, "advanced_rbac"},
            {zeptodb::auth::Feature::KAFKA, "kafka"},
            {zeptodb::auth::Feature::MIGRATION, "migration"},
            {zeptodb::auth::Feature::GEO_REPLICATION, "geo_replication"},
            {zeptodb::auth::Feature::ROLLING_UPGRADE, "rolling_upgrade"},
            {zeptodb::auth::Feature::IOT_CONNECTORS, "iot_connectors"},
        };
        bool first = true;
        for (auto& [f, name] : feat_map) {
            if (lic.hasFeature(f)) {
                if (!first) os << ",";
                os << "\"" << name << "\"";
                first = false;
            }
        }
        os << "]";
        os << ",\"max_nodes\":" << lic.maxNodes();
        os << ",\"trial\":" << (lic.isTrial() ? "true" : "false");
        os << ",\"expired\":" << (lic.isExpired() ? "true" : "false");
        os << ",\"company\":\"" << lic.claims().company << "\"";
        os << ",\"tenant_id\":\"" << lic.claims().tenant_id << "\"";
        os << ",\"grace_days\":" << lic.claims().grace_days;
        os << ",\"issued_at\":" << lic.claims().issued_at;
        if (lic.claims().expiry > 0) {
            std::time_t t = static_cast<std::time_t>(lic.claims().expiry);
            std::tm tm{}; gmtime_r(&t, &tm);
            char buf[16]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            os << ",\"expires\":\"" << buf << "\"";
        }
        os << "}";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/license — upload a license key
    // -------------------------------------------------------------------------
    svr_->Post("/admin/license", [require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        auto& lic = zeptodb::auth::license();
        if (lic.load(req.body)) {
            res.set_content("{\"loaded\":true,\"edition\":\"" +
                std::string(lic.edition() == zeptodb::auth::Edition::ENTERPRISE ? "enterprise" : "community") +
                "\",\"company\":\"" + lic.claims().company + "\"}", "application/json");
        } else {
            res.status = 400;
            res.set_content(R"({"loaded":false,"error":"Invalid license key"})", "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // POST /admin/license/trial — generate and load a trial key
    // -------------------------------------------------------------------------
    svr_->Post("/admin/license/trial", [require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        std::string trial_jwt = zeptodb::auth::LicenseValidator::generate_trial_key();
        auto& lic = zeptodb::auth::license();
        lic.load(trial_jwt);
        std::time_t t = static_cast<std::time_t>(lic.claims().expiry);
        std::tm tm{}; gmtime_r(&t, &tm);
        char buf[16]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        res.set_content("{\"loaded\":true,\"edition\":\"enterprise\",\"trial\":true,\"expires\":\"" +
            std::string(buf) + "\"}", "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/keys — create API key
    // Body: {"name":"<name>","role":"<role>","symbols":["SYM1",...],"tables":["T1",...],"tenant_id":"...","expires_at_ns":0}
    // -------------------------------------------------------------------------
    svr_->Post("/admin/keys", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }
        // Minimal JSON parsing for string fields
        auto extract_str = [&](const std::string& field) -> std::string {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return "";
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return "";
            auto q1 = req.body.find('"', colon + 1);
            if (q1 == std::string::npos) return "";
            auto q2 = req.body.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return req.body.substr(q1 + 1, q2 - q1 - 1);
        };
        // Parse JSON array of strings: "field":["a","b"]
        auto extract_arr = [&](const std::string& field) -> std::vector<std::string> {
            std::vector<std::string> result;
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return result;
            auto bracket = req.body.find('[', pos + pat.size());
            if (bracket == std::string::npos) return result;
            auto end_bracket = req.body.find(']', bracket);
            if (end_bracket == std::string::npos) return result;
            std::string arr = req.body.substr(bracket + 1, end_bracket - bracket - 1);
            size_t p = 0;
            while (p < arr.size()) {
                auto q1 = arr.find('"', p);
                if (q1 == std::string::npos) break;
                auto q2 = arr.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                auto val = arr.substr(q1 + 1, q2 - q1 - 1);
                if (!val.empty()) result.push_back(val);
                p = q2 + 1;
            }
            return result;
        };
        // Parse integer field
        auto extract_int = [&](const std::string& field) -> int64_t {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return 0;
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return 0;
            size_t start = colon + 1;
            while (start < req.body.size() && (req.body[start] == ' ' || req.body[start] == '\t')) ++start;
            try { return std::stoll(req.body.substr(start)); } catch (...) { return 0; }
        };

        std::string name = extract_str("name");
        std::string role_str = extract_str("role");
        if (name.empty()) {
            res.status = 400;
            res.set_content(build_error_json("Missing 'name' field"), "application/json");
            return;
        }
        zeptodb::auth::Role role = zeptodb::auth::role_from_string(role_str);
        auto symbols = extract_arr("symbols");
        auto tables = extract_arr("tables");
        std::string tenant_id = extract_str("tenant_id");
        int64_t expires_at_ns = extract_int("expires_at_ns");

        std::string key = auth_->create_api_key(name, role, symbols, tables, tenant_id, expires_at_ns);
        res.set_content("{\"key\":\"" + key + "\"}", "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/keys — list API keys
    // -------------------------------------------------------------------------
    svr_->Get("/admin/keys", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!auth_) {
            res.set_content("[]", "application/json");
            return;
        }
        auto keys = auth_->list_api_keys();
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) os << ",";
            const auto& k = keys[i];
            os << "{\"id\":\"" << k.id << "\","
               << "\"name\":\"" << k.name << "\","
               << "\"role\":\"" << zeptodb::auth::role_to_string(k.role) << "\","
               << "\"enabled\":" << (k.enabled ? "true" : "false") << ","
               << "\"created_at_ns\":" << k.created_at_ns << ","
               << "\"last_used_ns\":" << k.last_used_ns << ","
               << "\"expires_at_ns\":" << k.expires_at_ns << ","
               << "\"tenant_id\":\"" << k.tenant_id << "\","
               << "\"allowed_symbols\":[";
            for (size_t si = 0; si < k.allowed_symbols.size(); ++si) {
                if (si > 0) os << ",";
                os << "\"" << k.allowed_symbols[si] << "\"";
            }
            os << "],\"allowed_tables\":[";
            for (size_t ti = 0; ti < k.allowed_tables.size(); ++ti) {
                if (ti > 0) os << ",";
                os << "\"" << k.allowed_tables[ti] << "\"";
            }
            os << "]}";
        }
        os << "]";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // DELETE /admin/keys/:id — revoke API key
    // -------------------------------------------------------------------------
    svr_->Delete(R"(/admin/keys/([^/]+))", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }
        std::string key_id = req.matches[1];
        bool ok = auth_->revoke_api_key(key_id);
        if (ok) {
            res.set_content(R"({"revoked":true})", "application/json");
        } else {
            res.status = 404;
            res.set_content(build_error_json("Key not found"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // PATCH /admin/keys/:id — update API key fields
    // Body: {"symbols":["S1"],"tables":["T1"],"enabled":true,"tenant_id":"x","expires_at_ns":0}
    // All fields optional — only provided fields are updated.
    // -------------------------------------------------------------------------
    svr_->Patch(R"(/admin/keys/([^/]+))", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }
        std::string key_id = req.matches[1];

        // Parse optional fields from body
        auto has_field = [&](const std::string& f) {
            return req.body.find("\"" + f + "\"") != std::string::npos;
        };
        auto extract_str = [&](const std::string& field) -> std::string {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return "";
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return "";
            auto q1 = req.body.find('"', colon + 1);
            if (q1 == std::string::npos) return "";
            auto q2 = req.body.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return req.body.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_arr = [&](const std::string& field) -> std::vector<std::string> {
            std::vector<std::string> result;
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return result;
            auto bracket = req.body.find('[', pos + pat.size());
            if (bracket == std::string::npos) return result;
            auto end_bracket = req.body.find(']', bracket);
            if (end_bracket == std::string::npos) return result;
            std::string arr = req.body.substr(bracket + 1, end_bracket - bracket - 1);
            size_t p = 0;
            while (p < arr.size()) {
                auto q1 = arr.find('"', p);
                if (q1 == std::string::npos) break;
                auto q2 = arr.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                auto val = arr.substr(q1 + 1, q2 - q1 - 1);
                if (!val.empty()) result.push_back(val);
                p = q2 + 1;
            }
            return result;
        };
        auto extract_bool = [&](const std::string& field) -> bool {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return false;
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return false;
            auto rest = req.body.substr(colon + 1);
            return rest.find("true") < rest.find("false");
        };
        auto extract_int = [&](const std::string& field) -> int64_t {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return 0;
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return 0;
            size_t start = colon + 1;
            while (start < req.body.size() && (req.body[start] == ' ' || req.body[start] == '\t')) ++start;
            try { return std::stoll(req.body.substr(start)); } catch (...) { return 0; }
        };

        std::optional<std::vector<std::string>> symbols;
        std::optional<std::vector<std::string>> tables;
        std::optional<bool> enabled;
        std::optional<std::string> tenant_id;
        std::optional<int64_t> expires_at_ns;

        if (has_field("symbols"))       symbols = extract_arr("symbols");
        if (has_field("tables"))        tables = extract_arr("tables");
        if (has_field("enabled"))       enabled = extract_bool("enabled");
        if (has_field("tenant_id"))     tenant_id = extract_str("tenant_id");
        if (has_field("expires_at_ns")) expires_at_ns = extract_int("expires_at_ns");

        bool ok = auth_->update_api_key(key_id, symbols, tables, enabled, tenant_id, expires_at_ns);
        if (ok) {
            res.set_content(R"({"updated":true})", "application/json");
        } else {
            res.status = 404;
            res.set_content(build_error_json("Key not found"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // GET /admin/queries — list active queries
    // -------------------------------------------------------------------------
    svr_->Get("/admin/queries", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        auto queries = query_tracker_.list();
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < queries.size(); ++i) {
            if (i > 0) os << ",";
            const auto& q = queries[i];
            os << "{\"id\":\"" << q.query_id << "\","
               << "\"subject\":\"" << q.subject << "\","
               << "\"sql\":\"" << q.sql_preview << "\","
               << "\"started_at_ns\":" << q.started_at_ns << "}";
        }
        os << "]";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // DELETE /admin/queries/:id — cancel a query
    // -------------------------------------------------------------------------
    svr_->Delete(R"(/admin/queries/([^/]+))", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        std::string qid = req.matches[1];
        bool ok = query_tracker_.cancel(qid);
        if (ok) {
            res.set_content(R"({"cancelled":true})", "application/json");
        } else {
            res.status = 404;
            res.set_content(build_error_json("Query not found"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // GET /admin/audit — recent audit events (last N, default 100)
    // -------------------------------------------------------------------------
    svr_->Get("/admin/audit", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::AUDIT_EXPORT)) {
            res.status = 402;
            res.set_content(build_402_json("Audit log export"), "application/json");
            return;
        }
        if (!auth_) {
            res.set_content("[]", "application/json");
            return;
        }
        size_t n = 100;
        if (req.has_param("n")) {
            try { n = std::stoul(req.get_param_value("n")); } catch (...) {}
        }
        auto events = auth_->audit_buffer().last(n);
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < events.size(); ++i) {
            if (i > 0) os << ",";
            const auto& e = events[i];
            auto esc = [](const std::string& s) {
                std::string out;
                for (char c : s) {
                    if (c == '"') out += "\\\"";
                    else if (c == '\\') out += "\\\\";
                    else out += c;
                }
                return out;
            };
            os << "{\"ts\":" << e.timestamp_ns << ","
               << "\"subject\":\"" << esc(e.subject) << "\","
               << "\"role\":\"" << esc(e.role_str) << "\","
               << "\"action\":\"" << esc(e.action) << "\","
               << "\"detail\":\"" << esc(e.detail) << "\","
               << "\"from\":\"" << esc(e.remote_addr) << "\"}";
        }
        os << "]";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/sessions — list active client sessions (.z.po equivalent)
    // -------------------------------------------------------------------------
    svr_->Get("/admin/sessions", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        auto sessions = list_sessions();
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (i > 0) os << ",";
            const auto& s = sessions[i];
            os << "{\"remote_addr\":\"" << s.remote_addr << "\","
               << "\"user\":\"" << s.user << "\","
               << "\"connected_at_ns\":" << s.connected_at_ns << ","
               << "\"last_active_ns\":" << s.last_active_ns << ","
               << "\"query_count\":" << s.query_count << "}";
        }
        os << "]";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/version — server version info
    // -------------------------------------------------------------------------
    svr_->Get("/admin/version", [require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        res.set_content(
            R"({"engine":"ZeptoDB","version":"0.1.0","build":")" __DATE__ R"("})",
            "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/nodes — list cluster node info
    // Standalone: self only. Cluster: coordinator collects from all nodes.
    // -------------------------------------------------------------------------
    svr_->Get("/admin/nodes", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;

        if (coordinator_) {
            // Cluster mode: collect stats from all nodes via RPC
            std::ostringstream os;
            os << "{\"nodes\":[";
            auto lock = coordinator_->router_read_lock();
            // Access endpoints via scatter — send STATS_REQUEST to each
            // We use the coordinator's internal node list
            lock.unlock();

            // Scatter stats requests to all nodes
            auto results = coordinator_scatter_stats();
            for (size_t i = 0; i < results.size(); ++i) {
                if (i > 0) os << ",";
                os << results[i];
            }
            os << "]}";
            res.set_content(os.str(), "application/json");
        } else {
            // Standalone mode: self only
            const auto& stats = executor_.stats();
            std::ostringstream os;
            os << "{\"nodes\":[{\"id\":0"
               << ",\"host\":\"localhost\""
               << ",\"port\":" << port_
               << ",\"state\":\"ACTIVE\""
               << ",\"ticks_ingested\":" << stats.ticks_ingested.load()
               << ",\"ticks_stored\":" << stats.ticks_stored.load()
               << ",\"queries_executed\":" << stats.queries_executed.load()
               << "}]}";
            res.set_content(os.str(), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // GET /admin/cluster — cluster overview (stats, partitions, memory)
    // -------------------------------------------------------------------------
    svr_->Get("/admin/cluster", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        const auto& stats = executor_.stats();
        std::ostringstream os;
        if (coordinator_) {
            auto nc = coordinator_->node_count();
            os << "{\"mode\":\"" << (nc > 1 ? "cluster" : "standalone") << "\""
               << ",\"node_count\":" << nc;
        } else {
            os << "{\"mode\":\"standalone\""
               << ",\"node_count\":1";
        }
        os << ",\"partitions_created\":" << stats.partitions_created.load()
           << ",\"partitions_evicted\":" << stats.partitions_evicted.load()
           << ",\"ticks_ingested\":" << stats.ticks_ingested.load()
           << ",\"ticks_stored\":" << stats.ticks_stored.load()
           << ",\"ticks_dropped\":" << stats.ticks_dropped.load()
           << ",\"queries_executed\":" << stats.queries_executed.load()
           << ",\"total_rows_scanned\":" << stats.total_rows_scanned.load()
           << ",\"last_ingest_latency_ns\":" << stats.last_ingest_latency_ns.load()
           << "}";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/nodes — add a remote node to the cluster
    // Body: {"id":2,"host":"10.0.1.2","port":8123}
    // -------------------------------------------------------------------------
    svr_->Post("/admin/nodes", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Multi-node cluster")) return;
        if (!coordinator_) {
            res.status = 400;
            res.set_content(build_error_json("Not in cluster mode — set_coordinator() first"),
                            "application/json");
            return;
        }
        // Minimal JSON parsing
        auto extract_str = [&](const std::string& field) -> std::string {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return "";
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return "";
            auto q1 = req.body.find('"', colon + 1);
            if (q1 == std::string::npos) return "";
            auto q2 = req.body.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return req.body.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_int = [&](const std::string& field) -> int64_t {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return 0;
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return 0;
            try { return std::stoll(req.body.substr(colon + 1)); } catch (...) { return 0; }
        };

        std::string host = extract_str("host");
        auto id   = static_cast<uint32_t>(extract_int("id"));
        auto port = static_cast<uint16_t>(extract_int("port"));
        if (host.empty() || id == 0 || port == 0) {
            res.status = 400;
            res.set_content(build_error_json("Missing id, host, or port"), "application/json");
            return;
        }

        zeptodb::cluster::NodeAddress addr{host, port, id};
        coordinator_->add_remote_node(addr);
        res.set_content("{\"added\":true,\"id\":" + std::to_string(id) +
                        ",\"host\":\"" + host + "\",\"port\":" + std::to_string(port) + "}",
                        "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/table-placement — set explicit table placement policy
    // Body: {"table":"T","policy":"hash_by_table"|"pinned_node"|"clear","node_id":1}
    // -------------------------------------------------------------------------
    svr_->Post("/admin/table-placement", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!coordinator_) {
            res.status = 400;
            res.set_content(build_error_json("Cluster coordinator is not enabled"),
                            "application/json");
            return;
        }

        const std::string table = json_string_field(req.body, "table");
        const std::string policy = json_string_field(req.body, "policy");
        const int64_t node_id_raw = json_i64_field(req.body, "node_id", 0);
        if (table.empty()) {
            res.status = 400;
            res.set_content(build_error_json("table is required"),
                            "application/json");
            return;
        }
        if (policy.empty()) {
            res.status = 400;
            res.set_content(build_error_json("policy is required"),
                            "application/json");
            return;
        }

        bool ok = false;
        std::string error;
        if (policy == "clear") {
            ok = coordinator_->clear_table_placement(table, &error);
        } else {
            zeptodb::cluster::TablePlacementPolicy placement =
                zeptodb::cluster::TablePlacementPolicy::HashByTableAndSymbol;
            zeptodb::cluster::NodeId node_id =
                zeptodb::cluster::INVALID_NODE_ID;
            if (policy == "hash_by_table") {
                placement = zeptodb::cluster::TablePlacementPolicy::HashByTable;
            } else if (policy == "hash_by_table_and_symbol") {
                placement =
                    zeptodb::cluster::TablePlacementPolicy::HashByTableAndSymbol;
            } else if (policy == "pinned_node") {
                if (node_id_raw <= 0 ||
                    node_id_raw > std::numeric_limits<uint32_t>::max()) {
                    res.status = 400;
                    res.set_content(
                        build_error_json("pinned_node policy requires node_id"),
                        "application/json");
                    return;
                }
                placement = zeptodb::cluster::TablePlacementPolicy::PinnedNode;
                node_id = static_cast<zeptodb::cluster::NodeId>(node_id_raw);
            } else {
                res.status = 400;
                res.set_content(
                    build_error_json(
                        "policy must be hash_by_table, "
                        "hash_by_table_and_symbol, pinned_node, or clear"),
                    "application/json");
                return;
            }
            ok = coordinator_->set_table_placement(table, placement, node_id,
                                                   &error);
        }
        if (!ok) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "table placement update failed"
                                : error),
                            "application/json");
            return;
        }

        std::ostringstream os;
        os << "{\"ok\":true,\"table\":" << json_quote(table)
           << ",\"policy\":" << json_quote(policy);
        if (node_id_raw > 0) {
            os << ",\"node_id\":" << node_id_raw;
        }
        os << "}";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/edge-fleet-connector — experimental connector lifecycle status
    // -------------------------------------------------------------------------
    svr_->Get("/admin/edge-fleet-connector", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        res.set_content(
            edge_fleet_connector_status_json(edge_fleet_connector_runtime_.snapshot()),
            "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/edge-fleet-connector — configure and optionally enable
    // Body: {"name":"physical_ai_edge_fleet","enabled":true,
    //        "edge_outbox_table":"...","fleet_ack_table":"...",
    //        "checkpoint_path":"/var/lib/zeptodb/edge-fleet.checkpoint",
    //        "batch_limit":128,"max_inflight":128,
    //        "max_retries_per_event":1,"allow_late_events":true,
    //        "worker_enabled":false,"worker_poll_interval_ms":1000}
    // -------------------------------------------------------------------------
    svr_->Post("/admin/edge-fleet-connector", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;

        zeptodb::feeds::EdgeFleetConnectorRuntimeConfig config;
        config.name = json_string_field(req.body, "name", config.name);
        config.edge_outbox_table = json_string_field(
            req.body, "edge_outbox_table", config.edge_outbox_table);
        config.fleet_ack_table = json_string_field(
            req.body, "fleet_ack_table", config.fleet_ack_table);
        config.feed.checkpoint_path = json_string_field(req.body, "checkpoint_path");
        config.feed.allow_late_events = json_bool_field(
            req.body, "allow_late_events", config.feed.allow_late_events);
        config.worker_enabled = json_bool_field(
            req.body, "worker_enabled", config.worker_enabled);

        const int64_t batch_limit = json_i64_field(
            req.body, "batch_limit", static_cast<int64_t>(config.feed.batch_limit));
        const int64_t max_inflight = json_i64_field(
            req.body, "max_inflight", static_cast<int64_t>(config.feed.max_inflight));
        const int64_t max_retries = json_i64_field(
            req.body, "max_retries_per_event",
            static_cast<int64_t>(config.feed.max_retries_per_event));
        const int64_t worker_poll_interval_ms = json_i64_field(
            req.body,
            "worker_poll_interval_ms",
            static_cast<int64_t>(config.worker_poll_interval_ms));
        if (batch_limit <= 0 || max_inflight <= 0 || max_retries <= 0 ||
            worker_poll_interval_ms <= 0 ||
            max_retries > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            res.status = 400;
            res.set_content(
                build_error_json(
                    "batch_limit, max_inflight, max_retries_per_event, and worker_poll_interval_ms must be positive"),
                "application/json");
            return;
        }
        config.feed.batch_limit = static_cast<size_t>(batch_limit);
        config.feed.max_inflight = static_cast<size_t>(max_inflight);
        config.feed.max_retries_per_event = static_cast<uint32_t>(max_retries);
        config.worker_poll_interval_ms =
            static_cast<uint64_t>(worker_poll_interval_ms);

        std::string error;
        if (!edge_fleet_connector_runtime_.configure(std::move(config), &error)) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "edge/fleet connector configuration failed"
                                : error),
                            "application/json");
            return;
        }

        const bool enabled = json_bool_field(req.body, "enabled", true);
        if (enabled && !edge_fleet_connector_runtime_.start(&error)) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "edge/fleet connector start failed"
                                : error),
                            "application/json");
            return;
        }

        res.set_content(
            edge_fleet_connector_status_json(edge_fleet_connector_runtime_.snapshot()),
            "application/json");
    });

    // -------------------------------------------------------------------------
    // DELETE /admin/edge-fleet-connector — disable and clear experimental config
    // -------------------------------------------------------------------------
    svr_->Delete("/admin/edge-fleet-connector", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        std::string error;
        const auto before = edge_fleet_connector_runtime_.snapshot();
        if (before.enabled && !edge_fleet_connector_runtime_.stop(&error)) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "edge/fleet connector stop failed"
                                : error),
                            "application/json");
            return;
        }
        (void)edge_fleet_connector_runtime_.clear(&error);
        res.set_content(
            edge_fleet_connector_status_json(edge_fleet_connector_runtime_.snapshot()),
            "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/action-outcome-supervisor - experimental supervisor status
    // -------------------------------------------------------------------------
    svr_->Get("/admin/action-outcome-supervisor", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        res.set_content(
            action_outcome_supervisor_status_json(
                action_outcome_supervisor_runtime_.snapshot()),
            "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/action-outcome-supervisor - configure and optionally enable
    // Body: {"name":"physical_ai_action_outcome","enabled":true,
    //        "mode":"shadow","history_table":"...",
    //        "proposal_table":"...","decision_table":"...",
    //        "evidence_table":"...","fail_closed_action":"manual_review",
    //        "worker_enabled":false,"worker_poll_interval_ms":1000,
    //        "batch_limit":128,"max_consecutive_failures":3}
    // -------------------------------------------------------------------------
    svr_->Post("/admin/action-outcome-supervisor", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;

        zeptodb::feeds::ActionOutcomeSupervisorRuntimeConfig config;
        config.name = json_string_field(req.body, "name", config.name);
        config.mode = json_string_field(req.body, "mode", config.mode);
        config.history_table = json_string_field(
            req.body, "history_table", config.history_table);
        config.proposal_table = json_string_field(
            req.body, "proposal_table", config.proposal_table);
        config.decision_table = json_string_field(
            req.body, "decision_table", config.decision_table);
        config.evidence_table = json_string_field(
            req.body, "evidence_table", config.evidence_table);
        config.fail_closed_action = json_string_field(
            req.body, "fail_closed_action", config.fail_closed_action);
        config.worker_enabled = json_bool_field(
            req.body, "worker_enabled", config.worker_enabled);

        const int64_t batch_limit = json_i64_field(
            req.body, "batch_limit", static_cast<int64_t>(config.batch_limit));
        const int64_t worker_poll_interval_ms = json_i64_field(
            req.body,
            "worker_poll_interval_ms",
            static_cast<int64_t>(config.worker_poll_interval_ms));
        const int64_t max_consecutive_failures = json_i64_field(
            req.body,
            "max_consecutive_failures",
            static_cast<int64_t>(config.max_consecutive_failures));
        if (batch_limit <= 0 || worker_poll_interval_ms <= 0 ||
            max_consecutive_failures <= 0 ||
            max_consecutive_failures >
                static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            res.status = 400;
            res.set_content(
                build_error_json(
                    "batch_limit, worker_poll_interval_ms, and max_consecutive_failures must be positive"),
                "application/json");
            return;
        }
        config.batch_limit = static_cast<size_t>(batch_limit);
        config.worker_poll_interval_ms =
            static_cast<uint64_t>(worker_poll_interval_ms);
        config.max_consecutive_failures =
            static_cast<uint32_t>(max_consecutive_failures);

        std::string error;
        if (!action_outcome_supervisor_runtime_.configure(std::move(config), &error)) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "action-outcome supervisor configuration failed"
                                : error),
                            "application/json");
            return;
        }

        const bool enabled = json_bool_field(req.body, "enabled", true);
        if (enabled && !action_outcome_supervisor_runtime_.start(&error)) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "action-outcome supervisor start failed"
                                : error),
                            "application/json");
            return;
        }

        res.set_content(
            action_outcome_supervisor_status_json(
                action_outcome_supervisor_runtime_.snapshot()),
            "application/json");
    });

    // -------------------------------------------------------------------------
    // DELETE /admin/action-outcome-supervisor - disable and clear config
    // -------------------------------------------------------------------------
    svr_->Delete("/admin/action-outcome-supervisor", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        std::string error;
        const auto before = action_outcome_supervisor_runtime_.snapshot();
        if (before.enabled && !action_outcome_supervisor_runtime_.stop(&error)) {
            res.status = 400;
            res.set_content(build_error_json(error.empty()
                                ? "action-outcome supervisor stop failed"
                                : error),
                            "application/json");
            return;
        }
        (void)action_outcome_supervisor_runtime_.clear(&error);
        res.set_content(
            action_outcome_supervisor_status_json(
                action_outcome_supervisor_runtime_.snapshot()),
            "application/json");
    });

    // -------------------------------------------------------------------------
    // DELETE /admin/nodes/:id — remove a node from the cluster
    // -------------------------------------------------------------------------
    svr_->Delete(R"(/admin/nodes/(\d+))", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Multi-node cluster")) return;
        if (!coordinator_) {
            res.status = 400;
            res.set_content(build_error_json("Not in cluster mode"), "application/json");
            return;
        }
        uint32_t node_id = 0;
        try { node_id = static_cast<uint32_t>(std::stoul(std::string(req.matches[1]))); }
        catch (...) {
            res.status = 400;
            res.set_content(build_error_json("Invalid node ID"), "application/json");
            return;
        }
        coordinator_->remove_node(node_id);
        res.set_content("{\"removed\":true,\"id\":" + std::to_string(node_id) + "}",
                        "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/keys/:id/usage — API Key usage info
    // -------------------------------------------------------------------------
    svr_->Get(R"(/admin/keys/([^/]+)/usage)", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }
        std::string key_id = req.matches[1];
        auto keys = auth_->list_api_keys();
        std::optional<zeptodb::auth::ApiKeyEntry> target;
        for (const auto& k : keys) {
            if (k.id == key_id) { target = k; break; }
        }
        if (!target) {
            res.status = 404;
            res.set_content(build_error_json("Key not found"), "application/json");
            return;
        }
        std::ostringstream os;
        os << "{\"id\":\"" << target->id << "\","
           << "\"name\":\"" << target->name << "\","
           << "\"last_used_ns\":" << target->last_used_ns << ","
           << "\"allowed_symbols\":[";
        for (size_t i = 0; i < target->allowed_symbols.size(); ++i) {
            if (i > 0) os << ",";
            os << "\"" << target->allowed_symbols[i] << "\"";
        }
        os << "]}";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/tenants — list all tenants and their quotas
    // -------------------------------------------------------------------------
    svr_->Get("/admin/tenants", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::ADVANCED_RBAC, "Per-tenant rate limiting")) return;
        if (!tenant_mgr_) {
            res.set_content("[]", "application/json");
            return;
        }
        auto tenants = tenant_mgr_->list_tenants();
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < tenants.size(); ++i) {
            if (i > 0) os << ",";
            const auto& t = tenants[i];
            const auto* usage = tenant_mgr_->usage(t.tenant_id);
            os << "{\"tenant_id\":\"" << t.tenant_id << "\","
               << "\"name\":\"" << t.name << "\","
               << "\"table_namespace\":\"" << t.table_namespace << "\","
               << "\"max_concurrent_queries\":" << t.max_concurrent_queries << ",";
            if (usage) {
                os << "\"usage\":{\"active_queries\":" << usage->active_queries.load()
                   << ",\"total_queries\":" << usage->total_queries.load()
                   << ",\"rejected_queries\":" << usage->rejected_queries.load() << "}";
            } else {
                os << "\"usage\":null";
            }
            os << "}";
        }
        os << "]";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/tenants — create a new tenant
    // -------------------------------------------------------------------------
    svr_->Post("/admin/tenants", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::ADVANCED_RBAC, "Per-tenant rate limiting")) return;
        if (!tenant_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("Tenant manager not configured"), "application/json");
            return;
        }

        auto extract_str = [&](const std::string& field) -> std::string {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return "";
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return "";
            auto q1 = req.body.find('"', colon + 1);
            if (q1 == std::string::npos) return "";
            auto q2 = req.body.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return req.body.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_int = [&](const std::string& field) -> int64_t {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return 0;
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return 0;
            try { return std::stoll(req.body.substr(colon + 1)); } catch (...) { return 0; }
        };

        std::string tenant_id = extract_str("tenant_id");
        std::string name = extract_str("name");
        std::string ns = extract_str("table_namespace");
        uint32_t mcq = static_cast<uint32_t>(extract_int("max_concurrent_queries"));

        if (tenant_id.empty()) {
            res.status = 400;
            res.set_content(build_error_json("Missing 'tenant_id'"), "application/json");
            return;
        }

        zeptodb::auth::TenantConfig cfg;
        cfg.tenant_id = tenant_id;
        cfg.name = name.empty() ? tenant_id : name;
        cfg.table_namespace = ns;
        cfg.max_concurrent_queries = mcq;

        if (tenant_mgr_->create_tenant(cfg)) {
            res.set_content("{\"created\":true,\"tenant_id\":\"" + tenant_id + "\"}", "application/json");
        } else {
            res.status = 409;
            res.set_content(build_error_json("Tenant ID already exists"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // DELETE /admin/tenants/:id — remove a tenant
    // -------------------------------------------------------------------------
    svr_->Delete(R"(/admin/tenants/([^/]+))", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::ADVANCED_RBAC, "Per-tenant rate limiting")) return;
        if (!tenant_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("Tenant manager not configured"), "application/json");
            return;
        }
        std::string tid = req.matches[1];
        if (tenant_mgr_->drop_tenant(tid)) {
            res.set_content("{\"deleted\":true}", "application/json");
        } else {
            res.status = 404;
            res.set_content(build_error_json("Tenant not found"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // POST /admin/auth/reload — force refresh JWKS keys
    // -------------------------------------------------------------------------
    svr_->Post("/admin/auth/reload", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }
        bool ok = auth_->refresh_jwks();
        if (ok) {
            auto* p = auth_->jwks_provider();
            size_t n = p ? p->key_count() : 0;
            res.set_content("{\"refreshed\":true,\"keys_loaded\":" + std::to_string(n) + "}", "application/json");
        } else {
            res.status = 502;
            res.set_content(build_error_json("JWKS refresh failed (no JWKS URL configured or fetch error)"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // GET /admin/settings — server configuration
    // -------------------------------------------------------------------------
    svr_->Get("/admin/settings", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        std::ostringstream os;
        os << "{"
           << "\"port\":" << port_ << ","
           << "\"tls_enabled\":" << (tls_.enabled ? "true" : "false") << ","
           << "\"auth_enabled\":" << (auth_ ? "true" : "false") << ","
           << "\"query_timeout_ms\":" << query_timeout_ms_ << ","
           << "\"multi_tenancy_enabled\":" << (tenant_mgr_ ? "true" : "false") << ","
           << "\"cluster_mode\":" << (coordinator_ ? "true" : "false")
           << "}";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // SSO / OAuth2 / Session endpoints (no admin required)
    // -------------------------------------------------------------------------

    // GET /auth/login — redirect to IdP authorization endpoint
    svr_->Get("/auth/login", [this](
        const httplib::Request& /*req*/, httplib::Response& res)
    {
        if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::SSO)) {
            res.status = 402;
            res.set_content(build_402_json("SSO/OIDC authentication"), "application/json");
            return;
        }
        if (!auth_ || !auth_->oidc_metadata()) {
            res.status = 503;
            res.set_content(build_error_json("OIDC not configured"), "application/json");
            return;
        }
        const auto* meta = auth_->oidc_metadata();
        std::string state = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());

        std::string url = meta->authorization_endpoint
            + "?response_type=code"
            + "&client_id=" + auth_->oidc_client_id()
            + "&redirect_uri=" + auth_->oidc_redirect_uri()
            + "&scope=openid+email+profile"
            + "&state=" + state;
        res.set_redirect(url);
    });

    // GET /auth/callback — OAuth2 authorization code callback
    svr_->Get("/auth/callback", [this](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::SSO)) {
            res.status = 402;
            res.set_content(build_402_json("SSO/OIDC authentication"), "application/json");
            return;
        }
        if (!auth_ || !auth_->oidc_metadata()) {
            res.status = 503;
            res.set_content(build_error_json("OIDC not configured"), "application/json");
            return;
        }
        if (!req.has_param("code")) {
            res.status = 400;
            std::string error_desc = req.has_param("error_description")
                ? req.get_param_value("error_description") : "No authorization code";
            res.set_content(build_error_json(error_desc), "application/json");
            return;
        }

        const auto* meta = auth_->oidc_metadata();
        zeptodb::auth::OAuth2ExchangeParams params;
        params.token_endpoint = meta->token_endpoint;
        params.code           = req.get_param_value("code");
        params.redirect_uri   = auth_->oidc_redirect_uri();
        params.client_id      = auth_->oidc_client_id();
        params.client_secret  = auth_->oidc_client_secret();

        auto tokens = zeptodb::auth::OAuth2TokenExchange::exchange(params);
        if (!tokens) {
            res.status = 502;
            res.set_content(build_error_json("Token exchange failed"), "application/json");
            return;
        }

        // Resolve identity from id_token (or access_token)
        std::string jwt = !tokens->id_token.empty() ? tokens->id_token : tokens->access_token;
        auto decision = auth_->check("GET", "/auth/callback",
                                      "Bearer " + jwt, req.remote_addr);
        if (decision.status != zeptodb::auth::AuthStatus::OK) {
            res.status = 401;
            res.set_content(build_error_json("Identity resolution failed: " + decision.reason),
                            "application/json");
            return;
        }

        // Create server-side session
        auto* store = auth_->session_store();
        if (store) {
            auto sid = store->create(
                decision.context.subject, decision.context.name,
                decision.context.role, decision.context.source,
                decision.context.allowed_symbols, decision.context.tenant_id,
                tokens->refresh_token);
            res.set_header("Set-Cookie", store->make_cookie(sid));
            // Redirect to Web UI
            res.set_redirect("/query");
        } else {
            // No session store — return tokens as JSON (API mode)
            std::ostringstream os;
            os << "{\"access_token\":\"" << tokens->access_token << "\""
               << ",\"role\":\"" << zeptodb::auth::role_to_string(decision.context.role) << "\""
               << ",\"subject\":\"" << decision.context.subject << "\"";
            if (!tokens->refresh_token.empty())
                os << ",\"refresh_token\":\"" << tokens->refresh_token << "\"";
            if (tokens->expires_in > 0)
                os << ",\"expires_in\":" << tokens->expires_in;
            os << "}";
            res.set_content(os.str(), "application/json");
        }
    });

    // POST /auth/session — create session from Bearer token (JWT/API key login)
    svr_->Post("/auth/session", [this](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::SSO)) {
            res.status = 402;
            res.set_content(build_402_json("SSO/OIDC authentication"), "application/json");
            return;
        }
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }
        std::string auth_hdr;
        if (req.has_header("Authorization"))
            auth_hdr = req.get_header_value("Authorization");
        if (auth_hdr.empty()) {
            res.status = 401;
            res.set_content(build_error_json("Authorization header required"), "application/json");
            return;
        }

        auto decision = auth_->check("POST", "/auth/session", auth_hdr, req.remote_addr);
        if (decision.status != zeptodb::auth::AuthStatus::OK) {
            res.status = 401;
            res.set_content(build_error_json(decision.reason), "application/json");
            return;
        }

        auto* store = auth_->session_store();
        if (!store) {
            res.status = 503;
            res.set_content(build_error_json("Sessions not enabled"), "application/json");
            return;
        }

        auto sid = store->create(
            decision.context.subject, decision.context.name,
            decision.context.role, decision.context.source,
            decision.context.allowed_symbols, decision.context.tenant_id);
        res.set_header("Set-Cookie", store->make_cookie(sid));
        std::ostringstream os;
        os << "{\"session\":true"
           << ",\"role\":\"" << zeptodb::auth::role_to_string(decision.context.role) << "\""
           << ",\"subject\":\"" << decision.context.subject << "\""
           << "}";
        res.set_content(os.str(), "application/json");
    });

    // POST /auth/logout — destroy session
    svr_->Post("/auth/logout", [this](
        const httplib::Request& req, httplib::Response& res)
    {
        auto* store = auth_ ? auth_->session_store() : nullptr;
        if (!store) {
            res.set_content("{\"ok\":true}", "application/json");
            return;
        }

        // Extract session cookie
        if (req.has_header("Cookie")) {
            auto ctx = auth_->check_session(req.get_header_value("Cookie"));
            // Find and destroy the session
            const auto& cookie = req.get_header_value("Cookie");
            const auto& name = store->config().cookie_name;
            auto pos = cookie.find(name + "=");
            if (pos != std::string::npos) {
                auto val_start = pos + name.size() + 1;
                auto val_end = cookie.find(';', val_start);
                std::string sid = (val_end != std::string::npos)
                    ? cookie.substr(val_start, val_end - val_start)
                    : cookie.substr(val_start);
                while (!sid.empty() && sid.back() == ' ') sid.pop_back();
                store->destroy(sid);
            }
        }
        res.set_header("Set-Cookie", store->make_clear_cookie());
        res.set_content("{\"ok\":true}", "application/json");
    });

    // POST /auth/refresh — refresh session using stored refresh token
    svr_->Post("/auth/refresh", [this](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::SSO)) {
            res.status = 402;
            res.set_content(build_402_json("SSO/OIDC authentication"), "application/json");
            return;
        }
        auto* store = auth_ ? auth_->session_store() : nullptr;
        if (!store || !auth_->oidc_metadata()) {
            res.status = 503;
            res.set_content(build_error_json("Session refresh not available"), "application/json");
            return;
        }

        // Get current session from cookie
        std::string sid;
        if (req.has_header("Cookie")) {
            const auto& cookie = req.get_header_value("Cookie");
            const auto& name = store->config().cookie_name;
            auto pos = cookie.find(name + "=");
            if (pos != std::string::npos) {
                auto val_start = pos + name.size() + 1;
                auto val_end = cookie.find(';', val_start);
                sid = (val_end != std::string::npos)
                    ? cookie.substr(val_start, val_end - val_start)
                    : cookie.substr(val_start);
                while (!sid.empty() && sid.back() == ' ') sid.pop_back();
            }
        }

        auto session = store->get(sid);
        if (!session || session->refresh_token.empty()) {
            res.status = 401;
            res.set_content(build_error_json("No valid session or refresh token"), "application/json");
            return;
        }

        const auto* meta = auth_->oidc_metadata();
        auto tokens = zeptodb::auth::OAuth2TokenExchange::refresh(
            meta->token_endpoint,
            session->refresh_token,
            auth_->oidc_client_id(),
            auth_->oidc_client_secret());

        if (!tokens) {
            // Refresh failed — destroy session
            store->destroy(sid);
            res.set_header("Set-Cookie", store->make_clear_cookie());
            res.status = 401;
            res.set_content(build_error_json("Token refresh failed"), "application/json");
            return;
        }

        // Update refresh token if a new one was issued
        if (!tokens->refresh_token.empty()) {
            store->update_refresh_token(sid, tokens->refresh_token);
        }

        // Extend session cookie
        res.set_header("Set-Cookie", store->make_cookie(sid));
        std::ostringstream os;
        os << "{\"refreshed\":true"
           << ",\"expires_in\":" << tokens->expires_in
           << "}";
        res.set_content(os.str(), "application/json");
    });

    // GET /auth/me — return current session info (from cookie or Bearer)
    svr_->Get("/auth/me", [this](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!auth_) {
            res.status = 503;
            res.set_content(build_error_json("Auth not configured"), "application/json");
            return;
        }

        // Try session cookie first
        if (req.has_header("Cookie")) {
            auto ctx = auth_->check_session(req.get_header_value("Cookie"));
            if (ctx) {
                std::ostringstream os;
                os << "{\"subject\":\"" << ctx->subject << "\""
                   << ",\"role\":\"" << zeptodb::auth::role_to_string(ctx->role) << "\""
                   << ",\"source\":\"" << ctx->source << "\""
                   << "}";
                res.set_content(os.str(), "application/json");
                return;
            }
        }

        // Fall back to Bearer token
        std::string auth_hdr;
        if (req.has_header("Authorization"))
            auth_hdr = req.get_header_value("Authorization");
        if (auth_hdr.empty()) {
            res.status = 401;
            res.set_content(build_error_json("Not authenticated"), "application/json");
            return;
        }
        auto decision = auth_->check("GET", "/auth/me", auth_hdr, req.remote_addr);
        if (decision.status != zeptodb::auth::AuthStatus::OK) {
            res.status = 401;
            res.set_content(build_error_json(decision.reason), "application/json");
            return;
        }
        std::ostringstream os;
        os << "{\"subject\":\"" << decision.context.subject << "\""
           << ",\"role\":\"" << zeptodb::auth::role_to_string(decision.context.role) << "\""
           << ",\"source\":\"" << decision.context.source << "\""
           << "}";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // Rebalance Admin API
    // -------------------------------------------------------------------------

    // GET /admin/rebalance/status — current rebalance status
    svr_->Get("/admin/rebalance/status", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Live rebalancing")) return;
        if (!rebalance_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("rebalance not available"), "application/json");
            return;
        }
        auto s = rebalance_mgr_->status();
        const char* state_str = "IDLE";
        switch (s.state) {
            case zeptodb::cluster::RebalanceState::RUNNING:    state_str = "RUNNING"; break;
            case zeptodb::cluster::RebalanceState::PAUSED:     state_str = "PAUSED"; break;
            case zeptodb::cluster::RebalanceState::CANCELLING: state_str = "CANCELLING"; break;
            default: break;
        }
        std::ostringstream os;
        os << "{\"state\":\"" << state_str << "\""
           << ",\"total_moves\":" << s.total_moves
           << ",\"completed_moves\":" << s.completed_moves
           << ",\"failed_moves\":" << s.failed_moves
           << ",\"current_symbol\":\"" << s.current_symbol << "\""
           << ",\"max_bandwidth_mbps\":" << rebalance_mgr_->config().max_bandwidth_mbps
           << "}";
        res.set_content(os.str(), "application/json");
    });

    // POST /admin/rebalance/start — start rebalance
    svr_->Post("/admin/rebalance/start", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Live rebalancing")) return;
        if (!rebalance_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("rebalance not available"), "application/json");
            return;
        }
        // Parse action and node_id from body
        auto extract_str = [&](const std::string& field) -> std::string {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return "";
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return "";
            auto q1 = req.body.find('"', colon + 1);
            if (q1 == std::string::npos) return "";
            auto q2 = req.body.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return req.body.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_int = [&](const std::string& field) -> int64_t {
            std::string pat = "\"" + field + "\"";
            auto pos = req.body.find(pat);
            if (pos == std::string::npos) return 0;
            auto colon = req.body.find(':', pos + pat.size());
            if (colon == std::string::npos) return 0;
            try { return std::stoll(req.body.substr(colon + 1)); } catch (...) { return 0; }
        };

        std::string action = extract_str("action");
        auto node_id = static_cast<uint32_t>(extract_int("node_id"));

        bool ok = false;
        if (action == "add_node") {
            if (node_id == 0) {
                res.status = 400;
                res.set_content(build_error_json("Missing or invalid node_id"), "application/json");
                return;
            }
            ok = rebalance_mgr_->start_add_node(node_id);
        } else if (action == "remove_node") {
            if (node_id == 0) {
                res.status = 400;
                res.set_content(build_error_json("Missing or invalid node_id"), "application/json");
                return;
            }
            ok = rebalance_mgr_->start_remove_node(node_id);
        } else if (action == "move_partitions") {
            // Parse moves array: [{"symbol":N,"from":N,"to":N}, ...]
            auto moves_pos = req.body.find("\"moves\"");
            auto arr_start = (moves_pos != std::string::npos) ? req.body.find('[', moves_pos) : std::string::npos;
            auto arr_end = (arr_start != std::string::npos) ? req.body.find(']', arr_start) : std::string::npos;
            if (arr_start == std::string::npos || arr_end == std::string::npos) {
                res.status = 400;
                res.set_content(build_error_json("Missing 'moves' array"), "application/json");
                return;
            }
            std::vector<cluster::PartitionRouter::Move> moves;
            auto cur = arr_start;
            while (cur < arr_end) {
                auto obj_start = req.body.find('{', cur);
                if (obj_start == std::string::npos || obj_start >= arr_end) break;
                auto obj_end = req.body.find('}', obj_start);
                if (obj_end == std::string::npos || obj_end > arr_end) break;
                auto obj = req.body.substr(obj_start, obj_end - obj_start + 1);
                auto find_val = [&](const std::string& key) -> int64_t {
                    auto p = obj.find("\"" + key + "\"");
                    if (p == std::string::npos) return -1;
                    auto c = obj.find(':', p);
                    if (c == std::string::npos) return -1;
                    try { return std::stoll(obj.substr(c + 1)); } catch (...) { return -1; }
                };
                auto sym = find_val("symbol");
                auto from = find_val("from");
                auto to = find_val("to");
                if (sym < 0 || from < 0 || to < 0) {
                    res.status = 400;
                    res.set_content(build_error_json("Invalid move object"), "application/json");
                    return;
                }
                if (from == to) {
                    res.status = 400;
                    res.set_content(build_error_json("Invalid move: 'from' and 'to' must differ"), "application/json");
                    return;
                }
                moves.push_back({static_cast<uint32_t>(sym), static_cast<uint32_t>(from), static_cast<uint32_t>(to)});
                cur = obj_end + 1;
            }
            if (moves.empty()) {
                res.status = 400;
                res.set_content(build_error_json("Empty moves array"), "application/json");
                return;
            }
            ok = rebalance_mgr_->start_move_partitions(std::move(moves));
        } else {
            res.status = 400;
            res.set_content(build_error_json("Invalid action: use 'add_node', 'remove_node', or 'move_partitions'"),
                            "application/json");
            return;
        }

        if (ok) {
            res.set_content(R"({"ok":true})", "application/json");
        } else {
            res.set_content(R"({"ok":false,"error":"already running"})", "application/json");
        }
    });

    // POST /admin/rebalance/pause
    svr_->Post("/admin/rebalance/pause", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Live rebalancing")) return;
        if (!rebalance_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("rebalance not available"), "application/json");
            return;
        }
        rebalance_mgr_->pause();
        res.set_content(R"({"ok":true})", "application/json");
    });

    // POST /admin/rebalance/resume
    svr_->Post("/admin/rebalance/resume", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Live rebalancing")) return;
        if (!rebalance_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("rebalance not available"), "application/json");
            return;
        }
        rebalance_mgr_->resume();
        res.set_content(R"({"ok":true})", "application/json");
    });

    // POST /admin/rebalance/cancel
    svr_->Post("/admin/rebalance/cancel", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Live rebalancing")) return;
        if (!rebalance_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("rebalance not available"), "application/json");
            return;
        }
        rebalance_mgr_->cancel();
        res.set_content(R"({"ok":true})", "application/json");
    });

    // GET /admin/rebalance/history — past rebalance events
    svr_->Get("/admin/rebalance/history", [this, require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::CLUSTER, "Live rebalancing")) return;
        if (!rebalance_mgr_) {
            res.status = 503;
            res.set_content(build_error_json("rebalance not available"), "application/json");
            return;
        }
        auto entries = rebalance_mgr_->history();
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < entries.size(); ++i) {
            auto& e = entries[i];
            const char* act = "move_partitions";
            if (e.action == zeptodb::cluster::RebalanceAction::ADD_NODE) act = "add_node";
            else if (e.action == zeptodb::cluster::RebalanceAction::REMOVE_NODE) act = "remove_node";
            if (i > 0) os << ",";
            os << "{\"action\":\"" << act << "\""
               << ",\"node_id\":" << e.node_id
               << ",\"total_moves\":" << e.total_moves
               << ",\"completed_moves\":" << e.completed_moves
               << ",\"failed_moves\":" << e.failed_moves
               << ",\"start_time_ms\":" << e.start_time_ms
               << ",\"duration_ms\":" << e.duration_ms
               << ",\"cancelled\":" << (e.cancelled ? "true" : "false")
               << "}";
        }
        os << "]";
        res.set_content(os.str(), "application/json");
    });

    // -------------------------------------------------------------------------
    // POST /admin/upgrade/start — rolling upgrade (placeholder, gated)
    // -------------------------------------------------------------------------
    svr_->Post("/admin/upgrade/start", [require_admin, require_feature](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!require_feature(res, zeptodb::auth::Feature::ROLLING_UPGRADE, "Rolling upgrade")) return;
        res.status = 501;
        res.set_content(R"({"error":"Rolling upgrade not yet implemented"})", "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /admin/clock — PTP clock sync status
    // -------------------------------------------------------------------------
    svr_->Get("/admin/clock", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        if (!ptp_detector_) {
            res.status = 503;
            res.set_content(build_error_json("PTP detector not configured"), "application/json");
            return;
        }
        ptp_detector_->refresh();
        res.set_content(ptp_detector_->to_json(), "application/json");
    });

    // -------------------------------------------------------------------------
    // Metrics collector — start background capture (3s interval, 1h buffer)
    // -------------------------------------------------------------------------
    metrics_collector_ = std::make_unique<MetricsCollector>(executor_.stats());
    metrics_collector_->start();

    // -------------------------------------------------------------------------
    // GET /admin/metrics/history — time-series metrics JSON
    // Standalone: local collector. Cluster: merge from all nodes.
    // Query params: ?since=<epoch_ms>  (optional, default = all)
    //               ?limit=<N>         (optional, default = config response_limit)
    // -------------------------------------------------------------------------
    svr_->Get("/admin/metrics/history", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        int64_t since_ms = 0;
        size_t limit = 0;
        if (req.has_param("since")) {
            since_ms = std::stoll(req.get_param_value("since"));
        }
        if (req.has_param("limit")) {
            limit = static_cast<size_t>(std::stoll(req.get_param_value("limit")));
        }

        if (coordinator_) {
            // Cluster mode: merge metrics from all nodes
            auto all_json = coordinator_scatter_metrics(since_ms, limit);
            res.set_content(all_json, "application/json");
        } else {
            // Standalone mode: local collector only
            auto snaps = metrics_collector_->get_history(since_ms, limit);
            res.set_content(MetricsCollector::to_json(snaps), "application/json");
        }
    });
}

// ============================================================================
// set_web_dir — serve static files (Web UI)
// ============================================================================
void HttpServer::set_web_dir(const std::string& dir) {
    if (!std::filesystem::is_directory(dir)) {
        zeptodb::util::Logger::instance().warn(
            "{\"event\":\"web_dir_not_found\",\"path\":\"" + dir + "\"}", "http");
        return;
    }
    svr_->set_mount_point("/ui", dir);
    svr_->Get("/", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_redirect("/ui/");
    });
    zeptodb::util::Logger::instance().info(
        "{\"event\":\"web_ui_enabled\",\"path\":\"" + dir + "\"}", "http");
}

// ============================================================================
// start() — blocking
// ============================================================================
void HttpServer::start() {
    zeptodb::util::Logger::instance().info(
        "{\"event\":\"server_start\",\"port\":" + std::to_string(port_)
        + ",\"tls\":" + (tls_.enabled ? "true" : "false")
        + ",\"auth\":" + (auth_ ? "true" : "false") + "}", "http");
    running_.store(true);
    svr_->listen("0.0.0.0", static_cast<int>(port_));
    running_.store(false);
}

// ============================================================================
// start_async() — background thread
// ============================================================================
void HttpServer::start_async() {
    zeptodb::util::Logger::instance().info(
        "{\"event\":\"server_start\",\"port\":" + std::to_string(port_)
        + ",\"tls\":" + (tls_.enabled ? "true" : "false")
        + ",\"auth\":" + (auth_ ? "true" : "false")
        + ",\"async\":true}", "http");
    running_.store(true);
    thread_ = std::thread([this]() {
        svr_->listen("0.0.0.0", static_cast<int>(port_));
        running_.store(false);
    });
    // Wait briefly for the server to start listening
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// Cluster helpers: scatter stats/metrics to all nodes via RPC
// ============================================================================

std::vector<std::string> HttpServer::coordinator_scatter_stats() {
    if (!coordinator_) return {};

    // Local node stats
    const auto& stats = executor_.stats();
    std::ostringstream local;
    local << "{\"id\":0"
          << ",\"host\":\"localhost\""
          << ",\"port\":" << port_
          << ",\"state\":\"ACTIVE\""
          << ",\"ticks_ingested\":" << stats.ticks_ingested.load()
          << ",\"ticks_stored\":" << stats.ticks_stored.load()
          << ",\"queries_executed\":" << stats.queries_executed.load()
          << "}";

    std::vector<std::string> results;
    results.push_back(local.str());

    // Remote nodes via RPC (parallel)
    auto remote = coordinator_->collect_remote_stats();
    for (auto& r : remote) results.push_back(r);

    return results;
}

std::string HttpServer::coordinator_scatter_metrics(int64_t since_ms, size_t limit) {
    // Local metrics
    auto local_snaps = metrics_collector_->get_history(since_ms, limit);

    if (!coordinator_ || coordinator_->node_count() <= 1) {
        return MetricsCollector::to_json(local_snaps);
    }

    // Build merged JSON array: local entries + remote entries
    std::string out = "[";
    bool has_entry = false;

    // Local entries
    for (auto& s : local_snaps) {
        if (has_entry) out += ",";
        out += "{\"timestamp_ms\":" + std::to_string(s.timestamp_ms)
            + ",\"node_id\":" + std::to_string(s.node_id)
            + ",\"ticks_ingested\":" + std::to_string(s.ticks_ingested)
            + ",\"ticks_stored\":" + std::to_string(s.ticks_stored)
            + ",\"ticks_dropped\":" + std::to_string(s.ticks_dropped)
            + ",\"queries_executed\":" + std::to_string(s.queries_executed)
            + ",\"total_rows_scanned\":" + std::to_string(s.total_rows_scanned)
            + ",\"partitions_created\":" + std::to_string(s.partitions_created)
            + ",\"last_ingest_latency_ns\":" + std::to_string(s.last_ingest_latency_ns)
            + "}";
        has_entry = true;
    }

    // Remote entries via METRICS_REQUEST RPC (parallel fan-out)
    auto remote = coordinator_->collect_remote_metrics(since_ms,
                      static_cast<uint32_t>(limit));
    for (auto& r : remote) {
        // Each r is a JSON array "[{...},{...}]"; strip outer [] and append
        if (r.size() > 2 && r.front() == '[' && r.back() == ']') {
            auto inner = r.substr(1, r.size() - 2);
            if (!inner.empty()) {
                if (has_entry) out += ",";
                out += inner;
                has_entry = true;
            }
        }
    }
    out += "]";
    return out;
}

// ============================================================================
// stop()
// ============================================================================
void HttpServer::stop() {
    zeptodb::util::Logger::instance().info(
        "{\"event\":\"server_stop\",\"port\":" + std::to_string(port_) + "}", "http");
    std::string persist_error;
    persist_agent_memory_snapshot_(&persist_error, true);
    if (!persist_error.empty()) {
        zeptodb::util::Logger::instance().error(
            "{\"event\":\"agent_memory_snapshot_failed\",\"error\":\"" +
            json_escape(persist_error) + "\"}", "http");
    }
    if (metrics_collector_) metrics_collector_->stop();
    svr_->stop();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

// ============================================================================
// JSON response builder
// ============================================================================
std::string HttpServer::build_json_response(
    const zeptodb::sql::QueryResultSet& result)
{
    std::ostringstream os;
    os << "{";

    // columns 배열
    os << "\"columns\":[";
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (i > 0) os << ",";
        os << "\"" << result.column_names[i] << "\"";
    }
    os << "],";

    // data 배열 (2D)
    // Precompute which columns are string (symbol dict lookup)
    std::vector<bool> is_str_col(result.column_names.size(), false);
    for (size_t c = 0; c < result.column_names.size(); ++c) {
        if (result.column_names[c] == "symbol" && result.symbol_dict)
            is_str_col[c] = true;
        if (c < result.column_types.size() &&
            result.column_types[c] == storage::ColumnType::STRING)
            is_str_col[c] = true;
    }

    os << "\"data\":[";

    // ── String-only path (EXPLAIN, DDL messages) ──
    if (!result.string_rows.empty() && result.rows.empty()) {
        for (size_t i = 0; i < result.string_rows.size(); ++i) {
            if (i > 0) os << ",";
            os << "[\"";
            for (char ch : result.string_rows[i]) {
                if (ch == '"') os << "\\\"";
                else if (ch == '\\') os << "\\\\";
                else os << ch;
            }
            os << "\"]";
        }
    }
    // ── String-result path (SHOW TABLES / DESCRIBE) ──
    // When string_rows is populated and rows exist, interleave string values
    // with integer values based on column_types (SYMBOL/STRING → string).
    if (!result.string_rows.empty() && !result.rows.empty()) {
        size_t str_idx = 0;
        for (size_t r = 0; r < result.rows.size(); ++r) {
            if (r > 0) os << ",";
            os << "[";
            size_t int_idx = 0;
            for (size_t c = 0; c < result.column_names.size(); ++c) {
                if (c > 0) os << ",";
                if (c < result.column_types.size() &&
                    (result.column_types[c] == storage::ColumnType::SYMBOL ||
                     result.column_types[c] == storage::ColumnType::STRING) &&
                    str_idx < result.string_rows.size()) {
                    // JSON-escape the string
                    os << "\"";
                    for (char ch : result.string_rows[str_idx]) {
                        if (ch == '"') os << "\\\"";
                        else if (ch == '\\') os << "\\\\";
                        else os << ch;
                    }
                    os << "\"";
                    ++str_idx;
                } else if (int_idx < result.rows[r].size()) {
                    os << result.rows[r][int_idx++];
                } else {
                    os << "0";
                }
            }
            os << "]";
        }
    } else {
    // ── Normal numeric path ──
    for (size_t r = 0; r < result.rows.size(); ++r) {
        if (r > 0) os << ",";
        os << "[";
        if (r < result.typed_rows.size()) {
            const auto& trow = result.typed_rows[r];
            for (size_t c = 0; c < trow.size(); ++c) {
                if (c > 0) os << ",";
                if (is_str_col[c] && result.symbol_dict) {
                    os << "\"" << result.symbol_dict->lookup(
                        static_cast<uint32_t>(trow[c].i)) << "\"";
                } else if (c < result.column_types.size() &&
                    (result.column_types[c] == storage::ColumnType::FLOAT64 ||
                     result.column_types[c] == storage::ColumnType::FLOAT32))
                    os << trow[c].f;
                else
                    os << trow[c].i;
            }
        } else {
            const auto& row = result.rows[r];
            for (size_t c = 0; c < row.size(); ++c) {
                if (c > 0) os << ",";
                if (is_str_col[c] && result.symbol_dict)
                    os << "\"" << result.symbol_dict->lookup(
                        static_cast<uint32_t>(row[c])) << "\"";
                else
                    os << row[c];
            }
        }
        os << "]";
    }
    } // end else (normal numeric path)
    os << "],";

    // 메타데이터
    size_t row_count = result.rows.size();
    if (result.rows.empty() && !result.string_rows.empty())
        row_count = result.string_rows.size();
    os << "\"rows\":" << row_count << ",";
    os << "\"rows_scanned\":" << result.rows_scanned << ",";

    // 소수점 2자리로 제한
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", result.execution_time_us);
    os << "\"execution_time_us\":" << buf;

    os << "}";
    return os.str();
}

// ============================================================================
// Error JSON
// ============================================================================
std::string HttpServer::build_error_json(const std::string& msg) {
    // 간단한 JSON 이스케이프 (따옴표 처리)
    std::string escaped;
    for (char c : msg) {
        if (c == '"')       escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else                escaped += c;
    }
    return R"({"error":")" + escaped + R"("})";
}

// ============================================================================
// Stats JSON
// ============================================================================
std::string HttpServer::build_stats_json(
    const zeptodb::core::PipelineStats& stats)
{
    std::ostringstream os;
    os << "{"
       << "\"ticks_ingested\":"   << stats.ticks_ingested.load()   << ","
       << "\"ticks_stored\":"     << stats.ticks_stored.load()     << ","
       << "\"ticks_dropped\":"    << stats.ticks_dropped.load()    << ","
       << "\"queries_executed\":" << stats.queries_executed.load() << ","
       << "\"total_rows_scanned\":" << stats.total_rows_scanned.load() << ","
       << "\"partitions_created\":" << stats.partitions_created.load() << ","
       << "\"last_ingest_latency_ns\":" << stats.last_ingest_latency_ns.load()
       << "}";
    return os.str();
}

// ============================================================================
// Prometheus metrics (OpenMetrics format)
// ============================================================================
std::string HttpServer::build_prometheus_metrics() const {
    const auto& stats = executor_.stats();
    std::ostringstream os;

    os << "# HELP zepto_ticks_ingested_total Total number of ticks ingested\n";
    os << "# TYPE zepto_ticks_ingested_total counter\n";
    os << "zepto_ticks_ingested_total " << stats.ticks_ingested.load() << "\n\n";

    // Instantaneous ingest rate (P8-I4, devlog 117) — used as the HPA Pods
    // metric `zepto_ingest_ticks_per_sec` so Kubernetes autoscales on real
    // ingest load instead of CPU/memory proxies. Computed from the last two
    // MetricsCollector snapshots; emits 0.00 if the collector is absent or
    // hasn't captured at least two snapshots yet.
    os << "# HELP zepto_ingest_ticks_per_sec Instantaneous ingest rate (ticks/sec), computed from last two metrics snapshots\n";
    os << "# TYPE zepto_ingest_ticks_per_sec gauge\n";
    {
        double rate = metrics_collector_ ? metrics_collector_->ingest_ticks_per_sec() : 0.0;
        os << "zepto_ingest_ticks_per_sec "
           << std::fixed << std::setprecision(2) << rate
           << std::defaultfloat << "\n\n";
    }

    os << "# HELP zepto_ticks_stored_total Total number of ticks stored\n";
    os << "# TYPE zepto_ticks_stored_total counter\n";
    os << "zepto_ticks_stored_total " << stats.ticks_stored.load() << "\n\n";

    os << "# HELP zepto_ticks_dropped_total Total number of ticks dropped\n";
    os << "# TYPE zepto_ticks_dropped_total counter\n";
    os << "zepto_ticks_dropped_total " << stats.ticks_dropped.load() << "\n\n";

    os << "# HELP zepto_queries_executed_total Total number of queries executed\n";
    os << "# TYPE zepto_queries_executed_total counter\n";
    os << "zepto_queries_executed_total " << stats.queries_executed.load() << "\n\n";

    if (coordinator_) {
        const auto join_stats = coordinator_->small_table_join_stats();
        os << "# HELP zepto_small_table_join_candidates_total Bounded small-table JOIN candidates seen by the coordinator\n";
        os << "# TYPE zepto_small_table_join_candidates_total counter\n";
        os << "zepto_small_table_join_candidates_total "
           << join_stats.candidates << "\n\n";

        os << "# HELP zepto_small_table_join_accepted_total Bounded small-table JOINs accepted and executed by the coordinator\n";
        os << "# TYPE zepto_small_table_join_accepted_total counter\n";
        os << "zepto_small_table_join_accepted_total "
           << join_stats.accepted << "\n\n";

        os << "# HELP zepto_small_table_join_row_cap_rejections_total Bounded small-table JOINs rejected because a side exceeded the row cap\n";
        os << "# TYPE zepto_small_table_join_row_cap_rejections_total counter\n";
        os << "zepto_small_table_join_row_cap_rejections_total "
           << join_stats.rejected_row_cap << "\n\n";

        os << "# HELP zepto_small_table_join_errors_total Bounded small-table JOIN errors outside row-cap rejection\n";
        os << "# TYPE zepto_small_table_join_errors_total counter\n";
        os << "zepto_small_table_join_errors_total "
           << join_stats.errors << "\n\n";

        os << "# HELP zepto_small_table_join_rows_materialized_total Rows materialized into coordinator-local temporary tables for bounded small-table JOINs\n";
        os << "# TYPE zepto_small_table_join_rows_materialized_total counter\n";
        os << "zepto_small_table_join_rows_materialized_total "
           << join_stats.rows_materialized << "\n\n";

        os << "# HELP zepto_small_table_join_last_left_rows Last bounded small-table JOIN left-side row count\n";
        os << "# TYPE zepto_small_table_join_last_left_rows gauge\n";
        os << "zepto_small_table_join_last_left_rows "
           << join_stats.last_left_rows << "\n\n";

        os << "# HELP zepto_small_table_join_last_right_rows Last bounded small-table JOIN right-side row count\n";
        os << "# TYPE zepto_small_table_join_last_right_rows gauge\n";
        os << "zepto_small_table_join_last_right_rows "
           << join_stats.last_right_rows << "\n\n";
    }

    os << "# HELP zepto_rows_scanned_total Total number of rows scanned\n";
    os << "# TYPE zepto_rows_scanned_total counter\n";
    os << "zepto_rows_scanned_total " << stats.total_rows_scanned.load() << "\n\n";

    os << "# HELP zepto_server_up Server is up and running\n";
    os << "# TYPE zepto_server_up gauge\n";
    os << "zepto_server_up " << (running_.load() ? "1" : "0") << "\n\n";

    os << "# HELP zepto_server_ready Server is ready to accept queries\n";
    os << "# TYPE zepto_server_ready gauge\n";
    os << "zepto_server_ready " << (ready_.load() ? "1" : "0") << "\n\n";

    os << "# HELP zepto_http_requests_total Total HTTP requests served\n";
    os << "# TYPE zepto_http_requests_total counter\n";
    os << "zepto_http_requests_total " << g_request_seq.load(std::memory_order_relaxed) << "\n\n";

    os << "# HELP zepto_http_active_sessions Current active HTTP sessions\n";
    os << "# TYPE zepto_http_active_sessions gauge\n";
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        os << "zepto_http_active_sessions " << sessions_.size() << "\n";
    }

    {
        const auto memory_stats = agent_memory_->stats();
        const auto eviction = agent_memory_->eviction_config();
        os << "\n# HELP zepto_agent_memory_records Current Agent Memory record count\n";
        os << "# TYPE zepto_agent_memory_records gauge\n";
        os << "zepto_agent_memory_records " << memory_stats.memory_count << "\n\n";

        os << "# HELP zepto_agent_cache_entries Current Agent Cache entry count\n";
        os << "# TYPE zepto_agent_cache_entries gauge\n";
        os << "zepto_agent_cache_entries " << memory_stats.cache_count << "\n\n";

        os << "# HELP zepto_agent_memory_embedding_dim Agent Memory embedding dimension, or 0 before first embedding\n";
        os << "# TYPE zepto_agent_memory_embedding_dim gauge\n";
        os << "zepto_agent_memory_embedding_dim " << memory_stats.embedding_dim << "\n\n";

        os << "# HELP zepto_agent_memory_evictions_total Total Agent Memory records evicted by TTL or capacity policy\n";
        os << "# TYPE zepto_agent_memory_evictions_total counter\n";
        os << "zepto_agent_memory_evictions_total "
           << memory_stats.evicted_memory_count << "\n\n";

        os << "# HELP zepto_agent_cache_evictions_total Total Agent Cache entries evicted by TTL or capacity policy\n";
        os << "# TYPE zepto_agent_cache_evictions_total counter\n";
        os << "zepto_agent_cache_evictions_total "
           << memory_stats.evicted_cache_count << "\n\n";

        os << "# HELP zepto_agent_memory_max_records Configured Agent Memory capacity limit, 0 means unbounded\n";
        os << "# TYPE zepto_agent_memory_max_records gauge\n";
        os << "zepto_agent_memory_max_records " << eviction.max_memories << "\n\n";

        os << "# HELP zepto_agent_cache_max_entries Configured Agent Cache capacity limit, 0 means unbounded\n";
        os << "# TYPE zepto_agent_cache_max_entries gauge\n";
        os << "zepto_agent_cache_max_entries " << eviction.max_cache_entries << "\n";

        os << "\n# HELP zepto_agent_memory_tenant_quotas Configured Agent Memory tenant quota count\n";
        os << "# TYPE zepto_agent_memory_tenant_quotas gauge\n";
        os << "zepto_agent_memory_tenant_quotas "
           << eviction.tenant_quotas.size() << "\n";

        os << "\n# HELP zepto_agent_memory_snapshot_latency_seconds Last Agent Memory snapshot attempt duration in seconds\n";
        os << "# TYPE zepto_agent_memory_snapshot_latency_seconds gauge\n";
        os << "zepto_agent_memory_snapshot_latency_seconds "
           << memory_stats.snapshot_latency_seconds << "\n\n";

        os << "# HELP zepto_agent_memory_snapshot_failures_total Total failed Agent Memory snapshot attempts\n";
        os << "# TYPE zepto_agent_memory_snapshot_failures_total counter\n";
        os << "zepto_agent_memory_snapshot_failures_total "
           << memory_stats.snapshot_failures_total << "\n";

        os << "\n# HELP zepto_agent_memory_snapshot_records_bytes Last Agent Memory records sidecar size in bytes\n";
        os << "# TYPE zepto_agent_memory_snapshot_records_bytes gauge\n";
        os << "zepto_agent_memory_snapshot_records_bytes "
           << memory_stats.snapshot_records_bytes << "\n\n";

        os << "# HELP zepto_agent_memory_snapshot_vectors_bytes Last Agent Memory vectors sidecar size in bytes\n";
        os << "# TYPE zepto_agent_memory_snapshot_vectors_bytes gauge\n";
        os << "zepto_agent_memory_snapshot_vectors_bytes "
           << memory_stats.snapshot_vectors_bytes << "\n\n";

        os << "# HELP zepto_agent_memory_snapshot_total_bytes Last Agent Memory sidecar total size in bytes\n";
        os << "# TYPE zepto_agent_memory_snapshot_total_bytes gauge\n";
        os << "zepto_agent_memory_snapshot_total_bytes "
           << memory_stats.snapshot_total_bytes << "\n";

        os << "\n# HELP zepto_agent_memory_ann_indexed_vectors Agent Memory vectors indexed by ANN\n";
        os << "# TYPE zepto_agent_memory_ann_indexed_vectors gauge\n";
        os << "zepto_agent_memory_ann_indexed_vectors "
           << memory_stats.ann_indexed_vectors << "\n\n";

        os << "# HELP zepto_agent_memory_ann_memory_bytes Estimated Agent Memory ANN index bytes\n";
        os << "# TYPE zepto_agent_memory_ann_memory_bytes gauge\n";
        os << "zepto_agent_memory_ann_memory_bytes "
           << memory_stats.ann_memory_bytes << "\n\n";

        os << "# HELP zepto_agent_memory_ann_tombstone_entries Agent Memory ANN inactive tombstone entries\n";
        os << "# TYPE zepto_agent_memory_ann_tombstone_entries gauge\n";
        os << "zepto_agent_memory_ann_tombstone_entries "
           << memory_stats.ann_tombstone_entries << "\n\n";

        os << "# HELP zepto_agent_memory_ann_rebuilds_total Agent Memory ANN index rebuild count\n";
        os << "# TYPE zepto_agent_memory_ann_rebuilds_total counter\n";
        os << "zepto_agent_memory_ann_rebuilds_total "
           << memory_stats.ann_rebuild_count << "\n\n";

        os << "# HELP zepto_agent_memory_ann_last_rebuild_ms Last Agent Memory ANN rebuild duration in ms\n";
        os << "# TYPE zepto_agent_memory_ann_last_rebuild_ms gauge\n";
        os << "zepto_agent_memory_ann_last_rebuild_ms "
           << memory_stats.ann_last_rebuild_ms << "\n\n";

        os << "# HELP zepto_agent_memory_ann_searches_total Agent Memory searches served from ANN candidates\n";
        os << "# TYPE zepto_agent_memory_ann_searches_total counter\n";
        os << "zepto_agent_memory_ann_searches_total "
           << memory_stats.ann_search_count << "\n\n";

        os << "# HELP zepto_agent_memory_ann_fallbacks_total Agent Memory ANN searches that fell back to filtered scan\n";
        os << "# TYPE zepto_agent_memory_ann_fallbacks_total counter\n";
        os << "zepto_agent_memory_ann_fallbacks_total "
           << memory_stats.ann_fallback_count << "\n";
    }

    os << "\n" << edge_fleet_connector_runtime_.formatPrometheus();
    os << "\n" << action_outcome_supervisor_runtime_.formatPrometheus();

    // Append output from registered metrics providers (e.g. Kafka consumers).
    {
        std::lock_guard<std::mutex> lk(providers_mu_);
        for (const auto& provider : metrics_providers_) {
            os << "\n" << provider();
        }
    }

    return os.str();
}

void HttpServer::add_metrics_provider(std::function<std::string()> provider) {
    std::lock_guard<std::mutex> lk(providers_mu_);
    metrics_providers_.push_back(std::move(provider));
}

} // namespace zeptodb::server
