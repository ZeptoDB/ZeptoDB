// ============================================================================
// ZeptoDB: Arrow Flight Server — Implementation
// ============================================================================
#include "zeptodb/server/flight_server.h"
#include "zeptodb/server/arrow_ipc.h"
#include "zeptodb/common/logger.h"

#ifdef ZEPTO_FLIGHT_ENABLED

#include <arrow/api.h>
#include <arrow/flight/api.h>
#include <arrow/flight/server.h>
#include <arrow/builder.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>
#include <arrow/ipc/writer.h>

namespace flight = arrow::flight;

// ============================================================================
// ZeptoFlightServer — FlightServerBase implementation
// ============================================================================
class ZeptoFlightServer : public flight::FlightServerBase {
public:
    explicit ZeptoFlightServer(zeptodb::sql::QueryExecutor& executor)
        : executor_(executor) {}

    // ---- GetFlightInfo: SQL query → schema + row count --------------------
    arrow::Status GetFlightInfo(const flight::ServerCallContext&,
                                const flight::FlightDescriptor& desc,
                                std::unique_ptr<flight::FlightInfo>* info) override {
        if (desc.type != flight::FlightDescriptor::CMD) {
            return arrow::Status::Invalid("Only CMD descriptors supported");
        }
        std::string sql(desc.cmd);
        auto rs = executor_.execute(sql);
        if (!rs.ok())
            return arrow::Status::ExecutionError(rs.error);

        auto schema = zeptodb::server::build_arrow_schema(rs);
        size_t nrows = !rs.typed_rows.empty() ? rs.typed_rows.size() : rs.rows.size();

        flight::FlightEndpoint ep;
        ep.ticket.ticket = sql;

        ARROW_ASSIGN_OR_RAISE(auto fi,
            flight::FlightInfo::Make(*schema, desc, {ep},
                                     static_cast<int64_t>(nrows), -1));
        *info = std::make_unique<flight::FlightInfo>(std::move(fi));
        return arrow::Status::OK();
    }

    // ---- DoGet: execute SQL, stream RecordBatches -------------------------
    arrow::Status DoGet(const flight::ServerCallContext&,
                        const flight::Ticket& ticket,
                        std::unique_ptr<flight::FlightDataStream>* stream) override {
        std::string sql(ticket.ticket);
        auto rs = executor_.execute(sql);
        if (!rs.ok())
            return arrow::Status::ExecutionError(rs.error);

        auto schema = zeptodb::server::build_arrow_schema(rs);
        ARROW_ASSIGN_OR_RAISE(auto batch,
            zeptodb::server::result_to_record_batch(rs, schema));

        auto reader = arrow::RecordBatchReader::Make({batch}, schema);
        if (!reader.ok())
            return reader.status();

        *stream = std::make_unique<flight::RecordBatchStream>(*reader);
        return arrow::Status::OK();
    }

    // ---- DoPut: ingest Arrow RecordBatches --------------------------------
    arrow::Status DoPut(const flight::ServerCallContext&,
                        std::unique_ptr<flight::FlightMessageReader> reader,
                        std::unique_ptr<flight::FlightMetadataWriter>) override {
        auto& desc = reader->descriptor();
        if (desc.type != flight::FlightDescriptor::CMD) {
            return arrow::Status::Invalid("DoPut requires CMD descriptor with table name");
        }
        std::string table_name(desc.cmd);

        // Read all batches and INSERT via SQL
        flight::FlightStreamChunk chunk;
        int64_t total_rows = 0;
        while (true) {
            ARROW_ASSIGN_OR_RAISE(chunk, reader->Next());
            if (!chunk.data) break;

            auto batch = chunk.data;
            auto schema = batch->schema();
            int64_t nrows = batch->num_rows();
            int ncols = batch->num_columns();

            for (int64_t r = 0; r < nrows; ++r) {
                std::string sql = "INSERT INTO " + table_name + " VALUES (";
                for (int c = 0; c < ncols; ++c) {
                    if (c > 0) sql += ", ";
                    auto col = batch->column(c);
                    auto type_id = col->type_id();
                    if (type_id == arrow::Type::INT64) {
                        auto arr = std::static_pointer_cast<arrow::Int64Array>(col);
                        sql += std::to_string(arr->Value(r));
                    } else if (type_id == arrow::Type::DOUBLE) {
                        auto arr = std::static_pointer_cast<arrow::DoubleArray>(col);
                        sql += std::to_string(arr->Value(r));
                    } else if (type_id == arrow::Type::FLOAT) {
                        auto arr = std::static_pointer_cast<arrow::FloatArray>(col);
                        sql += std::to_string(arr->Value(r));
                    } else if (type_id == arrow::Type::STRING) {
                        auto arr = std::static_pointer_cast<arrow::StringArray>(col);
                        sql += "'" + arr->GetString(r) + "'";
                    } else {
                        sql += "0";
                    }
                }
                sql += ")";
                auto result = executor_.execute(sql);
                if (!result.ok())
                    return arrow::Status::ExecutionError(result.error);
            }
            total_rows += nrows;
        }
        ZEPTO_INFO("Flight DoPut: table={}, rows={}", table_name, total_rows);
        return arrow::Status::OK();
    }

    // ---- ListFlights: list tables -----------------------------------------
    arrow::Status ListFlights(const flight::ServerCallContext&,
                              const flight::Criteria*,
                              std::unique_ptr<flight::FlightListing>* listings) override {
        auto rs = executor_.execute("SHOW TABLES");
        std::vector<flight::FlightInfo> infos;

        if (rs.ok() && !rs.string_rows.empty()) {
            for (auto& table : rs.string_rows) {
                auto count_rs = executor_.execute(
                    "SELECT COUNT(*) FROM " + table);
                int64_t nrows = 0;
                if (count_rs.ok() && !count_rs.rows.empty())
                    nrows = count_rs.rows[0][0];

                flight::FlightDescriptor desc =
                    flight::FlightDescriptor::Command(table);
                auto schema = arrow::schema({arrow::field("info", arrow::utf8())});

                auto fi = flight::FlightInfo::Make(*schema, desc, {},
                                                    nrows, -1);
                if (fi.ok()) infos.push_back(std::move(*fi));
            }
        }

        *listings = std::make_unique<flight::SimpleFlightListing>(std::move(infos));
        return arrow::Status::OK();
    }

    // ---- DoAction: ping / healthcheck -------------------------------------
    arrow::Status DoAction(const flight::ServerCallContext&,
                           const flight::Action& action,
                           std::unique_ptr<flight::ResultStream>* result) override {
        if (action.type == "ping" || action.type == "healthcheck") {
            std::string body = "{\"status\":\"ok\"}";
            auto buf = arrow::Buffer::FromString(body);
            std::vector<flight::Result> results;
            results.push_back(flight::Result{buf});
            *result = std::make_unique<flight::SimpleResultStream>(
                std::move(results));
            return arrow::Status::OK();
        }
        return arrow::Status::NotImplemented("Unknown action: ", action.type);
    }

    // ---- ListActions ------------------------------------------------------
    arrow::Status ListActions(const flight::ServerCallContext&,
                              std::vector<flight::ActionType>* actions) override {
        *actions = {
            {"ping", "Health check"},
            {"healthcheck", "Health check (alias)"},
        };
        return arrow::Status::OK();
    }

private:
    zeptodb::sql::QueryExecutor& executor_;
};

// ============================================================================
// FlightServer pimpl
// ============================================================================
namespace zeptodb::server {

struct FlightServer::Impl {
    std::unique_ptr<ZeptoFlightServer> server;
};

FlightServer::FlightServer(zeptodb::sql::QueryExecutor& executor)
    : impl_(std::make_unique<Impl>()), executor_(executor) {
    impl_->server = std::make_unique<ZeptoFlightServer>(executor);
}

FlightServer::~FlightServer() { stop(); }

void FlightServer::start(uint16_t port) {
    auto loc = flight::Location::ForGrpcTcp("0.0.0.0", port);
    if (!loc.ok()) {
        ZEPTO_WARN("Flight: failed to create location: {}", loc.status().ToString());
        return;
    }
    flight::FlightServerOptions opts(*loc);
    auto st = impl_->server->Init(opts);
    if (!st.ok()) {
        ZEPTO_WARN("Flight: Init failed: {}", st.ToString());
        return;
    }
    port_ = impl_->server->port();
    running_.store(true);
    ZEPTO_INFO("Arrow Flight server listening on grpc://0.0.0.0:{}", port_);
    st = impl_->server->Serve();  // blocks
    running_.store(false);
}

void FlightServer::start_async(uint16_t port) {
    thread_ = std::thread([this, port]() { start(port); });
    // Wait until actually listening
    for (int i = 0; i < 200 && !running_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void FlightServer::stop() {
    if (running_.load()) {
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
        impl_->server->Shutdown(&deadline);
        running_.store(false);
    }
    if (thread_.joinable()) thread_.join();
}

} // namespace zeptodb::server

#else // !ZEPTO_FLIGHT_ENABLED — stub

namespace zeptodb::server {

struct FlightServer::Impl {};

FlightServer::FlightServer(zeptodb::sql::QueryExecutor& executor)
    : impl_(std::make_unique<Impl>()), executor_(executor) {}
FlightServer::~FlightServer() = default;
void FlightServer::start(uint16_t) {}
void FlightServer::start_async(uint16_t) {}
void FlightServer::stop() {}

} // namespace zeptodb::server

#endif // ZEPTO_FLIGHT_ENABLED
