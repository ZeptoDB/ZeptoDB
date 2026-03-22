// ============================================================================
// APEX-DB: HTTP API Server Implementation
// ============================================================================
// cpp-httplib based lightweight HTTP/HTTPS server.
// ClickHouse compatible port 8123.
// ============================================================================

// Enable TLS support if OpenSSL is available
#ifdef APEX_TLS_ENABLED
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

// httplib is header-only — include only in .cpp (compile speed)
#include "third_party/httplib.h"
#include "apex/server/http_server.h"
#include "apex/core/pipeline.h"
#include "apex/auth/cancellation_token.h"

#include <sstream>
#include <string>
#include <cstdio>
#include <future>
#include <chrono>

namespace apex::server {

// ============================================================================
// Constructors
// ============================================================================
HttpServer::HttpServer(apex::sql::QueryExecutor& executor, uint16_t port)
    : executor_(executor)
    , port_(port)
    , tls_{}
    , auth_(nullptr)
    , svr_(std::make_unique<httplib::Server>())
{
    setup_routes();
    setup_auth_middleware();
    setup_admin_routes();
}

HttpServer::HttpServer(apex::sql::QueryExecutor& executor,
                       uint16_t port,
                       apex::auth::TlsConfig tls,
                       std::shared_ptr<apex::auth::AuthManager> auth)
    : executor_(executor)
    , port_(port)
    , tls_(std::move(tls))
    , auth_(std::move(auth))
{
#ifdef APEX_TLS_ENABLED
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
        fprintf(stderr, "[APEX-DB] WARNING: TLS requested but not compiled in "
                        "(rebuild with APEX_TLS_ENABLED). Falling back to HTTP.\n");
    }
    svr_ = std::make_unique<httplib::Server>();
#endif
    setup_routes();
    setup_auth_middleware();
    setup_admin_routes();
}

HttpServer::~HttpServer() {
    if (running_.load()) stop();
}

// ============================================================================
// setup_auth_middleware — pre-routing handler for authentication
// ============================================================================
void HttpServer::setup_auth_middleware() {
    if (!auth_) return;

    svr_->set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res)
        -> httplib::Server::HandlerResponse
    {
        std::string auth_header;
        if (req.has_header("Authorization"))
            auth_header = req.get_header_value("Authorization");

        std::string remote_addr = req.remote_addr;

        auto decision = auth_->check(req.method, req.path,
                                     auth_header, remote_addr);

        if (decision.status == apex::auth::AuthStatus::OK) {
            // Continue to route handler
            return httplib::Server::HandlerResponse::Unhandled;
        }

        int status_code = (decision.status == apex::auth::AuthStatus::UNAUTHORIZED)
                          ? 401 : 403;
        res.status = status_code;
        if (status_code == 401)
            res.set_header("WWW-Authenticate", "Bearer realm=\"APEX-DB\"");
        res.set_content(build_error_json(decision.reason), "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
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

    // GET /metrics — Prometheus metrics (OpenMetrics format)
    svr_->Get("/metrics", [this](const httplib::Request& /*req*/,
                                  httplib::Response& res) {
        res.set_content(build_prometheus_metrics(), "text/plain; version=0.0.4");
    });

    // GET /stats — pipeline statistics
    svr_->Get("/stats", [this](const httplib::Request& /*req*/,
                                httplib::Response& res) {
        auto json = build_stats_json(executor_.stats());
        res.set_content(json, "application/json");
    });

    // POST / — execute SQL query (ClickHouse compatible)
    svr_->Post("/", [this](const httplib::Request& req,
                            httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content(build_error_json("Empty query body"), "application/json");
            return;
        }

        auto result = run_query_with_tracking(req.body, req.remote_addr);

        if (!result.ok()) {
            res.status = (result.error == "Query cancelled" ||
                          result.error == "Query timed out") ? 408 : 400;
            res.set_content(build_error_json(result.error), "application/json");
            return;
        }

        res.set_content(build_json_response(result), "application/json");
    });

    // GET / — execute SQL query via query parameter
    svr_->Get("/", [this](const httplib::Request& req,
                           httplib::Response& res) {
        auto q = req.get_param_value("query");
        if (q.empty()) {
            res.set_content(R"({"status":"ok","engine":"APEX-DB"})", "application/json");
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
apex::sql::QueryResultSet HttpServer::run_query_with_tracking(
    const std::string& sql,
    const std::string& subject)
{
    auto token = std::make_shared<apex::auth::CancellationToken>();
    std::string query_id = query_tracker_.register_query(subject, sql, token);

    apex::sql::QueryResultSet result;

    if (query_timeout_ms_ > 0) {
        // Run the query on a separate thread; cancel after timeout
        auto future = std::async(std::launch::async, [this, &sql, &token]() {
            return executor_.execute(sql, token.get());
        });

        auto status = future.wait_for(std::chrono::milliseconds(query_timeout_ms_));
        if (status == std::future_status::timeout) {
            token->cancel();
            // Wait for the query thread to acknowledge cancellation
            future.wait();
            result.error = "Query timed out";
        } else {
            result = future.get();
        }
    } else {
        result = executor_.execute(sql, token.get());
    }

    query_tracker_.complete(query_id);
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
        if (decision.status != apex::auth::AuthStatus::OK) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer realm=\"APEX-DB\"");
            res.set_content(build_error_json(decision.reason), "application/json");
            return false;
        }
        if (!decision.context.has_permission(apex::auth::Permission::ADMIN)) {
            res.status = 403;
            res.set_content(build_error_json("Admin permission required"),
                            "application/json");
            return false;
        }
        return true;
    };

    // -------------------------------------------------------------------------
    // POST /admin/keys — create API key
    // Body: {"name":"<name>","role":"<role>","symbols":["SYM1",...]}
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
        // Minimal JSON parsing for name + role fields
        auto extract = [&](const std::string& field) -> std::string {
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
        std::string name = extract("name");
        std::string role_str = extract("role");
        if (name.empty()) {
            res.status = 400;
            res.set_content(build_error_json("Missing 'name' field"), "application/json");
            return;
        }
        apex::auth::Role role = apex::auth::role_from_string(role_str);
        std::string key = auth_->create_api_key(name, role, {});
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
               << "\"role\":\"" << apex::auth::role_to_string(k.role) << "\","
               << "\"enabled\":" << (k.enabled ? "true" : "false") << ","
               << "\"created_at_ns\":" << k.created_at_ns << "}";
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
    // GET /admin/version — server version info
    // -------------------------------------------------------------------------
    svr_->Get("/admin/version", [require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
        res.set_content(
            R"({"engine":"APEX-DB","version":"0.1.0","build":")" __DATE__ R"("})",
            "application/json");
    });
}

// ============================================================================
// start() — blocking
// ============================================================================
void HttpServer::start() {
    running_.store(true);
    svr_->listen("0.0.0.0", static_cast<int>(port_));
    running_.store(false);
}

// ============================================================================
// start_async() — background thread
// ============================================================================
void HttpServer::start_async() {
    running_.store(true);
    thread_ = std::thread([this]() {
        svr_->listen("0.0.0.0", static_cast<int>(port_));
        running_.store(false);
    });
    // Wait briefly for the server to start listening
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// stop()
// ============================================================================
void HttpServer::stop() {
    svr_->stop();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

// ============================================================================
// JSON response builder
// ============================================================================
std::string HttpServer::build_json_response(
    const apex::sql::QueryResultSet& result)
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
    os << "\"data\":[";
    for (size_t r = 0; r < result.rows.size(); ++r) {
        if (r > 0) os << ",";
        os << "[";
        const auto& row = result.rows[r];
        for (size_t c = 0; c < row.size(); ++c) {
            if (c > 0) os << ",";
            os << row[c];
        }
        os << "]";
    }
    os << "],";

    // 메타데이터
    os << "\"rows\":" << result.rows.size() << ",";
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
    const apex::core::PipelineStats& stats)
{
    std::ostringstream os;
    os << "{"
       << "\"ticks_ingested\":"   << stats.ticks_ingested.load()   << ","
       << "\"ticks_stored\":"     << stats.ticks_stored.load()     << ","
       << "\"ticks_dropped\":"    << stats.ticks_dropped.load()    << ","
       << "\"queries_executed\":" << stats.queries_executed.load() << ","
       << "\"total_rows_scanned\":" << stats.total_rows_scanned.load()
       << "}";
    return os.str();
}

// ============================================================================
// Prometheus metrics (OpenMetrics format)
// ============================================================================
std::string HttpServer::build_prometheus_metrics() const {
    const auto& stats = executor_.stats();
    std::ostringstream os;

    os << "# HELP apex_ticks_ingested_total Total number of ticks ingested\n";
    os << "# TYPE apex_ticks_ingested_total counter\n";
    os << "apex_ticks_ingested_total " << stats.ticks_ingested.load() << "\n\n";

    os << "# HELP apex_ticks_stored_total Total number of ticks stored\n";
    os << "# TYPE apex_ticks_stored_total counter\n";
    os << "apex_ticks_stored_total " << stats.ticks_stored.load() << "\n\n";

    os << "# HELP apex_ticks_dropped_total Total number of ticks dropped\n";
    os << "# TYPE apex_ticks_dropped_total counter\n";
    os << "apex_ticks_dropped_total " << stats.ticks_dropped.load() << "\n\n";

    os << "# HELP apex_queries_executed_total Total number of queries executed\n";
    os << "# TYPE apex_queries_executed_total counter\n";
    os << "apex_queries_executed_total " << stats.queries_executed.load() << "\n\n";

    os << "# HELP apex_rows_scanned_total Total number of rows scanned\n";
    os << "# TYPE apex_rows_scanned_total counter\n";
    os << "apex_rows_scanned_total " << stats.total_rows_scanned.load() << "\n\n";

    os << "# HELP apex_server_up Server is up and running\n";
    os << "# TYPE apex_server_up gauge\n";
    os << "apex_server_up " << (running_.load() ? "1" : "0") << "\n\n";

    os << "# HELP apex_server_ready Server is ready to accept queries\n";
    os << "# TYPE apex_server_ready gauge\n";
    os << "apex_server_ready " << (ready_.load() ? "1" : "0") << "\n";

    return os.str();
}

} // namespace apex::server
