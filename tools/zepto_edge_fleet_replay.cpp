// ============================================================================
// ZeptoDB: Experimental Physical AI edge/fleet C++ connector replay
// ============================================================================

#include "third_party/httplib.h"
#include "zeptodb/feeds/edge_fleet_feed_connector.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using zeptodb::feeds::EdgeFleetDeliveryResult;
using zeptodb::feeds::EdgeFleetFeedConfig;
using zeptodb::feeds::EdgeFleetFeedConnector;
using zeptodb::feeds::EdgeFleetFeedEvent;
using zeptodb::feeds::EdgeFleetFeedPassResult;
using zeptodb::feeds::EdgeFleetEventKind;

constexpr std::string_view kEdgeOutboxTable = "physical_ai_edge_feed_outbox_016";
constexpr std::string_view kFleetExpectedActionsTable =
    "physical_ai_fleet_expected_actions_016";
constexpr std::string_view kFleetActionOutcomesTable =
    "physical_ai_fleet_action_outcomes_016";
constexpr std::string_view kFleetEdgeDecisionsTable =
    "physical_ai_fleet_edge_decisions_016";
constexpr std::string_view kFleetRetrievalTable =
    "physical_ai_fleet_retrieval_016";
constexpr std::string_view kFleetSuppressionsTable =
    "physical_ai_fleet_suppressions_016";
constexpr std::string_view kFleetInboxTable =
    "physical_ai_fleet_feed_inbox_016";
constexpr std::string_view kFleetAckTable =
    "physical_ai_fleet_feed_ack_016";
constexpr std::string_view kFleetTelemetryTable =
    "physical_ai_fleet_feed_telemetry_016";

const std::vector<std::string>& outbox_columns() {
    static const std::vector<std::string> columns = {
        "feed_event_id",
        "stream_seq",
        "event_kind",
        "query_id",
        "query_seq",
        "candidate_id",
        "suppression_key",
        "selected_action",
        "selected_action_code",
        "selected_expected_key",
        "unsafe_action_code",
        "recovery_top1_hit",
        "avoids_risky_repeat",
        "risky_action_suppressed",
        "suppressed_count",
        "edge_latency_ms",
        "retrieval_rank",
        "quality_label",
        "quality_code",
        "score_micros",
        "action_class",
        "action_code",
        "outcome_label",
        "raw_value_micros",
        "gated_value_micros",
        "context_score_micros",
        "source_edge_node_id",
        "decision_ts_ns",
        "ready_ts_ns",
    };
    return columns;
}

struct CliConfig {
    std::string edge_url = "http://127.0.0.1:19441";
    std::string fleet_url = "http://127.0.0.1:19442";
    std::string outage_url = "http://127.0.0.1:1";
    std::string edge_sql =
        "docs/research/results/physical_ai_edge_fleet_feed_replay_016_edge.sql";
    std::string fleet_seed_sql =
        "docs/research/results/physical_ai_edge_fleet_feed_replay_016_fleet.sql";
    std::string output =
        "docs/research/results/physical_ai_edge_fleet_cpp_connector_replay_018.md";
    std::string checkpoint =
        "/tmp/zeptodb_edge_fleet_cpp_connector_018.checkpoint";
    size_t batch_limit = 12;
    size_t max_inflight = 12;
    uint32_t max_retries_per_event = 1;
};

struct SqlResponse {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

struct JsonRows {
    std::vector<std::vector<std::string>> rows;
};

struct FeedOutboxRow {
    std::string feed_event_id;
    uint64_t stream_seq = 0;
    std::string event_kind;
    std::string query_id;
    int64_t query_seq = 0;
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
    int64_t source_edge_node_id = 0;
    int64_t decision_ts_ns = 0;
    int64_t ready_ts_ns = 0;
};

struct PassTelemetry {
    std::string phase;
    size_t pass_index = 0;
    EdgeFleetFeedPassResult result;
    size_t dropped_count = 0;
    bool restart_reloaded_ack = false;
    int64_t timestamp_ns = 0;
};

struct PhaseState {
    std::string phase;
    size_t pass_index = 0;
    uint64_t highest_before_pass = 0;
    bool outage = false;
    std::unordered_set<std::string> drop_once_ids;
    std::unordered_set<std::string> dropped_ids;
    size_t attempt_serial = 0;
};

std::string trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                           [&](char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char c) { return !is_space(c); })
                    .base(),
                value.end());
    return value;
}

std::string to_upper(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

bool starts_with_ci(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    return to_upper(value.substr(0, prefix.size())) == to_upper(prefix);
}

bool contains_ci(std::string_view value, std::string_view needle) {
    return to_upper(value).find(to_upper(needle)) != std::string::npos;
}

std::optional<int64_t> parse_i64(std::string_view value) {
    const std::string trimmed = trim(std::string(value));
    if (trimmed.empty()) return std::nullopt;
    char* end = nullptr;
    const long long parsed = std::strtoll(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != '\0') return std::nullopt;
    return static_cast<int64_t>(parsed);
}

std::optional<uint64_t> parse_u64(std::string_view value) {
    const std::string trimmed = trim(std::string(value));
    if (trimmed.empty()) return std::nullopt;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != '\0') return std::nullopt;
    return static_cast<uint64_t>(parsed);
}

bool parse_size_arg(std::string_view value, size_t* out) {
    auto parsed = parse_u64(value);
    if (!parsed || *parsed == 0) return false;
    *out = static_cast<size_t>(*parsed);
    return true;
}

std::string sql_quote(std::string_view value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string join_columns(const std::vector<std::string>& columns) {
    std::ostringstream out;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) out << ", ";
        out << columns[i];
    }
    return out.str();
}

std::vector<std::string> split_sql_statements(const std::string& sql) {
    std::vector<std::string> statements;
    std::string current;
    bool in_quote = false;
    for (size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        current.push_back(c);
        if (c == '\'') {
            if (in_quote && i + 1 < sql.size() && sql[i + 1] == '\'') {
                current.push_back(sql[++i]);
                continue;
            }
            in_quote = !in_quote;
        } else if (c == ';' && !in_quote) {
            current.pop_back();
            const std::string statement = trim(current);
            if (!statement.empty()) statements.push_back(statement);
            current.clear();
        }
    }
    const std::string tail = trim(current);
    if (!tail.empty()) statements.push_back(tail);
    return statements;
}

bool read_file(const std::string& path, std::string* out, std::string* error) {
    std::ifstream in(path);
    if (!in.good()) {
        if (error) *error = "failed to open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Runs Experiment 018: a native C++ EdgeFleetFeedConnector replay over\n"
        << "two live ZeptoDB HTTP nodes using the Experiment 016 Physical AI SQL fixture.\n"
        << "\n"
        << "Options:\n"
        << "  --edge-url URL           Edge ZeptoDB HTTP URL\n"
        << "  --fleet-url URL          Fleet ZeptoDB HTTP URL\n"
        << "  --outage-url URL         Simulated outage URL\n"
        << "  --edge-sql PATH          Edge seed SQL file\n"
        << "  --fleet-seed-sql PATH    Fleet seed SQL file\n"
        << "  --output PATH            Markdown result report\n"
        << "  --checkpoint PATH        Connector checkpoint file\n"
        << "  --batch-limit N          Connector batch limit\n"
        << "  --max-inflight N         Connector max inflight\n"
        << "  --help                   Show this help\n";
}

bool parse_args(int argc, char** argv, CliConfig* cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--edge-url") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->edge_url = v;
        } else if (arg == "--fleet-url") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->fleet_url = v;
        } else if (arg == "--outage-url") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->outage_url = v;
        } else if (arg == "--edge-sql") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->edge_sql = v;
        } else if (arg == "--fleet-seed-sql") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->fleet_seed_sql = v;
        } else if (arg == "--output") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->output = v;
        } else if (arg == "--checkpoint") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->checkpoint = v;
        } else if (arg == "--batch-limit") {
            const char* v = need_value(arg);
            if (!v || !parse_size_arg(v, &cfg->batch_limit)) return false;
        } else if (arg == "--max-inflight") {
            const char* v = need_value(arg);
            if (!v || !parse_size_arg(v, &cfg->max_inflight)) return false;
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

class ZeptoSqlClient {
public:
    explicit ZeptoSqlClient(std::string url)
        : url_(std::move(url)), client_(url_) {
        client_.set_connection_timeout(2);
        client_.set_read_timeout(30);
        client_.set_write_timeout(30);
    }

    SqlResponse execute(const std::string& sql) {
        httplib::Headers headers;
        headers.emplace("Content-Type", "text/plain");
        auto res = client_.Post("/", headers, sql, "text/plain");
        if (!res) {
            return {false, -1, {}, "HTTP request failed to " + url_};
        }
        if (res->status < 200 || res->status >= 300) {
            return {false, res->status, res->body,
                    "HTTP " + std::to_string(res->status) + " from " + url_};
        }
        return {true, res->status, res->body, {}};
    }

private:
    std::string url_;
    httplib::Client client_;
};

bool parse_json_string(const std::string& body, size_t* pos, std::string* out) {
    if (*pos >= body.size() || body[*pos] != '"') return false;
    ++(*pos);
    std::string value;
    while (*pos < body.size()) {
        const char c = body[(*pos)++];
        if (c == '"') {
            *out = value;
            return true;
        }
        if (c != '\\') {
            value.push_back(c);
            continue;
        }
        if (*pos >= body.size()) return false;
        const char escaped = body[(*pos)++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                return false;
        }
    }
    return false;
}

void skip_ws(const std::string& body, size_t* pos) {
    while (*pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[*pos])) != 0) {
        ++(*pos);
    }
}

bool parse_json_scalar(const std::string& body, size_t* pos, std::string* out) {
    skip_ws(body, pos);
    if (*pos >= body.size()) return false;
    if (body[*pos] == '"') return parse_json_string(body, pos, out);
    const size_t start = *pos;
    while (*pos < body.size() && body[*pos] != ',' && body[*pos] != ']' &&
           std::isspace(static_cast<unsigned char>(body[*pos])) == 0) {
        ++(*pos);
    }
    *out = body.substr(start, *pos - start);
    return true;
}

std::optional<JsonRows> parse_data_rows(const std::string& body) {
    const size_t key = body.find("\"data\"");
    if (key == std::string::npos) return JsonRows{};
    size_t pos = body.find(':', key);
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    skip_ws(body, &pos);
    if (pos >= body.size() || body[pos] != '[') return std::nullopt;
    ++pos;

    JsonRows parsed;
    skip_ws(body, &pos);
    if (pos < body.size() && body[pos] == ']') return parsed;

    while (pos < body.size()) {
        skip_ws(body, &pos);
        if (pos >= body.size() || body[pos] != '[') return std::nullopt;
        ++pos;
        std::vector<std::string> row;
        skip_ws(body, &pos);
        if (pos < body.size() && body[pos] != ']') {
            while (pos < body.size()) {
                std::string value;
                if (!parse_json_scalar(body, &pos, &value)) return std::nullopt;
                row.push_back(value);
                skip_ws(body, &pos);
                if (pos < body.size() && body[pos] == ',') {
                    ++pos;
                    continue;
                }
                break;
            }
        }
        skip_ws(body, &pos);
        if (pos >= body.size() || body[pos] != ']') return std::nullopt;
        ++pos;
        parsed.rows.push_back(std::move(row));
        skip_ws(body, &pos);
        if (pos < body.size() && body[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < body.size() && body[pos] == ']') return parsed;
        return std::nullopt;
    }
    return std::nullopt;
}

bool execute_checked(ZeptoSqlClient* client,
                     const std::string& sql,
                     const std::string& label,
                     std::string* error) {
    const auto res = client->execute(sql);
    if (res.ok) return true;
    if (error) {
        *error = label + ": " + res.error;
        if (!res.body.empty()) *error += ": " + res.body;
    }
    return false;
}

std::optional<JsonRows> query_rows(ZeptoSqlClient* client,
                                   const std::string& sql,
                                   std::string* error) {
    const auto res = client->execute(sql);
    if (!res.ok) {
        if (error) {
            *error = res.error;
            if (!res.body.empty()) *error += ": " + res.body;
        }
        return std::nullopt;
    }
    auto rows = parse_data_rows(res.body);
    if (!rows) {
        if (error) *error = "failed to parse SELECT response: " + res.body;
        return std::nullopt;
    }
    return rows;
}

std::optional<int64_t> query_i64(ZeptoSqlClient* client,
                                 const std::string& sql,
                                 std::string* error) {
    auto rows = query_rows(client, sql, error);
    if (!rows || rows->rows.empty() || rows->rows[0].empty()) {
        if (error && error->empty()) *error = "empty scalar SELECT result";
        return std::nullopt;
    }
    auto parsed = parse_i64(rows->rows[0][0]);
    if (!parsed) {
        if (error) *error = "non-integer scalar SELECT result: " + rows->rows[0][0];
        return std::nullopt;
    }
    return *parsed;
}

std::optional<int64_t> query_row_count(ZeptoSqlClient* client,
                                       const std::string& sql,
                                       std::string* error) {
    auto rows = query_rows(client, sql, error);
    if (!rows) return std::nullopt;
    return static_cast<int64_t>(rows->rows.size());
}

bool should_apply_fleet_seed_statement(const std::string& statement) {
    const std::string trimmed = trim(statement);
    if (starts_with_ci(trimmed, "DROP TABLE")) return true;
    if (starts_with_ci(trimmed, "CREATE TABLE")) return true;
    if (!starts_with_ci(trimmed, "INSERT INTO")) return false;
    return contains_ci(trimmed, "INSERT INTO " + std::string(kFleetExpectedActionsTable)) ||
           contains_ci(trimmed, "INSERT INTO " + std::string(kFleetActionOutcomesTable));
}

bool apply_sql_file(ZeptoSqlClient* client,
                    const std::string& path,
                    bool fleet_seed_filter,
                    size_t* applied_count,
                    std::string* error) {
    std::string sql;
    if (!read_file(path, &sql, error)) return false;
    const auto statements = split_sql_statements(sql);
    for (const auto& statement : statements) {
        if (fleet_seed_filter && !should_apply_fleet_seed_statement(statement)) {
            continue;
        }
        if (!execute_checked(client, statement, path, error)) return false;
        ++(*applied_count);
    }
    return true;
}

std::optional<FeedOutboxRow> row_to_outbox(const std::vector<std::string>& row,
                                           std::string* error) {
    if (row.size() != outbox_columns().size()) {
        if (error) {
            *error = "outbox SELECT returned " + std::to_string(row.size()) +
                     " columns, expected " + std::to_string(outbox_columns().size());
        }
        return std::nullopt;
    }

    auto i64 = [&](size_t index) -> std::optional<int64_t> {
        auto parsed = parse_i64(row[index]);
        if (!parsed && error) {
            *error = "failed to parse integer column " + outbox_columns()[index] +
                     ": " + row[index];
        }
        return parsed;
    };
    auto u64 = [&](size_t index) -> std::optional<uint64_t> {
        auto parsed = parse_u64(row[index]);
        if (!parsed && error) {
            *error = "failed to parse integer column " + outbox_columns()[index] +
                     ": " + row[index];
        }
        return parsed;
    };

    FeedOutboxRow out;
    out.feed_event_id = row[0];
    auto stream_seq = u64(1);
    if (!stream_seq) return std::nullopt;
    out.stream_seq = *stream_seq;
    out.event_kind = row[2];
    out.query_id = row[3];
    auto query_seq = i64(4);
    auto selected_action_code = i64(8);
    auto unsafe_action_code = i64(10);
    auto recovery_top1_hit = i64(11);
    auto avoids_risky_repeat = i64(12);
    auto risky_action_suppressed = i64(13);
    auto suppressed_count = i64(14);
    auto edge_latency_ms = i64(15);
    auto retrieval_rank = i64(16);
    auto quality_code = i64(18);
    auto score_micros = i64(19);
    auto action_code = i64(21);
    auto raw_value_micros = i64(23);
    auto gated_value_micros = i64(24);
    auto context_score_micros = i64(25);
    auto source_edge_node_id = i64(26);
    auto decision_ts_ns = i64(27);
    auto ready_ts_ns = i64(28);
    if (!query_seq || !selected_action_code || !unsafe_action_code ||
        !recovery_top1_hit || !avoids_risky_repeat || !risky_action_suppressed ||
        !suppressed_count || !edge_latency_ms || !retrieval_rank || !quality_code ||
        !score_micros || !action_code || !raw_value_micros || !gated_value_micros ||
        !context_score_micros || !source_edge_node_id || !decision_ts_ns ||
        !ready_ts_ns) {
        return std::nullopt;
    }
    out.query_seq = *query_seq;
    out.candidate_id = row[5];
    out.suppression_key = row[6];
    out.selected_action = row[7];
    out.selected_action_code = *selected_action_code;
    out.selected_expected_key = row[9];
    out.unsafe_action_code = *unsafe_action_code;
    out.recovery_top1_hit = *recovery_top1_hit;
    out.avoids_risky_repeat = *avoids_risky_repeat;
    out.risky_action_suppressed = *risky_action_suppressed;
    out.suppressed_count = *suppressed_count;
    out.edge_latency_ms = *edge_latency_ms;
    out.retrieval_rank = *retrieval_rank;
    out.quality_label = row[17];
    out.quality_code = *quality_code;
    out.score_micros = *score_micros;
    out.action_class = row[20];
    out.action_code = *action_code;
    out.outcome_label = row[22];
    out.raw_value_micros = *raw_value_micros;
    out.gated_value_micros = *gated_value_micros;
    out.context_score_micros = *context_score_micros;
    out.source_edge_node_id = *source_edge_node_id;
    out.decision_ts_ns = *decision_ts_ns;
    out.ready_ts_ns = *ready_ts_ns;
    return out;
}

std::optional<std::vector<FeedOutboxRow>> load_outbox(ZeptoSqlClient* edge,
                                                      std::string* error) {
    const std::string sql = "SELECT " + join_columns(outbox_columns()) +
                            " FROM " + std::string(kEdgeOutboxTable) +
                            " ORDER BY stream_seq ASC";
    auto rows = query_rows(edge, sql, error);
    if (!rows) return std::nullopt;
    std::vector<FeedOutboxRow> outbox;
    outbox.reserve(rows->rows.size());
    for (const auto& row : rows->rows) {
        auto parsed = row_to_outbox(row, error);
        if (!parsed) return std::nullopt;
        outbox.push_back(std::move(*parsed));
    }
    return outbox;
}

EdgeFleetFeedEvent to_feed_event(const FeedOutboxRow& row) {
    EdgeFleetFeedEvent event;
    event.event_id = row.feed_event_id;
    event.stream_seq = row.stream_seq;
    auto parsed_kind = EdgeFleetFeedConnector::parseKind(row.event_kind);
    event.kind = parsed_kind.value_or(EdgeFleetEventKind::Decision);
    event.ready_ts_ns = row.ready_ts_ns;
    event.query_id = row.query_id;
    event.payload_json = "{\"source\":\"physical_ai_edge_feed_outbox_016\"}";
    return event;
}

std::vector<EdgeFleetFeedEvent> to_feed_events(const std::vector<FeedOutboxRow>& rows) {
    std::vector<EdgeFleetFeedEvent> events;
    events.reserve(rows.size());
    for (const auto& row : rows) {
        events.push_back(to_feed_event(row));
    }
    return events;
}

std::string inbox_insert_sql(const FeedOutboxRow& row,
                             const std::string& status,
                             int64_t ack_ts_ns,
                             bool late_delivery,
                             size_t attempt_no) {
    const std::string attempt_id = row.feed_event_id + "|cpp_attempt|" +
                                   std::to_string(attempt_no) + "|" +
                                   std::to_string(ack_ts_ns);
    std::ostringstream sql;
    sql << "INSERT INTO " << kFleetInboxTable
        << " (attempt_id, feed_event_id, stream_seq, event_kind, query_id, "
           "delivery_status, duplicate_delivery, late_delivery, attempt_no, "
           "source_edge_node_id, timestamp) VALUES ("
        << sql_quote(attempt_id) << ", " << sql_quote(row.feed_event_id) << ", "
        << row.stream_seq << ", " << sql_quote(row.event_kind) << ", "
        << sql_quote(row.query_id) << ", " << sql_quote(status) << ", 0, "
        << (late_delivery ? 1 : 0) << ", " << attempt_no << ", "
        << row.source_edge_node_id << ", " << ack_ts_ns << ")";
    return sql.str();
}

std::string apply_event_sql(const FeedOutboxRow& row, int64_t ack_ts_ns) {
    std::ostringstream sql;
    if (row.event_kind == "decision") {
        const int64_t lag_ms = std::max<int64_t>(0, (ack_ts_ns - row.decision_ts_ns) / 1000000);
        sql << "INSERT INTO " << kFleetEdgeDecisionsTable
            << " (query_id, query_seq, robot_code, selected_action, selected_action_code, "
               "selected_expected_key, unsafe_action_code, recovery_top1_hit, "
               "avoids_risky_repeat, risky_action_suppressed, suppressed_count, "
               "edge_latency_ms, decision_ts_ns, consolidated_ts_ns, "
               "consolidation_lag_ms, source_edge_node_id, timestamp) VALUES ("
            << sql_quote(row.query_id) << ", " << row.query_seq << ", 0, "
            << sql_quote(row.selected_action) << ", " << row.selected_action_code << ", "
            << sql_quote(row.selected_expected_key) << ", " << row.unsafe_action_code << ", "
            << row.recovery_top1_hit << ", " << row.avoids_risky_repeat << ", "
            << row.risky_action_suppressed << ", " << row.suppressed_count << ", "
            << row.edge_latency_ms << ", " << row.decision_ts_ns << ", " << ack_ts_ns
            << ", " << lag_ms << ", " << row.source_edge_node_id << ", "
            << ack_ts_ns << ")";
    } else if (row.event_kind == "retrieval") {
        sql << "INSERT INTO " << kFleetRetrievalTable
            << " (query_id, query_seq, retrieval_rank, candidate_id, suppression_key, "
               "candidate_action, quality_label, quality_code, score_micros, timestamp) "
               "VALUES ("
            << sql_quote(row.query_id) << ", " << row.query_seq << ", "
            << row.retrieval_rank << ", " << sql_quote(row.candidate_id) << ", "
            << sql_quote(row.suppression_key) << ", " << sql_quote(row.action_class)
            << ", " << sql_quote(row.quality_label) << ", " << row.quality_code
            << ", " << row.score_micros << ", " << ack_ts_ns << ")";
    } else {
        sql << "INSERT INTO " << kFleetSuppressionsTable
            << " (query_id, query_seq, candidate_id, suppression_key, action_class, "
               "action_code, outcome_label, raw_value_micros, gated_value_micros, "
               "context_score_micros, source_edge_node_id, timestamp) VALUES ("
            << sql_quote(row.query_id) << ", " << row.query_seq << ", "
            << sql_quote(row.candidate_id) << ", " << sql_quote(row.suppression_key)
            << ", " << sql_quote(row.action_class) << ", " << row.action_code
            << ", " << sql_quote(row.outcome_label) << ", " << row.raw_value_micros
            << ", " << row.gated_value_micros << ", " << row.context_score_micros
            << ", " << row.source_edge_node_id << ", " << ack_ts_ns << ")";
    }
    return sql.str();
}

std::string ack_insert_sql(const FeedOutboxRow& row, int64_t ack_ts_ns) {
    std::ostringstream sql;
    sql << "INSERT INTO " << kFleetAckTable
        << " (feed_event_id, stream_seq, event_kind, query_id, ack_status, "
           "source_edge_node_id, ack_ts_ns, timestamp) VALUES ("
        << sql_quote(row.feed_event_id) << ", " << row.stream_seq << ", "
        << sql_quote(row.event_kind) << ", " << sql_quote(row.query_id)
        << ", 'acked', " << row.source_edge_node_id << ", " << ack_ts_ns
        << ", " << ack_ts_ns << ")";
    return sql.str();
}

class FleetSqlSink {
public:
    FleetSqlSink(ZeptoSqlClient* fleet,
                 ZeptoSqlClient* outage,
                 const std::unordered_map<std::string, FeedOutboxRow>* rows_by_id,
                 PhaseState* phase)
        : fleet_(fleet), outage_(outage), rows_by_id_(rows_by_id), phase_(phase) {}

    EdgeFleetDeliveryResult operator()(const EdgeFleetFeedEvent& event) {
        if (phase_->outage) {
            if (outage_) {
                (void)outage_->execute("SELECT count(*) FROM " +
                                       std::string(kFleetAckTable));
            }
            return EdgeFleetDeliveryResult::TransientFailure;
        }

        if (phase_->drop_once_ids.find(event.event_id) != phase_->drop_once_ids.end() &&
            phase_->dropped_ids.find(event.event_id) == phase_->dropped_ids.end()) {
            phase_->dropped_ids.insert(event.event_id);
            return EdgeFleetDeliveryResult::TransientFailure;
        }

        const auto it = rows_by_id_->find(event.event_id);
        if (it == rows_by_id_->end()) return EdgeFleetDeliveryResult::PermanentFailure;
        const FeedOutboxRow& row = it->second;
        const int64_t ack_ts_ns =
            row.ready_ts_ns + static_cast<int64_t>(phase_->pass_index) * 10000000 +
            static_cast<int64_t>(++phase_->attempt_serial);
        const bool late_delivery = row.stream_seq <= phase_->highest_before_pass;

        std::string ignored;
        if (!execute_checked(fleet_, inbox_insert_sql(row, "delivered", ack_ts_ns,
                                                     late_delivery, phase_->attempt_serial),
                             "fleet inbox insert", &ignored)) {
            return EdgeFleetDeliveryResult::TransientFailure;
        }
        if (!execute_checked(fleet_, apply_event_sql(row, ack_ts_ns),
                             "fleet event materialization", &ignored)) {
            return EdgeFleetDeliveryResult::TransientFailure;
        }
        if (!execute_checked(fleet_, ack_insert_sql(row, ack_ts_ns),
                             "fleet ack insert", &ignored)) {
            return EdgeFleetDeliveryResult::AppliedButAckFailed;
        }
        return EdgeFleetDeliveryResult::Acked;
    }

private:
    ZeptoSqlClient* fleet_;
    ZeptoSqlClient* outage_;
    const std::unordered_map<std::string, FeedOutboxRow>* rows_by_id_;
    PhaseState* phase_;
};

std::string telemetry_insert_sql(const PassTelemetry& telemetry,
                                 size_t max_inflight,
                                 size_t batch_limit) {
    const auto& result = telemetry.result;
    const size_t failed = result.transient_failure_count +
                          result.permanent_failure_count +
                          result.ack_boundary_failure_count;
    std::ostringstream sql;
    sql << "INSERT INTO " << kFleetTelemetryTable
        << " (phase, pass_index, batch_event_count, attempted_count, delivered_count, "
           "failed_count, dropped_count, duplicate_attempt_count, late_count, "
           "acked_before, acked_after, max_inflight, batch_limit, "
           "restart_reloaded_ack, timestamp) VALUES ("
        << sql_quote(telemetry.phase) << ", " << telemetry.pass_index << ", "
        << result.batch_event_count << ", " << result.attempted_count << ", "
        << result.acked_count << ", " << failed << ", " << telemetry.dropped_count
        << ", " << result.duplicate_count << ", " << result.late_count << ", "
        << result.acked_before << ", " << result.acked_after << ", "
        << max_inflight << ", " << batch_limit << ", "
        << (telemetry.restart_reloaded_ack ? 1 : 0) << ", "
        << telemetry.timestamp_ns << ")";
    return sql.str();
}

bool record_telemetry(ZeptoSqlClient* fleet,
                      const PassTelemetry& telemetry,
                      size_t max_inflight,
                      size_t batch_limit,
                      std::string* error) {
    return execute_checked(fleet,
                           telemetry_insert_sql(telemetry, max_inflight, batch_limit),
                           "fleet telemetry insert",
                           error);
}

PassTelemetry make_telemetry(const std::string& phase,
                             size_t pass_index,
                             const EdgeFleetFeedPassResult& result,
                             size_t dropped_count,
                             bool restart_reloaded_ack) {
    PassTelemetry telemetry;
    telemetry.phase = phase;
    telemetry.pass_index = pass_index;
    telemetry.result = result;
    telemetry.dropped_count = dropped_count;
    telemetry.restart_reloaded_ack = restart_reloaded_ack;
    telemetry.timestamp_ns =
        1810000000000000000LL + 20000000000LL + static_cast<int64_t>(pass_index) * 1000000LL;
    return telemetry;
}

std::optional<PassTelemetry> run_pass(EdgeFleetFeedConnector* connector,
                                      ZeptoSqlClient* fleet,
                                      const std::unordered_map<std::string, FeedOutboxRow>& rows_by_id,
                                      const std::vector<EdgeFleetFeedEvent>& events,
                                      PhaseState* phase,
                                      bool restart_reloaded_ack,
                                      size_t max_inflight,
                                      size_t batch_limit,
                                      std::string* error) {
    (void)rows_by_id;
    const EdgeFleetFeedPassResult result = connector->processOnce(events);
    PassTelemetry telemetry = make_telemetry(phase->phase,
                                            phase->pass_index,
                                            result,
                                            phase->dropped_ids.size(),
                                            restart_reloaded_ack);
    if (!record_telemetry(fleet, telemetry, max_inflight, batch_limit, error)) {
        return std::nullopt;
    }
    return telemetry;
}

std::optional<std::vector<PassTelemetry>> run_connector_replay(
    const CliConfig& cfg,
    ZeptoSqlClient* fleet,
    ZeptoSqlClient* outage,
    const std::vector<FeedOutboxRow>& outbox,
    const std::unordered_map<std::string, FeedOutboxRow>& rows_by_id,
    std::string* error) {
    if (outbox.size() < 3) {
        if (error) *error = "fixture outbox must contain at least three events";
        return std::nullopt;
    }
    std::filesystem::remove(cfg.checkpoint);

    std::vector<EdgeFleetFeedEvent> events = to_feed_events(outbox);
    std::vector<EdgeFleetFeedEvent> duplicated_events = events;
    duplicated_events.insert(duplicated_events.begin() + 3, events[2]);
    const std::string dropped_event_id = outbox[1].feed_event_id;

    std::vector<PassTelemetry> telemetry;
    telemetry.reserve(8);

    PhaseState phase;
    EdgeFleetFeedConfig connector_cfg;
    connector_cfg.batch_limit = cfg.batch_limit;
    connector_cfg.max_inflight = cfg.max_inflight;
    connector_cfg.max_retries_per_event = cfg.max_retries_per_event;
    connector_cfg.allow_late_events = true;
    connector_cfg.checkpoint_path = cfg.checkpoint;

    FleetSqlSink initial_sink(fleet, outage, &rows_by_id, &phase);
    EdgeFleetFeedConnector connector(connector_cfg, initial_sink);

    phase = PhaseState{};
    phase.phase = "outage_probe";
    phase.pass_index = 1;
    phase.highest_before_pass = connector.highestAckedStreamSeq();
    phase.outage = true;
    auto outage_pass = run_pass(&connector,
                                fleet,
                                rows_by_id,
                                events,
                                &phase,
                                false,
                                cfg.max_inflight,
                                cfg.batch_limit,
                                error);
    if (!outage_pass) return std::nullopt;
    telemetry.push_back(*outage_pass);

    phase = PhaseState{};
    phase.phase = "bounded_recovery_with_drop_duplicate";
    phase.pass_index = 2;
    phase.highest_before_pass = connector.highestAckedStreamSeq();
    phase.drop_once_ids.insert(dropped_event_id);
    auto recovery = run_pass(&connector,
                             fleet,
                             rows_by_id,
                             duplicated_events,
                             &phase,
                             false,
                             cfg.max_inflight,
                             cfg.batch_limit,
                             error);
    if (!recovery) return std::nullopt;
    telemetry.push_back(*recovery);

    FleetSqlSink restarted_sink(fleet, outage, &rows_by_id, &phase);
    EdgeFleetFeedConnector restarted(connector_cfg, restarted_sink);
    std::string checkpoint_error;
    const bool reloaded = restarted.loadCheckpoint(&checkpoint_error);
    if (!reloaded) {
        if (error) *error = "failed to load connector checkpoint: " + checkpoint_error;
        return std::nullopt;
    }

    size_t pass_index = 3;
    while (restarted.ackedCount() < outbox.size() && pass_index < 12) {
        phase = PhaseState{};
        phase.phase = pass_index == 3 ? "restart_retry_late_delivery" : "bounded_final_drain";
        phase.pass_index = pass_index;
        phase.highest_before_pass = restarted.highestAckedStreamSeq();
        auto item = run_pass(&restarted,
                             fleet,
                             rows_by_id,
                             events,
                             &phase,
                             pass_index == 3,
                             cfg.max_inflight,
                             cfg.batch_limit,
                             error);
        if (!item) return std::nullopt;
        telemetry.push_back(*item);
        ++pass_index;
    }

    if (restarted.ackedCount() != outbox.size()) {
        if (error) {
            *error = "replay did not converge: acked=" +
                     std::to_string(restarted.ackedCount()) +
                     " outbox=" + std::to_string(outbox.size());
        }
        return std::nullopt;
    }
    return telemetry;
}

std::string select_recovery_join_sql() {
    return "SELECT d.query_id FROM " + std::string(kFleetEdgeDecisionsTable) +
           " d JOIN " + std::string(kFleetExpectedActionsTable) +
           " e ON d.selected_expected_key = e.expected_action_key";
}

std::string select_suppression_join_sql() {
    return "SELECT s.query_id FROM " + std::string(kFleetSuppressionsTable) +
           " s JOIN " + std::string(kFleetActionOutcomesTable) +
           " o ON s.candidate_id = o.episode_id AND s.action_class = o.action_class";
}

bool validate_counts(ZeptoSqlClient* edge,
                     ZeptoSqlClient* fleet,
                     const std::vector<FeedOutboxRow>& outbox,
                     std::unordered_map<std::string, int64_t>* counts,
                     std::string* error) {
    auto edge_outbox = query_i64(edge, "SELECT count(*) FROM " + std::string(kEdgeOutboxTable), error);
    auto fleet_ack = query_i64(fleet, "SELECT count(*) FROM " + std::string(kFleetAckTable), error);
    auto fleet_inbox = query_i64(fleet, "SELECT count(*) FROM " + std::string(kFleetInboxTable), error);
    auto decisions = query_i64(fleet, "SELECT count(*) FROM " + std::string(kFleetEdgeDecisionsTable), error);
    auto retrieval = query_i64(fleet, "SELECT count(*) FROM " + std::string(kFleetRetrievalTable), error);
    auto suppressions = query_i64(fleet, "SELECT count(*) FROM " + std::string(kFleetSuppressionsTable), error);
    auto telemetry = query_i64(fleet, "SELECT count(*) FROM " + std::string(kFleetTelemetryTable), error);
    auto telemetry_rows = query_rows(
        fleet,
        "SELECT phase, failed_count, duplicate_attempt_count, late_count, "
        "restart_reloaded_ack FROM " + std::string(kFleetTelemetryTable),
        error);
    auto recovery_join = query_row_count(fleet, select_recovery_join_sql(), error);
    auto suppression_join = query_row_count(fleet, select_suppression_join_sql(), error);

    if (!edge_outbox || !fleet_ack || !fleet_inbox || !decisions || !retrieval ||
        !suppressions || !telemetry || !telemetry_rows || !recovery_join ||
        !suppression_join) {
        return false;
    }

    int64_t outage = 0;
    int64_t duplicate = 0;
    int64_t late = 0;
    int64_t restart = 0;
    for (const auto& row : telemetry_rows->rows) {
        if (row.size() != 5) {
            if (error) *error = "telemetry validation SELECT returned unexpected columns";
            return false;
        }
        const auto failed_count = parse_i64(row[1]);
        const auto duplicate_count = parse_i64(row[2]);
        const auto late_count = parse_i64(row[3]);
        const auto restart_reloaded = parse_i64(row[4]);
        if (!failed_count || !duplicate_count || !late_count || !restart_reloaded) {
            if (error) *error = "telemetry validation SELECT returned non-integer counters";
            return false;
        }
        if (row[0] == "outage_probe" && *failed_count > 0) ++outage;
        if (*duplicate_count > 0) ++duplicate;
        if (*late_count > 0) ++late;
        if (*restart_reloaded == 1) ++restart;
    }

    (*counts)["edge_outbox"] = *edge_outbox;
    (*counts)["fleet_ack"] = *fleet_ack;
    (*counts)["fleet_inbox"] = *fleet_inbox;
    (*counts)["fleet_decisions"] = *decisions;
    (*counts)["fleet_retrieval"] = *retrieval;
    (*counts)["fleet_suppressions"] = *suppressions;
    (*counts)["fleet_telemetry"] = *telemetry;
    (*counts)["outage_telemetry_rows"] = outage;
    (*counts)["duplicate_telemetry_rows"] = duplicate;
    (*counts)["late_telemetry_rows"] = late;
    (*counts)["restart_telemetry_rows"] = restart;
    (*counts)["recovery_join_rows"] = *recovery_join;
    (*counts)["suppression_join_rows"] = *suppression_join;

    if (*edge_outbox != static_cast<int64_t>(outbox.size()) ||
        *fleet_ack != static_cast<int64_t>(outbox.size()) ||
        *decisions != 5 ||
        *retrieval != 15 ||
        *suppressions != 32 ||
        outage < 1 ||
        duplicate < 1 ||
        late < 1 ||
        restart < 1 ||
        *recovery_join != 5 ||
        *suppression_join < 32) {
        if (error) *error = "validation counts did not match expected replay invariants";
        return false;
    }
    return true;
}

bool write_report(const CliConfig& cfg,
                  size_t edge_statements,
                  size_t fleet_seed_statements,
                  const std::vector<FeedOutboxRow>& outbox,
                  const std::vector<PassTelemetry>& telemetry,
                  const std::unordered_map<std::string, int64_t>& counts,
                  std::string* error) {
    const std::filesystem::path output_path(cfg.output);
    const auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) *error = "failed to create report directory: " + ec.message();
            return false;
        }
    }

    std::ofstream out(cfg.output);
    if (!out.good()) {
        if (error) *error = "failed to write report " + cfg.output;
        return false;
    }

    out << "# Experiment 018: C++ Connector Two-Node Edge/Fleet Replay\n\n";
    out << "Status: PASS\n\n";
    out << "This run connects the experimental `EdgeFleetFeedConnector` to two live "
           "ZeptoDB HTTP nodes. The edge node is seeded from the Physical AI SQL "
           "fixture, the C++ connector reads the edge outbox through native SQL, "
           "and the fleet node receives inbox, materialized operational rows, ACKs, "
           "and feed telemetry through SQL inserts.\n\n";
    out << "## Inputs\n\n";
    out << "- Edge URL: `" << cfg.edge_url << "`\n";
    out << "- Fleet URL: `" << cfg.fleet_url << "`\n";
    out << "- Outage URL: `" << cfg.outage_url << "`\n";
    out << "- Edge SQL statements applied: " << edge_statements << "\n";
    out << "- Fleet seed SQL statements applied: " << fleet_seed_statements << "\n";
    out << "- Connector batch limit: " << cfg.batch_limit << "\n";
    out << "- Connector max inflight: " << cfg.max_inflight << "\n";
    out << "- Edge outbox events: " << outbox.size() << "\n";
    out << "- Checkpoint: `" << cfg.checkpoint << "`\n\n";
    out << "## Pass Telemetry\n\n";
    out << "| phase | pass | batch | attempted | acked | failed | dropped | "
           "duplicates | late | acked before | acked after | restart reload |\n";
    out << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& item : telemetry) {
        const auto failed = item.result.transient_failure_count +
                            item.result.permanent_failure_count +
                            item.result.ack_boundary_failure_count;
        out << "| " << item.phase << " | " << item.pass_index << " | "
            << item.result.batch_event_count << " | " << item.result.attempted_count
            << " | " << item.result.acked_count << " | " << failed << " | "
            << item.dropped_count << " | " << item.result.duplicate_count << " | "
            << item.result.late_count << " | " << item.result.acked_before << " | "
            << item.result.acked_after << " | "
            << (item.restart_reloaded_ack ? 1 : 0) << " |\n";
    }

    auto count = [&](const std::string& key) -> int64_t {
        const auto it = counts.find(key);
        return it == counts.end() ? 0 : it->second;
    };

    out << "\n## Native SQL Validation\n\n";
    out << "| check | value |\n";
    out << "| --- | ---: |\n";
    out << "| edge outbox rows | " << count("edge_outbox") << " |\n";
    out << "| fleet inbox rows | " << count("fleet_inbox") << " |\n";
    out << "| fleet ACK rows | " << count("fleet_ack") << " |\n";
    out << "| fleet decision rows | " << count("fleet_decisions") << " |\n";
    out << "| fleet retrieval rows | " << count("fleet_retrieval") << " |\n";
    out << "| fleet suppression rows | " << count("fleet_suppressions") << " |\n";
    out << "| fleet telemetry rows | " << count("fleet_telemetry") << " |\n";
    out << "| outage telemetry rows | " << count("outage_telemetry_rows") << " |\n";
    out << "| duplicate telemetry rows | " << count("duplicate_telemetry_rows") << " |\n";
    out << "| late telemetry rows | " << count("late_telemetry_rows") << " |\n";
    out << "| restart telemetry rows | " << count("restart_telemetry_rows") << " |\n";
    out << "| recovery JOIN rows | " << count("recovery_join_rows") << " |\n";
    out << "| suppression audit JOIN rows | " << count("suppression_join_rows") << " |\n\n";
    out << "## Interpretation\n\n";
    out << "The C++ connector preserves edge-local replay state under outage, bounded "
           "drop, duplicate, late, and restart phases. Fleet-global audit rows "
           "converge after checkpoint reload, and the native JOIN checks prove "
           "that recovery recommendations and suppression audit rows remain "
           "queryable after materialization.\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    CliConfig cfg;
    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return 2;
    }

    ZeptoSqlClient edge(cfg.edge_url);
    ZeptoSqlClient fleet(cfg.fleet_url);
    ZeptoSqlClient outage(cfg.outage_url);

    size_t edge_statement_count = 0;
    size_t fleet_seed_statement_count = 0;
    std::string error;
    if (!apply_sql_file(&edge, cfg.edge_sql, false, &edge_statement_count, &error)) {
        std::cerr << error << "\n";
        return 3;
    }
    if (!apply_sql_file(&fleet,
                        cfg.fleet_seed_sql,
                        true,
                        &fleet_seed_statement_count,
                        &error)) {
        std::cerr << error << "\n";
        return 4;
    }

    auto outbox = load_outbox(&edge, &error);
    if (!outbox) {
        std::cerr << error << "\n";
        return 5;
    }

    std::unordered_map<std::string, FeedOutboxRow> rows_by_id;
    rows_by_id.reserve(outbox->size());
    for (const auto& row : *outbox) {
        rows_by_id.emplace(row.feed_event_id, row);
    }

    auto telemetry = run_connector_replay(cfg, &fleet, &outage, *outbox, rows_by_id, &error);
    if (!telemetry) {
        std::cerr << error << "\n";
        return 6;
    }

    std::unordered_map<std::string, int64_t> counts;
    if (!validate_counts(&edge, &fleet, *outbox, &counts, &error)) {
        std::cerr << error << "\n";
        return 7;
    }
    if (!write_report(cfg,
                      edge_statement_count,
                      fleet_seed_statement_count,
                      *outbox,
                      *telemetry,
                      counts,
                      &error)) {
        std::cerr << error << "\n";
        return 8;
    }

    std::cout << "Experiment 018 PASS: C++ connector replay converged with "
              << counts["fleet_ack"] << " ACKs; report=" << cfg.output << "\n";
    return 0;
}
