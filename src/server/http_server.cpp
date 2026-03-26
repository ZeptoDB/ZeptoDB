// ============================================================================
// ZeptoDB: HTTP API Server Implementation
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
#include "zeptodb/server/http_server.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/auth/cancellation_token.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/sql/parser.h"
#include "zeptodb/util/logger.h"

#include <sstream>
#include <string>
#include <cstdio>
#include <future>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <random>

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

// ============================================================================
// Constructors
// ============================================================================
HttpServer::HttpServer(zeptodb::sql::QueryExecutor& executor, uint16_t port)
    : executor_(executor)
    , port_(port)
    , tls_{}
    , auth_(nullptr)
    , svr_(std::make_unique<httplib::Server>())
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
        fprintf(stderr, "[ZeptoDB] WARNING: TLS requested but not compiled in "
                        "(rebuild with APEX_TLS_ENABLED). Falling back to HTTP.\n");
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

        // Table-level ACL enforcement
        if (req.has_header("X-Zepto-Allowed-Tables")) {
            std::string allowed = req.get_header_value("X-Zepto-Allowed-Tables");
            std::vector<std::string> allowed_tables;
            std::istringstream ss(allowed);
            std::string t;
            while (std::getline(ss, t, ','))
                if (!t.empty()) allowed_tables.push_back(t);

            if (!allowed_tables.empty()) {
                try {
                    zeptodb::sql::Parser parser;
                    auto ps = parser.parse_statement(req.body);
                    std::string table;
                    if (ps.select) table = ps.select->from_table;
                    else if (ps.insert) table = ps.insert->table_name;
                    else if (ps.update) table = ps.update->table_name;
                    else if (ps.del) table = ps.del->table_name;
                    if (!table.empty()) {
                        bool ok = false;
                        for (const auto& a : allowed_tables)
                            if (a == table) { ok = true; break; }
                        if (!ok) {
                            res.status = 403;
                            res.set_content(build_error_json(
                                "Access denied: table '" + table + "' not in allowed list"),
                                "application/json");
                            return;
                        }
                    }
                } catch (...) {} // parse failure → let executor handle it
            }
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

    if (query_timeout_ms_ > 0) {
        // Run the query on a separate thread; cancel after timeout
        auto future = std::async(std::launch::async, [this, &sql, &token]() {
            return executor_.execute(sql, token.get());
        });

        auto status = future.wait_for(std::chrono::milliseconds(query_timeout_ms_));
        if (status == std::future_status::timeout) {
            token->cancel();
            future.wait();
            result.error = "Query timed out";
        } else {
            result = future.get();
        }
    } else {
        result = executor_.execute(sql, token.get());
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
        zeptodb::auth::Role role = zeptodb::auth::role_from_string(role_str);
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
               << "\"role\":\"" << zeptodb::auth::role_to_string(k.role) << "\","
               << "\"enabled\":" << (k.enabled ? "true" : "false") << ","
               << "\"created_at_ns\":" << k.created_at_ns << ","
               << "\"allowed_tables\":[";
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
        os << "{\"mode\":\"standalone\""
           << ",\"node_count\":1"
           << ",\"partitions_created\":" << stats.partitions_created.load()
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
    svr_->Post("/admin/nodes", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
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
    // DELETE /admin/nodes/:id — remove a node from the cluster
    // -------------------------------------------------------------------------
    svr_->Delete(R"(/admin/nodes/(\d+))", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
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
    svr_->Get("/admin/tenants", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
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
    svr_->Post("/admin/tenants", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
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
    svr_->Delete(R"(/admin/tenants/([^/]+))", [this, require_admin](
        const httplib::Request& req, httplib::Response& res)
    {
        if (!require_admin(req, res)) return;
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

    // ── String-result path (SHOW TABLES / DESCRIBE) ──
    // When string_rows is populated and rows exist, interleave string values
    // with integer values based on column_types (SYMBOL → string, else int).
    if (!result.string_rows.empty() && !result.rows.empty()) {
        size_t str_idx = 0;
        for (size_t r = 0; r < result.rows.size(); ++r) {
            if (r > 0) os << ",";
            os << "[";
            size_t int_idx = 0;
            for (size_t c = 0; c < result.column_names.size(); ++c) {
                if (c > 0) os << ",";
                if (c < result.column_types.size() &&
                    result.column_types[c] == storage::ColumnType::SYMBOL &&
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

    os << "# HELP zepto_ticks_stored_total Total number of ticks stored\n";
    os << "# TYPE zepto_ticks_stored_total counter\n";
    os << "zepto_ticks_stored_total " << stats.ticks_stored.load() << "\n\n";

    os << "# HELP zepto_ticks_dropped_total Total number of ticks dropped\n";
    os << "# TYPE zepto_ticks_dropped_total counter\n";
    os << "zepto_ticks_dropped_total " << stats.ticks_dropped.load() << "\n\n";

    os << "# HELP zepto_queries_executed_total Total number of queries executed\n";
    os << "# TYPE zepto_queries_executed_total counter\n";
    os << "zepto_queries_executed_total " << stats.queries_executed.load() << "\n\n";

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
