// ============================================================================
// ZeptoDB: Arrow Flight Server — Implementation
// ============================================================================
#include "zeptodb/server/flight_server.h"
#include "zeptodb/server/arrow_ipc.h"
#include "zeptodb/common/logger.h"

#include <utility>

#ifdef ZEPTO_FLIGHT_ENABLED

#include "zeptodb/sql/parser.h"

#include <arrow/api.h>
#include <arrow/builder.h>
#include <arrow/flight/api.h>
#include <arrow/flight/server.h>
#include <arrow/ipc/writer.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>
#include <arrow/util/byte_size.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace flight = arrow::flight;

namespace zeptodb::server {
namespace {

struct CallIdentity {
    zeptodb::auth::AuthContext context;
    bool enforced = false;
};

bool ascii_case_equal(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> authorization_header(
    const flight::ServerCallContext& call,
    bool* duplicate) {
    *duplicate = false;
    std::optional<std::string> value;
    for (const auto& [name, header_value] : call.incoming_headers()) {
        if (!ascii_case_equal(name, "authorization")) continue;
        if (value) {
            *duplicate = true;
            return std::nullopt;
        }
        value = std::string{header_value};
    }
    return value;
}

std::string normalized_peer_address(std::string_view peer) {
    if (peer.starts_with("ipv4:")) {
        peer.remove_prefix(5);
        const size_t port = peer.rfind(':');
        return std::string{peer.substr(0, port)};
    }
    if (peer.starts_with("ipv6:")) {
        peer.remove_prefix(5);
        if (peer.starts_with('[')) {
            const size_t close = peer.find(']');
            if (close != std::string_view::npos) {
                return std::string{peer.substr(1, close - 1)};
            }
        }
        const size_t port = peer.rfind(':');
        return std::string{peer.substr(0, port)};
    }
    return std::string{peer};
}

bool is_loopback_host(std::string_view host) {
    if (ascii_case_equal(host, "localhost") || host == "::1" ||
        host == "[::1]") {
        return true;
    }
    std::array<unsigned int, 4> octets{};
    size_t start = 0;
    for (size_t index = 0; index < octets.size(); ++index) {
        const size_t end = host.find('.', start);
        if ((index < octets.size() - 1 && end == std::string_view::npos) ||
            (index == octets.size() - 1 && end != std::string_view::npos)) {
            return false;
        }
        const auto segment = host.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                  : end - start);
        if (segment.empty() || segment.size() > 3) return false;
        unsigned int value = 0;
        for (const unsigned char ch : segment) {
            if (std::isdigit(ch) == 0) return false;
            value = value * 10 + static_cast<unsigned int>(ch - '0');
        }
        if (value > 255) return false;
        octets[index] = value;
        start = end == std::string_view::npos ? host.size() : end + 1;
    }
    return octets[0] == 127;
}

std::optional<std::string> read_pem_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) return std::nullopt;
    auto pem = contents.str();
    if (pem.empty()) return std::nullopt;
    return pem;
}

bool valid_table_name(std::string_view table) {
    if (table.empty()) return false;
    bool segment_start = true;
    for (const unsigned char ch : table) {
        if (ch == '.') {
            if (segment_start) return false;
            segment_start = true;
            continue;
        }
        if (segment_start) {
            if (std::isalpha(ch) == 0 && ch != '_') return false;
            segment_start = false;
        } else if (std::isalnum(ch) == 0 && ch != '_') {
            return false;
        }
    }
    return !segment_start;
}

std::string sql_string_literal(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') escaped.push_back('\'');
        escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
}

void append_unique_table(std::vector<std::string>* tables,
                         const std::string& table) {
    if (table.empty() ||
        std::find(tables->begin(), tables->end(), table) != tables->end()) {
        return;
    }
    tables->push_back(table);
}

void collect_select_tables(
    const zeptodb::sql::SelectStmt& select,
    std::unordered_set<std::string> cte_names,
    std::vector<std::string>* tables);

void collect_expr_tables(
    const std::shared_ptr<zeptodb::sql::Expr>& expression,
    const std::unordered_set<std::string>& cte_names,
    std::vector<std::string>* tables) {
    if (!expression) return;
    if (expression->subquery) {
        collect_select_tables(*expression->subquery, cte_names, tables);
    }
    collect_expr_tables(expression->left, cte_names, tables);
    collect_expr_tables(expression->right, cte_names, tables);
}

void collect_select_tables(
    const zeptodb::sql::SelectStmt& select,
    std::unordered_set<std::string> cte_names,
    std::vector<std::string>* tables) {
    // CTEs are visible sequentially. Traverse each body before adding its own
    // name so a later CTE cannot shadow a physical table in an earlier body.
    for (const auto& cte : select.cte_defs) {
        if (cte.stmt) collect_select_tables(*cte.stmt, cte_names, tables);
        cte_names.insert(cte.name);
    }

    if (!select.from_table.empty() && !cte_names.contains(select.from_table)) {
        append_unique_table(tables, select.from_table);
    }
    if (select.join && !select.join->table.empty() &&
        !cte_names.contains(select.join->table)) {
        append_unique_table(tables, select.join->table);
    }
    if (select.from_subquery) {
        collect_select_tables(*select.from_subquery, cte_names, tables);
    }
    if (select.rhs) collect_select_tables(*select.rhs, cte_names, tables);
    if (select.where) {
        collect_expr_tables(select.where->expr, cte_names, tables);
    }
    if (select.having) {
        collect_expr_tables(select.having->expr, cte_names, tables);
    }
    for (const auto& column : select.columns) {
        if (!column.case_when) continue;
        for (const auto& branch : column.case_when->branches) {
            collect_expr_tables(branch.when_cond, cte_names, tables);
        }
    }
}

std::vector<std::string> select_tables(
    const zeptodb::sql::SelectStmt& select) {
    std::vector<std::string> tables;
    collect_select_tables(select, {}, &tables);
    return tables;
}

arrow::Status unauthenticated(std::string message) {
    return flight::MakeFlightError(
        flight::FlightStatusCode::Unauthenticated, std::move(message));
}

arrow::Status unauthorized(std::string message) {
    return flight::MakeFlightError(
        flight::FlightStatusCode::Unauthorized, std::move(message));
}

struct FlightRecordBatch {
    std::shared_ptr<arrow::Schema> schema;
    std::shared_ptr<arrow::RecordBatch> batch;
};

arrow::Result<FlightRecordBatch> make_flight_record_batch(
    const zeptodb::sql::QueryResultSet& result,
    zeptodb::sql::ParsedStatement::Kind statement_kind) {
    using Kind = zeptodb::sql::ParsedStatement::Kind;

    if (statement_kind == Kind::SHOW_TABLES) {
        if (result.string_rows.size() != result.rows.size()) {
            return arrow::Status::ExecutionError(
                "SHOW TABLES returned inconsistent row metadata");
        }
        arrow::StringBuilder names;
        arrow::Int64Builder counts;
        ARROW_RETURN_NOT_OK(
            names.Reserve(static_cast<int64_t>(result.string_rows.size())));
        ARROW_RETURN_NOT_OK(
            counts.Reserve(static_cast<int64_t>(result.rows.size())));
        for (size_t index = 0; index < result.string_rows.size(); ++index) {
            if (result.rows[index].empty()) {
                return arrow::Status::ExecutionError(
                    "SHOW TABLES row count is missing");
            }
            ARROW_RETURN_NOT_OK(names.Append(result.string_rows[index]));
            ARROW_RETURN_NOT_OK(counts.Append(result.rows[index][0]));
        }
        ARROW_ASSIGN_OR_RAISE(auto name_array, names.Finish());
        ARROW_ASSIGN_OR_RAISE(auto count_array, counts.Finish());
        const auto schema = arrow::schema({
            arrow::field("name", arrow::utf8()),
            arrow::field("rows", arrow::int64()),
        });
        return FlightRecordBatch{
            schema,
            arrow::RecordBatch::Make(
                schema, static_cast<int64_t>(result.rows.size()),
                {std::move(name_array), std::move(count_array)})};
    }

    if (statement_kind == Kind::DESCRIBE_TABLE) {
        if (result.string_rows.size() != result.rows.size() * 2) {
            return arrow::Status::ExecutionError(
                "DESCRIBE returned inconsistent row metadata");
        }
        arrow::StringBuilder columns;
        arrow::StringBuilder types;
        ARROW_RETURN_NOT_OK(
            columns.Reserve(static_cast<int64_t>(result.rows.size())));
        ARROW_RETURN_NOT_OK(
            types.Reserve(static_cast<int64_t>(result.rows.size())));
        for (size_t index = 0; index < result.rows.size(); ++index) {
            ARROW_RETURN_NOT_OK(columns.Append(result.string_rows[index * 2]));
            ARROW_RETURN_NOT_OK(types.Append(result.string_rows[index * 2 + 1]));
        }
        ARROW_ASSIGN_OR_RAISE(auto column_array, columns.Finish());
        ARROW_ASSIGN_OR_RAISE(auto type_array, types.Finish());
        const auto schema = arrow::schema({
            arrow::field("column", arrow::utf8()),
            arrow::field("type", arrow::utf8()),
        });
        return FlightRecordBatch{
            schema,
            arrow::RecordBatch::Make(
                schema, static_cast<int64_t>(result.rows.size()),
                {std::move(column_array), std::move(type_array)})};
    }

    if (!result.string_rows.empty() && result.rows.empty() &&
        result.typed_rows.empty()) {
        arrow::StringBuilder lines;
        ARROW_RETURN_NOT_OK(
            lines.Reserve(static_cast<int64_t>(result.string_rows.size())));
        for (const auto& line : result.string_rows) {
            ARROW_RETURN_NOT_OK(lines.Append(line));
        }
        ARROW_ASSIGN_OR_RAISE(auto line_array, lines.Finish());
        const std::string column_name = result.column_names.empty()
            ? "text"
            : result.column_names.front();
        const auto schema =
            arrow::schema({arrow::field(column_name, arrow::utf8())});
        return FlightRecordBatch{
            schema,
            arrow::RecordBatch::Make(
                schema, static_cast<int64_t>(result.string_rows.size()),
                {std::move(line_array)})};
    }

    const auto schema = zeptodb::server::build_arrow_schema(result);
    ARROW_ASSIGN_OR_RAISE(
        auto batch,
        zeptodb::server::result_to_record_batch(result, schema));
    return FlightRecordBatch{schema, std::move(batch)};
}

}  // namespace

// ============================================================================
// ZeptoFlightServer — FlightServerBase implementation
// ============================================================================
class ZeptoFlightServer final : public flight::FlightServerBase {
public:
    ZeptoFlightServer(
        zeptodb::sql::QueryExecutor& executor,
        std::shared_ptr<zeptodb::auth::AuthManager> auth,
        std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager)
        : executor_(executor),
          auth_(std::move(auth)),
          tenant_manager_(std::move(tenant_manager)) {}

    void set_limits(const FlightServerConfig& config) {
        max_query_rows_ = config.max_query_rows;
        max_query_bytes_ = config.max_query_bytes;
        query_timeout_ms_ = config.query_timeout_ms;
        max_put_rows_ = config.max_put_rows;
        max_put_bytes_ = config.max_put_bytes;
        allow_non_atomic_put_ = config.allow_non_atomic_put;
    }

    arrow::Status GetFlightInfo(
        const flight::ServerCallContext& call,
        const flight::FlightDescriptor& desc,
        std::unique_ptr<flight::FlightInfo>* info) override {
        if (desc.type != flight::FlightDescriptor::CMD) {
            return arrow::Status::Invalid("Only CMD descriptors supported");
        }

        const std::string sql{desc.cmd};
        zeptodb::sql::ParsedStatement statement;
        CallIdentity identity;
        ARROW_RETURN_NOT_OK(authorize_read_sql(
            call, "GetFlightInfo", sql, &statement, &identity));
        ARROW_ASSIGN_OR_RAISE(
            auto tenant_slot,
            acquire_tenant_query_slot(call, "GetFlightInfo", identity));

        const auto statement_kind = statement.kind;
        bound_read_statement(&statement);
        ARROW_ASSIGN_OR_RAISE(
            auto result,
            execute_with_timeout(sql, std::move(statement)));
        if (!result.ok()) {
            return arrow::Status::ExecutionError(result.error);
        }
        ARROW_RETURN_NOT_OK(check_query_limits(result));
        if (statement_kind ==
            zeptodb::sql::ParsedStatement::Kind::SHOW_TABLES) {
            ARROW_RETURN_NOT_OK(filter_show_tables(identity, &result));
        }

        ARROW_ASSIGN_OR_RAISE(
            auto flight_batch,
            make_flight_record_batch(result, statement_kind));
        if (static_cast<size_t>(std::max<int64_t>(
                arrow::util::TotalBufferSize(*flight_batch.batch), 0)) >
            max_query_bytes_) {
            return arrow::Status::CapacityError(
                "Flight query byte limit exceeded");
        }

        flight::FlightEndpoint endpoint;
        endpoint.ticket.ticket = sql;
        ARROW_ASSIGN_OR_RAISE(
            auto flight_info,
            flight::FlightInfo::Make(
                *flight_batch.schema, desc, {endpoint},
                flight_batch.batch->num_rows(), -1));
        *info = std::make_unique<flight::FlightInfo>(std::move(flight_info));
        return arrow::Status::OK();
    }

    arrow::Status DoGet(
        const flight::ServerCallContext& call,
        const flight::Ticket& ticket,
        std::unique_ptr<flight::FlightDataStream>* stream) override {
        const std::string sql{ticket.ticket};
        zeptodb::sql::ParsedStatement statement;
        CallIdentity identity;
        ARROW_RETURN_NOT_OK(authorize_read_sql(
            call, "DoGet", sql, &statement, &identity));
        ARROW_ASSIGN_OR_RAISE(
            auto tenant_slot,
            acquire_tenant_query_slot(call, "DoGet", identity));

        const auto statement_kind = statement.kind;
        bound_read_statement(&statement);
        ARROW_ASSIGN_OR_RAISE(
            auto result,
            execute_with_timeout(sql, std::move(statement)));
        if (!result.ok()) {
            return arrow::Status::ExecutionError(result.error);
        }
        ARROW_RETURN_NOT_OK(check_query_limits(result));
        if (statement_kind ==
            zeptodb::sql::ParsedStatement::Kind::SHOW_TABLES) {
            ARROW_RETURN_NOT_OK(filter_show_tables(identity, &result));
        }

        ARROW_ASSIGN_OR_RAISE(
            auto flight_batch,
            make_flight_record_batch(result, statement_kind));
        if (static_cast<size_t>(std::max<int64_t>(
                arrow::util::TotalBufferSize(*flight_batch.batch), 0)) >
            max_query_bytes_) {
            return arrow::Status::CapacityError(
                "Flight query byte limit exceeded");
        }
        auto reader = arrow::RecordBatchReader::Make(
            {flight_batch.batch}, flight_batch.schema);
        if (!reader.ok()) return reader.status();

        *stream = std::make_unique<flight::RecordBatchStream>(*reader);
        return arrow::Status::OK();
    }

    arrow::Status DoPut(
        const flight::ServerCallContext& call,
        std::unique_ptr<flight::FlightMessageReader> reader,
        std::unique_ptr<flight::FlightMetadataWriter>) override {
        CallIdentity identity;
        ARROW_RETURN_NOT_OK(authenticate(
            call, "DoPut", zeptodb::auth::Permission::WRITE, &identity));

        if (!allow_non_atomic_put_) {
            return arrow::Status::NotImplemented(
                "Flight DoPut is disabled because atomic stream commit is "
                "not available; enable the experimental non-atomic mode "
                "only with explicit data-loss acceptance");
        }
        ARROW_ASSIGN_OR_RAISE(
            auto tenant_slot,
            acquire_tenant_query_slot(call, "DoPut", identity));

        const auto& descriptor = reader->descriptor();
        if (descriptor.type != flight::FlightDescriptor::CMD) {
            return arrow::Status::Invalid(
                "DoPut requires CMD descriptor with table name");
        }
        const std::string table_name{descriptor.cmd};
        if (!valid_table_name(table_name)) {
            return arrow::Status::Invalid("Invalid DoPut table name");
        }
        ARROW_RETURN_NOT_OK(authorize_table(
            call, "DoPut", identity, table_name));

        flight::FlightStreamChunk chunk;
        size_t total_rows = 0;
        size_t total_bytes = 0;
        while (true) {
            ARROW_ASSIGN_OR_RAISE(chunk, reader->Next());
            if (!chunk.data) break;

            const auto& batch = chunk.data;
            const int64_t row_count = batch->num_rows();
            const int64_t batch_bytes = arrow::util::TotalBufferSize(*batch);
            if (row_count < 0 || batch_bytes < 0 ||
                static_cast<uint64_t>(row_count) >
                    max_put_rows_ - std::min(total_rows, max_put_rows_) ||
                static_cast<uint64_t>(batch_bytes) >
                    max_put_bytes_ - std::min(total_bytes, max_put_bytes_)) {
                return arrow::Status::CapacityError(
                    "Flight DoPut row or byte limit exceeded");
            }
            const int column_count = batch->num_columns();
            for (int64_t row = 0; row < row_count; ++row) {
                std::string sql = "INSERT INTO " + table_name + " VALUES (";
                for (int column = 0; column < column_count; ++column) {
                    if (column > 0) sql += ", ";
                    const auto& values = batch->column(column);
                    if (values->IsNull(row)) {
                        return arrow::Status::Invalid(
                            "DoPut does not support NULL values");
                    }

                    switch (values->type_id()) {
                        case arrow::Type::INT64: {
                            const auto array =
                                std::static_pointer_cast<arrow::Int64Array>(values);
                            sql += std::to_string(array->Value(row));
                            break;
                        }
                        case arrow::Type::DOUBLE: {
                            const auto array =
                                std::static_pointer_cast<arrow::DoubleArray>(values);
                            const double value = array->Value(row);
                            if (!std::isfinite(value)) {
                                return arrow::Status::Invalid(
                                    "DoPut rejects non-finite floating-point values");
                            }
                            sql += std::to_string(value);
                            break;
                        }
                        case arrow::Type::FLOAT: {
                            const auto array =
                                std::static_pointer_cast<arrow::FloatArray>(values);
                            const float value = array->Value(row);
                            if (!std::isfinite(value)) {
                                return arrow::Status::Invalid(
                                    "DoPut rejects non-finite floating-point values");
                            }
                            sql += std::to_string(value);
                            break;
                        }
                        case arrow::Type::STRING: {
                            const auto array =
                                std::static_pointer_cast<arrow::StringArray>(values);
                            sql += sql_string_literal(array->GetString(row));
                            break;
                        }
                        default:
                            return arrow::Status::Invalid(
                                "Unsupported Arrow type in DoPut: ",
                                values->type()->ToString());
                    }
                }
                sql += ')';
                const auto result = executor_.execute(sql);
                if (!result.ok()) {
                    return arrow::Status::ExecutionError(result.error);
                }
            }
            total_rows += static_cast<size_t>(row_count);
            total_bytes += static_cast<size_t>(batch_bytes);
        }
        ZEPTO_INFO("Flight DoPut: table={}, rows={}", table_name, total_rows);
        return arrow::Status::OK();
    }

    arrow::Status ListFlights(
        const flight::ServerCallContext& call,
        const flight::Criteria*,
        std::unique_ptr<flight::FlightListing>* listings) override {
        CallIdentity identity;
        ARROW_RETURN_NOT_OK(authenticate(
            call, "ListFlights", zeptodb::auth::Permission::READ, &identity));
        ARROW_ASSIGN_OR_RAISE(
            auto tenant_slot,
            acquire_tenant_query_slot(call, "ListFlights", identity));

        ARROW_ASSIGN_OR_RAISE(
            const auto tables_result, execute_with_timeout("SHOW TABLES"));
        if (!tables_result.ok()) {
            return arrow::Status::ExecutionError(tables_result.error);
        }

        std::vector<flight::FlightInfo> infos;
        infos.reserve(tables_result.string_rows.size());
        for (const auto& table : tables_result.string_rows) {
            if (!table_visible(identity, table)) continue;

            ARROW_ASSIGN_OR_RAISE(
                const auto count_result,
                execute_with_timeout("SELECT COUNT(*) FROM " + table));
            if (!count_result.ok()) {
                return arrow::Status::ExecutionError(count_result.error);
            }
            int64_t row_count = 0;
            if (!count_result.rows.empty() && !count_result.rows[0].empty()) {
                row_count = count_result.rows[0][0];
            }

            const auto descriptor = flight::FlightDescriptor::Command(table);
            const auto schema =
                arrow::schema({arrow::field("info", arrow::utf8())});
            ARROW_ASSIGN_OR_RAISE(
                auto flight_info,
                flight::FlightInfo::Make(
                    *schema, descriptor, {}, row_count, -1));
            infos.push_back(std::move(flight_info));
        }

        *listings =
            std::make_unique<flight::SimpleFlightListing>(std::move(infos));
        return arrow::Status::OK();
    }

    arrow::Status DoAction(
        const flight::ServerCallContext&,
        const flight::Action& action,
        std::unique_ptr<flight::ResultStream>* result) override {
        if (action.type == "ping" || action.type == "healthcheck") {
            const auto body = arrow::Buffer::FromString("{\"status\":\"ok\"}");
            std::vector<flight::Result> results;
            results.push_back(flight::Result{body});
            *result = std::make_unique<flight::SimpleResultStream>(
                std::move(results));
            return arrow::Status::OK();
        }
        return arrow::Status::NotImplemented("Unknown action: ", action.type);
    }

    arrow::Status ListActions(
        const flight::ServerCallContext&,
        std::vector<flight::ActionType>* actions) override {
        *actions = {
            {"ping", "Health check"},
            {"healthcheck", "Health check (alias)"},
        };
        return arrow::Status::OK();
    }

private:
    struct TenantQuerySlotGuard {
        std::shared_ptr<zeptodb::auth::TenantManager> manager;
        std::string tenant_id;

        ~TenantQuerySlotGuard() {
            if (manager) manager->release_query_slot(tenant_id);
        }
    };

    arrow::Result<std::unique_ptr<TenantQuerySlotGuard>>
    acquire_tenant_query_slot(
        const flight::ServerCallContext& call,
        std::string_view rpc,
        const CallIdentity& identity) const {
        if (!identity.enforced || identity.context.tenant_id.empty()) {
            return std::unique_ptr<TenantQuerySlotGuard>{};
        }
        if (!tenant_manager_ ||
            !tenant_manager_->acquire_query_slot(identity.context.tenant_id)) {
            audit_denial(
                call, rpc, identity.context, "tenant-concurrency-limit");
            return flight::MakeFlightError(
                flight::FlightStatusCode::Unavailable,
                "Tenant Flight query concurrency limit exceeded");
        }
        auto guard = std::make_unique<TenantQuerySlotGuard>();
        guard->manager = tenant_manager_;
        guard->tenant_id = identity.context.tenant_id;
        return guard;
    }

    arrow::Result<zeptodb::sql::QueryResultSet> execute_with_timeout(
        std::string sql,
        std::optional<zeptodb::sql::ParsedStatement> statement =
            std::nullopt) const {
        auto execute = [this, &sql, &statement](
                           zeptodb::auth::CancellationToken* token) {
            if (statement) {
                return executor_.execute_parsed(
                    sql, std::move(*statement), token);
            }
            return executor_.execute(sql, token);
        };
        if (query_timeout_ms_ == 0) return execute(nullptr);

        auto token =
            std::make_shared<zeptodb::auth::CancellationToken>();
        auto future = std::async(
            std::launch::async,
            [execute = std::move(execute), token]() mutable {
                return execute(token.get());
            });
        if (future.wait_for(std::chrono::milliseconds(query_timeout_ms_)) ==
            std::future_status::timeout) {
            token->cancel();
            // Cancellation is cooperative. Waiting prevents the executor and
            // referenced request state from outliving this RPC.
            future.wait();
            return flight::MakeFlightError(
                flight::FlightStatusCode::TimedOut,
                "Flight query timed out");
        }
        return future.get();
    }

    arrow::Status authenticate(
        const flight::ServerCallContext& call,
        std::string_view rpc,
        zeptodb::auth::Permission permission,
        CallIdentity* identity) const {
        if (!auth_) {
            identity->context.subject = "anonymous";
            identity->context.name = "anonymous";
            identity->context.role = zeptodb::auth::Role::ADMIN;
            identity->context.source = "disabled";
            identity->enforced = false;
            return arrow::Status::OK();
        }

        bool duplicate = false;
        const auto header = authorization_header(call, &duplicate);
        if (duplicate) {
            return unauthenticated("Multiple Authorization metadata values");
        }

        const std::string path = "/flight/" + std::string{rpc};
        const std::string peer = normalized_peer_address(call.peer());
        auto decision = auth_->check(
            "FLIGHT", path, header.value_or(""), peer);
        if (decision.status == zeptodb::auth::AuthStatus::UNAUTHORIZED) {
            return unauthenticated(decision.reason);
        }
        if (decision.status == zeptodb::auth::AuthStatus::FORBIDDEN) {
            return unauthorized(decision.reason);
        }

        identity->context = std::move(decision.context);
        identity->enforced = identity->context.source != "disabled";
        if (identity->enforced &&
            !identity->context.has_permission(permission)) {
            audit_denial(call, rpc, identity->context, "permission-forbidden");
            return unauthorized("Required Flight permission is missing");
        }

        if (identity->enforced &&
            !identity->context.allowed_symbols.empty()) {
            audit_denial(call, rpc, identity->context,
                         "symbol-scope-unsupported-forbidden");
            return unauthorized(
                "Symbol-scoped Flight data access is not available");
        }

        if (identity->enforced && !identity->context.tenant_id.empty()) {
            if (!tenant_manager_ ||
                !tenant_manager_->get_tenant(identity->context.tenant_id)) {
                audit_denial(
                    call, rpc, identity->context, "tenant-policy-forbidden");
                return unauthorized("Tenant policy is unavailable");
            }
        }
        return arrow::Status::OK();
    }

    arrow::Status authorize_read_sql(
        const flight::ServerCallContext& call,
        std::string_view rpc,
        const std::string& sql,
        zeptodb::sql::ParsedStatement* statement,
        CallIdentity* identity) const {
        ARROW_RETURN_NOT_OK(authenticate(
            call, rpc, zeptodb::auth::Permission::READ, identity));

        try {
            zeptodb::sql::Parser parser;
            *statement = parser.parse_statement(sql);
        } catch (const std::exception&) {
            audit_denial(call, rpc, identity->context,
                         "sql-unclassified-forbidden");
            return unauthorized("Flight query could not be authorized");
        }

        using Kind = zeptodb::sql::ParsedStatement::Kind;
        switch (statement->kind) {
            case Kind::SELECT:
                if (!statement->select) {
                    audit_denial(call, rpc, identity->context,
                                 "sql-unclassified-forbidden");
                    return unauthorized("Flight query could not be authorized");
                }
                for (const auto& table : select_tables(*statement->select)) {
                    ARROW_RETURN_NOT_OK(authorize_table(
                        call, rpc, *identity, table));
                }
                return arrow::Status::OK();
            case Kind::SHOW_TABLES:
                return arrow::Status::OK();
            case Kind::DESCRIBE_TABLE:
                if (statement->describe_table_name.empty()) {
                    return unauthorized("Flight query could not be authorized");
                }
                return authorize_table(
                    call, rpc, *identity, statement->describe_table_name);
            default:
                audit_denial(call, rpc, identity->context,
                             "sql-mutation-forbidden");
                return unauthorized(
                    "Flight query tickets accept read-only SQL only");
        }
    }

    arrow::Status authorize_table(
        const flight::ServerCallContext& call,
        std::string_view rpc,
        const CallIdentity& identity,
        const std::string& table) const {
        if (table_visible(identity, table)) return arrow::Status::OK();

        const bool table_acl_denied = identity.enforced &&
            !identity.context.can_access_table(table);
        audit_denial(
            call, rpc, identity.context,
            table_acl_denied ? "table-forbidden" : "tenant-table-forbidden");
        return unauthorized("Flight table access denied");
    }

    bool table_visible(
        const CallIdentity& identity,
        const std::string& table) const {
        if (!identity.enforced) return true;
        if (!identity.context.can_access_table(table)) return false;
        if (identity.context.tenant_id.empty()) return true;
        return tenant_manager_ &&
            tenant_manager_->can_access_table(
                identity.context.tenant_id, table);
    }

    arrow::Status filter_show_tables(
        const CallIdentity& identity,
        zeptodb::sql::QueryResultSet* result) const {
        if (result->string_rows.size() != result->rows.size()) {
            return arrow::Status::ExecutionError(
                "SHOW TABLES returned inconsistent row metadata");
        }

        std::vector<std::string> visible_names;
        std::vector<std::vector<int64_t>> visible_rows;
        visible_names.reserve(result->string_rows.size());
        visible_rows.reserve(result->rows.size());
        for (size_t index = 0; index < result->string_rows.size(); ++index) {
            if (!table_visible(identity, result->string_rows[index])) continue;
            visible_names.push_back(std::move(result->string_rows[index]));
            visible_rows.push_back(std::move(result->rows[index]));
        }
        result->string_rows = std::move(visible_names);
        result->rows = std::move(visible_rows);
        return arrow::Status::OK();
    }

    void audit_denial(
        const flight::ServerCallContext& call,
        std::string_view rpc,
        const zeptodb::auth::AuthContext& context,
        const std::string& detail) const {
        if (!auth_ || context.source == "disabled") return;
        auth_->audit(
            context, "FLIGHT /flight/" + std::string{rpc}, detail,
            normalized_peer_address(call.peer()));
    }

    void bound_read_statement(
        zeptodb::sql::ParsedStatement* statement) const {
        if (!statement->select) return;
        const size_t detection_limit =
            max_query_rows_ == std::numeric_limits<size_t>::max()
                ? max_query_rows_
                : max_query_rows_ + 1;
        const int64_t bounded = static_cast<int64_t>(std::min(
            detection_limit,
            static_cast<size_t>(std::numeric_limits<int64_t>::max())));
        if (!statement->select->limit ||
            *statement->select->limit > bounded) {
            statement->select->limit = bounded;
        }
    }

    arrow::Status check_query_limits(
        const zeptodb::sql::QueryResultSet& result) const {
        const size_t rows = std::max(
            result.rows.size(),
            std::max(result.typed_rows.size(), result.string_rows.size()));
        if (rows > max_query_rows_) {
            return arrow::Status::CapacityError(
                "Flight query row limit exceeded");
        }
        return arrow::Status::OK();
    }

    zeptodb::sql::QueryExecutor& executor_;
    std::shared_ptr<zeptodb::auth::AuthManager> auth_;
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager_;
    size_t max_query_rows_ = 100000;
    size_t max_query_bytes_ = 64U * 1024U * 1024U;
    uint32_t query_timeout_ms_ = 30000;
    size_t max_put_rows_ = 100000;
    size_t max_put_bytes_ = 64U * 1024U * 1024U;
    bool allow_non_atomic_put_ = false;
};

// ============================================================================
// FlightServer pimpl and lifecycle
// ============================================================================
struct FlightServer::Impl {
    std::unique_ptr<ZeptoFlightServer> server;
    bool initialized = false;
};

FlightServer::FlightServer(
    zeptodb::sql::QueryExecutor& executor,
    std::shared_ptr<zeptodb::auth::AuthManager> auth,
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager)
    : impl_(std::make_unique<Impl>()),
      executor_(executor),
      auth_(std::move(auth)),
      tenant_manager_(std::move(tenant_manager)) {
    impl_->server = std::make_unique<ZeptoFlightServer>(
        executor_, auth_, tenant_manager_);
}

FlightServer::~FlightServer() { stop(); }

bool FlightServer::initialize(const FlightServerConfig& config) {
    if (impl_->initialized || thread_.joinable() || running_.load()) {
        last_error_ = "Flight server cannot be started more than once";
        return false;
    }
    if (config.host.empty()) {
        last_error_ = "Flight bind host must not be empty";
        return false;
    }
    if (config.max_query_rows == 0 || config.max_query_bytes == 0 ||
        config.max_put_rows == 0 || config.max_put_bytes == 0) {
        last_error_ = "Flight row and byte limits must be non-zero";
        return false;
    }
    impl_->server->set_limits(config);

    const bool has_cert = !config.tls_cert_path.empty();
    const bool has_key = !config.tls_key_path.empty();
    if (has_cert != has_key) {
        last_error_ = "Flight TLS certificate and key must be configured together";
        return false;
    }
    if (!has_cert && !is_loopback_host(config.host) &&
        !config.allow_insecure_non_loopback) {
        last_error_ =
            "Plaintext Flight is restricted to loopback; configure TLS or "
            "use the explicit development-only insecure override";
        return false;
    }

    std::optional<std::string> cert;
    std::optional<std::string> key;
    if (has_cert) {
        cert = read_pem_file(config.tls_cert_path);
        key = read_pem_file(config.tls_key_path);
        if (!cert || !key) {
            last_error_ = "Failed to read non-empty Flight TLS PEM files";
            return false;
        }
    } else if (!is_loopback_host(config.host)) {
        ZEPTO_WARN(
            "Flight plaintext non-loopback listener enabled by explicit "
            "development override: host={}",
            config.host);
    }

    const auto location = has_cert
        ? flight::Location::ForGrpcTls(config.host, config.port)
        : flight::Location::ForGrpcTcp(config.host, config.port);
    if (!location.ok()) {
        last_error_ = "Failed to create Flight location: " +
            location.status().ToString();
        return false;
    }

    flight::FlightServerOptions options(*location);
    if (has_cert) {
        options.tls_certificates.push_back(
            flight::CertKeyPair{std::move(*cert), std::move(*key)});
    }
    const auto status = impl_->server->Init(options);
    if (!status.ok()) {
        last_error_ = "Flight listener initialization failed: " +
            status.ToString();
        return false;
    }

    port_ = impl_->server->port();
    impl_->initialized = true;
    last_error_.clear();
    ZEPTO_INFO(
        "Arrow Flight server initialized on {}://{}:{}",
        has_cert ? "grpc+tls" : "grpc", config.host, port_);
    return true;
}

void FlightServer::serve() {
    const auto status = impl_->server->Serve();
    if (!status.ok()) {
        last_error_ = "Flight server failed: " + status.ToString();
        ZEPTO_WARN("{}", last_error_);
    }
    running_.store(false);
}

void FlightServer::start(uint16_t port) {
    FlightServerConfig config;
    config.port = port;
    (void)start(config);
}

bool FlightServer::start(const FlightServerConfig& config) {
    if (!initialize(config)) {
        ZEPTO_WARN("Flight start rejected: {}", last_error_);
        return false;
    }
    running_.store(true);
    serve();
    return last_error_.empty();
}

void FlightServer::start_async(uint16_t port) {
    FlightServerConfig config;
    config.port = port;
    (void)start_async(config);
}

bool FlightServer::start_async(const FlightServerConfig& config) {
    if (!initialize(config)) {
        ZEPTO_WARN("Flight start rejected: {}", last_error_);
        return false;
    }
    running_.store(true);
    try {
        thread_ = std::thread([this]() { serve(); });
    } catch (const std::exception& error) {
        running_.store(false);
        last_error_ = "Failed to start Flight server thread: " +
            std::string{error.what()};
        return false;
    }
    return true;
}

void FlightServer::stop() {
    if (running_.load()) {
        const auto deadline =
            std::chrono::system_clock::now() + std::chrono::seconds(2);
        const auto status = impl_->server->Shutdown(&deadline);
        if (!status.ok()) {
            ZEPTO_WARN("Flight shutdown failed: {}", status.ToString());
        }
        running_.store(false);
    }
    if (thread_.joinable()) thread_.join();
}

}  // namespace zeptodb::server

#else  // !ZEPTO_FLIGHT_ENABLED — stub

namespace zeptodb::server {

struct FlightServer::Impl {};

FlightServer::FlightServer(
    zeptodb::sql::QueryExecutor& executor,
    std::shared_ptr<zeptodb::auth::AuthManager> auth,
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager)
    : impl_(std::make_unique<Impl>()),
      executor_(executor),
      auth_(std::move(auth)),
      tenant_manager_(std::move(tenant_manager)) {}
FlightServer::~FlightServer() = default;
bool FlightServer::initialize(const FlightServerConfig&) {
    last_error_ = "Arrow Flight support is not available in this build";
    return false;
}
void FlightServer::serve() {}
void FlightServer::start(uint16_t) {
    last_error_ = "Arrow Flight support is not available in this build";
}
bool FlightServer::start(const FlightServerConfig& config) {
    return initialize(config);
}
void FlightServer::start_async(uint16_t) {
    last_error_ = "Arrow Flight support is not available in this build";
}
bool FlightServer::start_async(const FlightServerConfig& config) {
    return initialize(config);
}
void FlightServer::stop() {}

}  // namespace zeptodb::server

#endif  // ZEPTO_FLIGHT_ENABLED
