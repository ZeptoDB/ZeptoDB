// ============================================================================
// ZeptoDB: SQL/HTTP-backed edge/fleet connector adapter
// ============================================================================

#ifdef ZEPTO_TLS_ENABLED
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include "zeptodb/server/edge_fleet_sql_http_adapter.h"

#include "third_party/httplib.h"
#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zeptodb::server {
namespace {

using zeptodb::feeds::EdgeFleetConnectorRuntimeHooks;
using zeptodb::feeds::EdgeFleetDeliveryResult;
using zeptodb::feeds::EdgeFleetEventKind;
using zeptodb::feeds::EdgeFleetFeedEvent;
using zeptodb::feeds::EdgeFleetFeedPassResult;
using zeptodb::feeds::EdgeFleetOutboxLoadResult;
using zeptodb::sql::QueryExecutor;
using zeptodb::sql::QueryResultSet;
using zeptodb::storage::ColumnType;

struct SqlRows {
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

bool is_safe_identifier(const std::string& value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first) && value.front() != '_') return false;
    return std::all_of(value.begin() + 1, value.end(), [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || ch == '_';
    });
}

bool require_identifier(const std::string& value,
                        const std::string& label,
                        std::string* error) {
    if (is_safe_identifier(value)) return true;
    if (error) *error = label + " must be a simple SQL identifier";
    return false;
}

bool require_url(const std::string& value,
                 const std::string& label,
                 std::string* error) {
    if (value.empty()) return true;
    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
        return true;
    }
    if (error) *error = label + " must start with http:// or https://";
    return false;
}

std::string sql_string_literal(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') out.push_back('\'');
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

std::string join_columns(const std::vector<std::string>& columns) {
    std::ostringstream out;
    for (size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) out << ", ";
        out << columns[index];
    }
    return out.str();
}

std::string insert_sql(const std::string& table,
                       const std::vector<std::string>& columns,
                       const std::vector<std::string>& values) {
    std::ostringstream sql;
    sql << "INSERT INTO " << table << " (";
    for (size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) sql << ", ";
        sql << columns[index];
    }
    sql << ") VALUES (";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) sql << ", ";
        sql << values[index];
    }
    sql << ")";
    return sql.str();
}

int64_t now_ns() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

size_t result_row_count(const QueryResultSet& result) {
    return std::max(result.rows.size(), result.typed_rows.size());
}

int64_t cell_i64(const QueryResultSet& result, size_t row, size_t col) {
    if (row < result.typed_rows.size() && col < result.typed_rows[row].size()) {
        if (col < result.column_types.size() &&
            (result.column_types[col] == ColumnType::FLOAT64 ||
             result.column_types[col] == ColumnType::FLOAT32)) {
            return static_cast<int64_t>(result.typed_rows[row][col].f);
        }
        return result.typed_rows[row][col].i;
    }
    if (row < result.rows.size() && col < result.rows[row].size()) {
        return result.rows[row][col];
    }
    return 0;
}

size_t string_cell_index(const QueryResultSet& result, size_t row, size_t col) {
    size_t per_row = 0;
    size_t before_col = 0;
    for (size_t column = 0; column < result.column_names.size(); ++column) {
        const bool is_string =
            column < result.column_types.size() &&
            (result.column_types[column] == ColumnType::STRING ||
             result.column_types[column] == ColumnType::SYMBOL);
        if (is_string) {
            if (column < col) ++before_col;
            ++per_row;
        }
    }
    return row * per_row + before_col;
}

std::string cell_string(const QueryResultSet& result, size_t row, size_t col) {
    if (col < result.column_types.size() &&
        (result.column_types[col] == ColumnType::STRING ||
         result.column_types[col] == ColumnType::SYMBOL)) {
        if (result.symbol_dict != nullptr) {
            return std::string(result.symbol_dict->lookup(
                static_cast<uint32_t>(cell_i64(result, row, col))));
        }
        const size_t index = string_cell_index(result, row, col);
        if (index < result.string_rows.size()) return result.string_rows[index];
    }
    if (result.rows.empty() && col == 0 && row < result.string_rows.size()) {
        return result.string_rows[row];
    }
    return std::to_string(cell_i64(result, row, col));
}

SqlRows rows_from_result(const QueryResultSet& result) {
    SqlRows rows;
    const size_t count = result_row_count(result);
    rows.rows.reserve(count);
    for (size_t row = 0; row < count; ++row) {
        std::vector<std::string> values;
        values.reserve(result.column_names.size());
        for (size_t col = 0; col < result.column_names.size(); ++col) {
            values.push_back(cell_string(result, row, col));
        }
        rows.rows.push_back(std::move(values));
    }
    if (count == 0 && result.rows.empty() && !result.string_rows.empty() &&
        result.column_names.size() == 1) {
        for (const auto& value : result.string_rows) {
            rows.rows.push_back({value});
        }
    }
    return rows;
}

void skip_ws(const std::string& body, size_t* pos) {
    while (*pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[*pos])) != 0) {
        ++(*pos);
    }
}

bool parse_json_string(const std::string& body, size_t* pos, std::string* out) {
    if (*pos >= body.size() || body[*pos] != '"') return false;
    ++(*pos);
    std::string value;
    while (*pos < body.size()) {
        const char ch = body[(*pos)++];
        if (ch == '"') {
            *out = value;
            return true;
        }
        if (ch != '\\') {
            value.push_back(ch);
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

std::optional<SqlRows> parse_http_data_rows(const std::string& body) {
    const size_t key = body.find("\"data\"");
    if (key == std::string::npos) return SqlRows{};
    size_t pos = body.find(':', key);
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    skip_ws(body, &pos);
    if (pos >= body.size() || body[pos] != '[') return std::nullopt;
    ++pos;

    SqlRows parsed;
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

std::optional<int64_t> parse_i64(std::string_view value) {
    const std::string text(value);
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return static_cast<int64_t>(parsed);
}

std::optional<uint64_t> parse_u64(std::string_view value) {
    if (!value.empty() && value.front() == '-') return std::nullopt;
    const std::string text(value);
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return static_cast<uint64_t>(parsed);
}

bool execute_local(QueryExecutor& executor,
                   const std::string& sql,
                   const std::string& context,
                   std::string* error) {
    const auto result = executor.execute(sql);
    if (result.ok()) return true;
    if (error) *error = context + ": " + result.error;
    return false;
}

std::optional<SqlRows> query_local(QueryExecutor& executor,
                                   const std::string& sql,
                                   const std::string& context,
                                   std::string* error) {
    const auto result = executor.execute(sql);
    if (!result.ok()) {
        if (error) *error = context + ": " + result.error;
        return std::nullopt;
    }
    return rows_from_result(result);
}

bool execute_http(const std::string& url,
                  const std::string& sql,
                  const std::string& context,
                  std::string* error) {
    httplib::Client client(url);
    client.set_connection_timeout(2);
    client.set_read_timeout(30);
    client.set_write_timeout(30);
    httplib::Headers headers;
    headers.emplace("Content-Type", "text/plain");
    const auto response = client.Post("/", headers, sql, "text/plain");
    if (!response) {
        if (error) *error = context + ": HTTP request failed to " + url;
        return false;
    }
    if (response->status < 200 || response->status >= 300) {
        if (error) {
            *error = context + ": HTTP " + std::to_string(response->status) +
                     " from " + url;
            if (!response->body.empty()) *error += ": " + response->body;
        }
        return false;
    }
    return true;
}

std::optional<SqlRows> query_http(const std::string& url,
                                  const std::string& sql,
                                  const std::string& context,
                                  std::string* error) {
    httplib::Client client(url);
    client.set_connection_timeout(2);
    client.set_read_timeout(30);
    client.set_write_timeout(30);
    httplib::Headers headers;
    headers.emplace("Content-Type", "text/plain");
    const auto response = client.Post("/", headers, sql, "text/plain");
    if (!response) {
        if (error) *error = context + ": HTTP request failed to " + url;
        return std::nullopt;
    }
    if (response->status < 200 || response->status >= 300) {
        if (error) {
            *error = context + ": HTTP " + std::to_string(response->status) +
                     " from " + url;
            if (!response->body.empty()) *error += ": " + response->body;
        }
        return std::nullopt;
    }
    auto rows = parse_http_data_rows(response->body);
    if (!rows) {
        if (error) *error = context + ": failed to parse SELECT response";
        return std::nullopt;
    }
    return rows;
}

class EdgeFleetSqlHttpAdapterState {
public:
    EdgeFleetSqlHttpAdapterState(QueryExecutor& executor,
                                 EdgeFleetSqlHttpAdapterConfig config)
        : executor_(executor), config_(std::move(config)) {}

    EdgeFleetOutboxLoadResult loadOutbox() {
        EdgeFleetOutboxLoadResult out;
        std::string error;
        size_t decoded_bytes = 0;
        uint64_t after_stream_seq = 0;
        std::unordered_map<std::string, FeedOutboxRow> next_rows_by_id;
        next_rows_by_id.reserve(config_.outbox_query_limit);
        out.events.reserve(config_.outbox_query_limit);

        while (out.events.size() < config_.outbox_query_limit) {
            auto rows = query(edgeUrl(),
                              outboxSelectSql(after_stream_seq),
                              "edge outbox SELECT",
                              &error);
            if (!rows) {
                out.error = error.empty() ? "edge outbox load failed" : error;
                return out;
            }
            if (rows->rows.empty()) break;

            bool advanced = false;
            for (const auto& row : rows->rows) {
                for (const auto& cell : row) {
                    decoded_bytes += cell.size();
                    if (decoded_bytes > config_.max_outbox_bytes) {
                        out.error = "edge outbox load exceeded max_outbox_bytes";
                        return out;
                    }
                }

                auto parsed = rowToOutbox(row, &error);
                if (!parsed) {
                    out.error = error.empty() ? "edge outbox row parse failed" : error;
                    return out;
                }
                after_stream_seq = std::max(after_stream_seq, parsed->stream_seq);
                advanced = true;

                const auto already_acked = isFleetAcked(*parsed, &error);
                if (!already_acked) {
                    out.error = error.empty() ? "fleet ACK lookup failed" : error;
                    return out;
                }
                if (*already_acked) {
                    rememberAck(*parsed);
                    continue;
                }

                auto event = toFeedEvent(*parsed, &error);
                if (!event) {
                    out.error = error.empty() ? "edge outbox event parse failed" : error;
                    return out;
                }
                next_rows_by_id[parsed->feed_event_id] = *parsed;
                out.events.push_back(std::move(*event));
                if (out.events.size() >= config_.outbox_query_limit) break;
            }

            if (!advanced || rows->rows.size() < config_.outbox_query_limit) {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            rows_by_id_ = std::move(next_rows_by_id);
        }
        out.ok = true;
        return out;
    }

    EdgeFleetDeliveryResult sink(const EdgeFleetFeedEvent& event) {
        FeedOutboxRow row;
        uint64_t highest_before = 0;
        size_t attempt_no = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = rows_by_id_.find(event.event_id);
            if (it == rows_by_id_.end()) {
                return EdgeFleetDeliveryResult::PermanentFailure;
            }
            row = it->second;
            highest_before = highest_acked_stream_seq_;
            attempt_no = ++attempt_serial_;
        }

        std::string error;
        const auto already_acked = isFleetAcked(row, &error);
        if (!already_acked) {
            return EdgeFleetDeliveryResult::TransientFailure;
        }
        if (*already_acked) {
            rememberAck(row);
            return EdgeFleetDeliveryResult::Acked;
        }

        const int64_t ack_ts_ns = std::max(now_ns(), row.ready_ts_ns);
        const bool late_delivery = row.stream_seq <= highest_before;
        if (!execute(fleetUrl(),
                     inboxInsertSql(row, ack_ts_ns, late_delivery, attempt_no),
                     "fleet inbox insert",
                     &error)) {
            return EdgeFleetDeliveryResult::TransientFailure;
        }
        if (!execute(fleetUrl(),
                     applyEventSql(row, ack_ts_ns),
                     "fleet event materialization",
                     &error)) {
            return EdgeFleetDeliveryResult::TransientFailure;
        }
        if (!execute(fleetUrl(),
                     ackInsertSql(row, ack_ts_ns),
                     "fleet ACK insert",
                     &error)) {
            return EdgeFleetDeliveryResult::AppliedButAckFailed;
        }
        rememberAck(row);
        return EdgeFleetDeliveryResult::Acked;
    }

    bool observePass(const EdgeFleetFeedPassResult& pass, std::string* error) {
        if (!config_.record_pass_telemetry) return true;
        size_t pass_index = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            pass_index = ++telemetry_pass_index_;
        }
        return execute(fleetUrl(),
                       telemetryInsertSql(pass, pass_index, now_ns()),
                       "fleet pass telemetry insert",
                       error);
    }

private:
    const std::string& edgeUrl() const noexcept { return config_.edge_sql_url; }
    const std::string& fleetUrl() const noexcept { return config_.fleet_sql_url; }

    bool execute(const std::string& url,
                 const std::string& sql,
                 const std::string& context,
                 std::string* error) {
        if (url.empty()) return execute_local(executor_, sql, context, error);
        return execute_http(url, sql, context, error);
    }

    std::optional<SqlRows> query(const std::string& url,
                                 const std::string& sql,
                                 const std::string& context,
                                 std::string* error) {
        if (url.empty()) return query_local(executor_, sql, context, error);
        return query_http(url, sql, context, error);
    }

    std::string outboxSelectSql(uint64_t after_stream_seq) const {
        std::ostringstream sql;
        sql << "SELECT " << join_columns(outbox_columns())
            << " FROM " << config_.runtime.edge_outbox_table;
        if (after_stream_seq > 0) {
            sql << " WHERE stream_seq > " << after_stream_seq;
        }
        sql << " ORDER BY stream_seq ASC"
            << " LIMIT " << config_.outbox_query_limit;
        return sql.str();
    }

    std::optional<FeedOutboxRow> rowToOutbox(const std::vector<std::string>& row,
                                             std::string* error) const {
        if (row.size() != outbox_columns().size()) {
            if (error) {
                *error = "outbox SELECT returned " + std::to_string(row.size()) +
                         " columns, expected " +
                         std::to_string(outbox_columns().size());
            }
            return std::nullopt;
        }

        auto i64 = [&](size_t index) -> std::optional<int64_t> {
            auto parsed = parse_i64(row[index]);
            if (!parsed && error) {
                *error = "failed to parse integer column " +
                         outbox_columns()[index] + ": " + row[index];
            }
            return parsed;
        };
        auto u64 = [&](size_t index) -> std::optional<uint64_t> {
            auto parsed = parse_u64(row[index]);
            if (!parsed && error) {
                *error = "failed to parse integer column " +
                         outbox_columns()[index] + ": " + row[index];
            }
            return parsed;
        };

        FeedOutboxRow out;
        out.feed_event_id = row[0];
        auto stream_seq = u64(1);
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
        if (!stream_seq || !query_seq || !selected_action_code ||
            !unsafe_action_code || !recovery_top1_hit ||
            !avoids_risky_repeat || !risky_action_suppressed ||
            !suppressed_count || !edge_latency_ms || !retrieval_rank ||
            !quality_code || !score_micros || !action_code ||
            !raw_value_micros || !gated_value_micros ||
            !context_score_micros || !source_edge_node_id ||
            !decision_ts_ns || !ready_ts_ns) {
            return std::nullopt;
        }

        out.stream_seq = *stream_seq;
        out.event_kind = row[2];
        out.query_id = row[3];
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

    std::optional<EdgeFleetFeedEvent> toFeedEvent(const FeedOutboxRow& row,
                                                  std::string* error) const {
        auto parsed_kind = zeptodb::feeds::EdgeFleetFeedConnector::parseKind(
            row.event_kind);
        if (!parsed_kind) {
            if (error) *error = "unsupported edge/fleet event kind: " + row.event_kind;
            return std::nullopt;
        }
        EdgeFleetFeedEvent event;
        event.event_id = row.feed_event_id;
        event.stream_seq = row.stream_seq;
        event.kind = *parsed_kind;
        event.ready_ts_ns = row.ready_ts_ns;
        event.query_id = row.query_id;
        event.payload_json = "{\"source\":\"" + config_.runtime.edge_outbox_table + "\"}";
        return event;
    }

    std::optional<bool> isFleetAcked(const FeedOutboxRow& row, std::string* error) {
        std::ostringstream sql;
        sql << "SELECT feed_event_id FROM " << config_.runtime.fleet_ack_table
            << " WHERE stream_seq = " << row.stream_seq;
        auto rows = query(fleetUrl(), sql.str(), "fleet ACK lookup", error);
        if (!rows) return std::nullopt;
        return std::any_of(rows->rows.begin(), rows->rows.end(),
                           [&](const std::vector<std::string>& ack_row) {
                               return !ack_row.empty() &&
                                      ack_row[0] == row.feed_event_id;
                           });
    }

    std::string inboxInsertSql(const FeedOutboxRow& row,
                               int64_t ack_ts_ns,
                               bool late_delivery,
                               size_t attempt_no) const {
        const std::string attempt_id = row.feed_event_id + "|server_attempt|" +
                                       std::to_string(attempt_no) + "|" +
                                       std::to_string(ack_ts_ns);
        return insert_sql(
            config_.fleet_inbox_table,
            {"attempt_id",
             "feed_event_id",
             "stream_seq",
             "event_kind",
             "query_id",
             "delivery_status",
             "duplicate_delivery",
             "late_delivery",
             "attempt_no",
             "source_edge_node_id",
             "timestamp"},
            {sql_string_literal(attempt_id),
             sql_string_literal(row.feed_event_id),
             std::to_string(row.stream_seq),
             sql_string_literal(row.event_kind),
             sql_string_literal(row.query_id),
             "'delivered'",
             "0",
             late_delivery ? "1" : "0",
             std::to_string(attempt_no),
             std::to_string(row.source_edge_node_id),
             std::to_string(ack_ts_ns)});
    }

    std::string applyEventSql(const FeedOutboxRow& row, int64_t ack_ts_ns) const {
        if (row.event_kind == "decision") {
            const int64_t lag_ms =
                std::max<int64_t>(0, (ack_ts_ns - row.decision_ts_ns) / 1000000);
            return insert_sql(
                config_.fleet_decision_table,
                {"query_id",
                 "query_seq",
                 "robot_code",
                 "selected_action",
                 "selected_action_code",
                 "selected_expected_key",
                 "unsafe_action_code",
                 "recovery_top1_hit",
                 "avoids_risky_repeat",
                 "risky_action_suppressed",
                 "suppressed_count",
                 "edge_latency_ms",
                 "decision_ts_ns",
                 "consolidated_ts_ns",
                 "consolidation_lag_ms",
                 "source_edge_node_id",
                 "timestamp"},
                {sql_string_literal(row.query_id),
                 std::to_string(row.query_seq),
                 "0",
                 sql_string_literal(row.selected_action),
                 std::to_string(row.selected_action_code),
                 sql_string_literal(row.selected_expected_key),
                 std::to_string(row.unsafe_action_code),
                 std::to_string(row.recovery_top1_hit),
                 std::to_string(row.avoids_risky_repeat),
                 std::to_string(row.risky_action_suppressed),
                 std::to_string(row.suppressed_count),
                 std::to_string(row.edge_latency_ms),
                 std::to_string(row.decision_ts_ns),
                 std::to_string(ack_ts_ns),
                 std::to_string(lag_ms),
                 std::to_string(row.source_edge_node_id),
                 std::to_string(ack_ts_ns)});
        }
        if (row.event_kind == "retrieval") {
            return insert_sql(
                config_.fleet_retrieval_table,
                {"query_id",
                 "query_seq",
                 "retrieval_rank",
                 "candidate_id",
                 "suppression_key",
                 "candidate_action",
                 "quality_label",
                 "quality_code",
                 "score_micros",
                 "timestamp"},
                {sql_string_literal(row.query_id),
                 std::to_string(row.query_seq),
                 std::to_string(row.retrieval_rank),
                 sql_string_literal(row.candidate_id),
                 sql_string_literal(row.suppression_key),
                 sql_string_literal(row.action_class),
                 sql_string_literal(row.quality_label),
                 std::to_string(row.quality_code),
                 std::to_string(row.score_micros),
                 std::to_string(ack_ts_ns)});
        }
        return insert_sql(
            config_.fleet_suppression_table,
            {"query_id",
             "query_seq",
             "candidate_id",
             "suppression_key",
             "action_class",
             "action_code",
             "outcome_label",
             "raw_value_micros",
             "gated_value_micros",
             "context_score_micros",
             "source_edge_node_id",
             "timestamp"},
            {sql_string_literal(row.query_id),
             std::to_string(row.query_seq),
             sql_string_literal(row.candidate_id),
             sql_string_literal(row.suppression_key),
             sql_string_literal(row.action_class),
             std::to_string(row.action_code),
             sql_string_literal(row.outcome_label),
             std::to_string(row.raw_value_micros),
             std::to_string(row.gated_value_micros),
             std::to_string(row.context_score_micros),
             std::to_string(row.source_edge_node_id),
             std::to_string(ack_ts_ns)});
    }

    std::string ackInsertSql(const FeedOutboxRow& row, int64_t ack_ts_ns) const {
        return insert_sql(
            config_.runtime.fleet_ack_table,
            {"feed_event_id",
             "stream_seq",
             "event_kind",
             "query_id",
             "ack_status",
             "source_edge_node_id",
             "ack_ts_ns",
             "timestamp"},
            {sql_string_literal(row.feed_event_id),
             std::to_string(row.stream_seq),
             sql_string_literal(row.event_kind),
             sql_string_literal(row.query_id),
             "'acked'",
             std::to_string(row.source_edge_node_id),
             std::to_string(ack_ts_ns),
             std::to_string(ack_ts_ns)});
    }

    std::string telemetryInsertSql(const EdgeFleetFeedPassResult& pass,
                                   size_t pass_index,
                                   int64_t timestamp_ns) const {
        const size_t failed = pass.transient_failure_count +
                              pass.permanent_failure_count +
                              pass.ack_boundary_failure_count;
        return insert_sql(
            config_.fleet_telemetry_table,
            {"phase",
             "pass_index",
             "batch_event_count",
             "attempted_count",
             "delivered_count",
             "failed_count",
             "dropped_count",
             "duplicate_attempt_count",
             "late_count",
             "acked_before",
             "acked_after",
             "max_inflight",
             "batch_limit",
             "restart_reloaded_ack",
             "timestamp"},
            {"'server_runtime'",
             std::to_string(pass_index),
             std::to_string(pass.batch_event_count),
             std::to_string(pass.attempted_count),
             std::to_string(pass.acked_count),
             std::to_string(failed),
             "0",
             std::to_string(pass.duplicate_count),
             std::to_string(pass.late_count),
             std::to_string(pass.acked_before),
             std::to_string(pass.acked_after),
             std::to_string(config_.runtime.feed.max_inflight),
             std::to_string(config_.runtime.feed.batch_limit),
             "0",
             std::to_string(timestamp_ns)});
    }

    void rememberAck(const FeedOutboxRow& row) {
        std::lock_guard<std::mutex> lock(mu_);
        highest_acked_stream_seq_ =
            std::max(highest_acked_stream_seq_, row.stream_seq);
    }

    QueryExecutor& executor_;
    EdgeFleetSqlHttpAdapterConfig config_;
    std::mutex mu_;
    std::unordered_map<std::string, FeedOutboxRow> rows_by_id_;
    uint64_t highest_acked_stream_seq_ = 0;
    size_t attempt_serial_ = 0;
    size_t telemetry_pass_index_ = 0;
};

bool create_table(QueryExecutor& executor,
                  const std::string& sql,
                  const std::string& context,
                  std::string* error) {
    return execute_local(executor, sql, context, error);
}

} // namespace

bool validateEdgeFleetSqlHttpAdapterConfig(
    const EdgeFleetSqlHttpAdapterConfig& config,
    std::string* error) {
    if (!zeptodb::feeds::EdgeFleetFeedConnector::isValidConfig(config.runtime.feed)) {
        if (error) {
            *error = "batch_limit, max_inflight, max_retries_per_event, and max_failures_per_pass must be positive";
        }
        return false;
    }
    if (config.runtime.worker_poll_interval_ms == 0) {
        if (error) *error = "worker_poll_interval_ms must be positive";
        return false;
    }
    if (!require_url(config.edge_sql_url, "edge_sql_url", error) ||
        !require_url(config.fleet_sql_url, "fleet_sql_url", error)) {
        return false;
    }
    if (config.runtime.name.empty()) {
        if (error) *error = "connector name is required";
        return false;
    }
    if (config.outbox_query_limit == 0) {
        if (error) *error = "outbox_query_limit must be positive";
        return false;
    }
    if (config.max_outbox_bytes == 0) {
        if (error) *error = "max_outbox_bytes must be positive";
        return false;
    }
    return require_identifier(config.runtime.edge_outbox_table,
                              "edge_outbox_table",
                              error) &&
           require_identifier(config.runtime.fleet_ack_table,
                              "fleet_ack_table",
                              error) &&
           require_identifier(config.fleet_inbox_table,
                              "fleet_inbox_table",
                              error) &&
           require_identifier(config.fleet_decision_table,
                              "fleet_decision_table",
                              error) &&
           require_identifier(config.fleet_retrieval_table,
                              "fleet_retrieval_table",
                              error) &&
           require_identifier(config.fleet_suppression_table,
                              "fleet_suppression_table",
                              error) &&
           require_identifier(config.fleet_telemetry_table,
                              "fleet_telemetry_table",
                              error);
}

bool ensureEdgeFleetSqlHttpTables(QueryExecutor& executor,
                                  const EdgeFleetSqlHttpAdapterConfig& config,
                                  std::string* error) {
    if (!config.edge_sql_url.empty() || !config.fleet_sql_url.empty()) {
        if (error) {
            *error = "SQL/HTTP table bootstrap only supports local adapter endpoints";
        }
        return false;
    }
    if (!validateEdgeFleetSqlHttpAdapterConfig(config, error)) {
        return false;
    }

    std::ostringstream outbox;
    outbox << "CREATE TABLE IF NOT EXISTS " << config.runtime.edge_outbox_table
           << " (feed_event_id STRING, stream_seq INT64, event_kind STRING, "
              "query_id STRING, query_seq INT64, candidate_id STRING, "
              "suppression_key STRING, selected_action STRING, "
              "selected_action_code INT64, selected_expected_key STRING, "
              "unsafe_action_code INT64, recovery_top1_hit INT64, "
              "avoids_risky_repeat INT64, risky_action_suppressed INT64, "
              "suppressed_count INT64, edge_latency_ms INT64, "
              "retrieval_rank INT64, quality_label STRING, quality_code INT64, "
              "score_micros INT64, action_class STRING, action_code INT64, "
              "outcome_label STRING, raw_value_micros INT64, "
              "gated_value_micros INT64, context_score_micros INT64, "
              "source_edge_node_id INT64, decision_ts_ns TIMESTAMP_NS, "
              "ready_ts_ns TIMESTAMP_NS, timestamp TIMESTAMP_NS)";
    if (!create_table(executor, outbox.str(), "create edge outbox table", error)) {
        return false;
    }

    std::ostringstream decisions;
    decisions << "CREATE TABLE IF NOT EXISTS " << config.fleet_decision_table
              << " (query_id STRING, query_seq INT64, robot_code INT64, "
                 "selected_action STRING, selected_action_code INT64, "
                 "selected_expected_key STRING, unsafe_action_code INT64, "
                 "recovery_top1_hit INT64, avoids_risky_repeat INT64, "
                 "risky_action_suppressed INT64, suppressed_count INT64, "
                 "edge_latency_ms INT64, decision_ts_ns TIMESTAMP_NS, "
                 "consolidated_ts_ns TIMESTAMP_NS, consolidation_lag_ms INT64, "
                 "source_edge_node_id INT64, timestamp TIMESTAMP_NS)";
    if (!create_table(executor, decisions.str(), "create fleet decision table", error)) {
        return false;
    }

    std::ostringstream retrieval;
    retrieval << "CREATE TABLE IF NOT EXISTS " << config.fleet_retrieval_table
              << " (query_id STRING, query_seq INT64, retrieval_rank INT64, "
                 "candidate_id STRING, suppression_key STRING, "
                 "candidate_action STRING, quality_label STRING, "
                 "quality_code INT64, score_micros INT64, timestamp TIMESTAMP_NS)";
    if (!create_table(executor, retrieval.str(), "create fleet retrieval table", error)) {
        return false;
    }

    std::ostringstream suppressions;
    suppressions << "CREATE TABLE IF NOT EXISTS " << config.fleet_suppression_table
                 << " (query_id STRING, query_seq INT64, candidate_id STRING, "
                    "suppression_key STRING, action_class STRING, action_code INT64, "
                    "outcome_label STRING, raw_value_micros INT64, "
                    "gated_value_micros INT64, context_score_micros INT64, "
                    "source_edge_node_id INT64, timestamp TIMESTAMP_NS)";
    if (!create_table(executor,
                      suppressions.str(),
                      "create fleet suppression table",
                      error)) {
        return false;
    }

    std::ostringstream inbox;
    inbox << "CREATE TABLE IF NOT EXISTS " << config.fleet_inbox_table
          << " (attempt_id STRING, feed_event_id STRING, stream_seq INT64, "
             "event_kind STRING, query_id STRING, delivery_status STRING, "
             "duplicate_delivery INT64, late_delivery INT64, attempt_no INT64, "
             "source_edge_node_id INT64, timestamp TIMESTAMP_NS)";
    if (!create_table(executor, inbox.str(), "create fleet inbox table", error)) {
        return false;
    }

    std::ostringstream ack;
    ack << "CREATE TABLE IF NOT EXISTS " << config.runtime.fleet_ack_table
        << " (feed_event_id STRING, stream_seq INT64, event_kind STRING, "
           "query_id STRING, ack_status STRING, source_edge_node_id INT64, "
           "ack_ts_ns TIMESTAMP_NS, timestamp TIMESTAMP_NS)";
    if (!create_table(executor, ack.str(), "create fleet ACK table", error)) {
        return false;
    }

    std::ostringstream telemetry;
    telemetry << "CREATE TABLE IF NOT EXISTS " << config.fleet_telemetry_table
              << " (phase STRING, pass_index INT64, batch_event_count INT64, "
                 "attempted_count INT64, delivered_count INT64, failed_count INT64, "
                 "dropped_count INT64, duplicate_attempt_count INT64, "
                 "late_count INT64, acked_before INT64, acked_after INT64, "
                 "max_inflight INT64, batch_limit INT64, "
                 "restart_reloaded_ack INT64, timestamp TIMESTAMP_NS)";
    return create_table(executor, telemetry.str(), "create fleet telemetry table", error);
}

EdgeFleetConnectorRuntimeHooks makeEdgeFleetSqlHttpRuntimeHooks(
    QueryExecutor& executor,
    EdgeFleetSqlHttpAdapterConfig config) {
    auto state = std::make_shared<EdgeFleetSqlHttpAdapterState>(
        executor, std::move(config));

    EdgeFleetConnectorRuntimeHooks hooks;
    hooks.load_outbox = [state] {
        return state->loadOutbox();
    };
    hooks.sink = [state](const EdgeFleetFeedEvent& event) {
        return state->sink(event);
    };
    hooks.observe_pass = [state](const EdgeFleetFeedPassResult& pass,
                                 std::string* error) {
        return state->observePass(pass, error);
    };
    return hooks;
}

} // namespace zeptodb::server
