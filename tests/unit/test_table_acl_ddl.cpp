// ============================================================================
// ZeptoDB: HTTP Table ACL — DDL + DESCRIBE coverage + Tenant namespace
// ============================================================================
// Exercises the POST / handler in HttpServer:
//   1. X-Zepto-Allowed-Tables header gates CREATE/DROP/ALTER TABLE + DESCRIBE
//      (previously only SELECT/INSERT/UPDATE/DELETE were covered).
//   2. TenantManager::can_access_table is enforced when X-Zepto-Tenant-Id
//      header is present on the request.
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/auth/license_validator.h"
#include "test_port_helper.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"

using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::sql::QueryExecutor;
using namespace std::chrono_literals;

namespace {

constexpr const char* kSqlRbacJwtSecret = "sql-rbac-jwt-secret";

std::string base64url_encode(const unsigned char* data, size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((size + 2) / 3 * 4);
    for (size_t index = 0; index < size; index += 3) {
        uint32_t value = static_cast<uint32_t>(data[index]) << 16;
        if (index + 1 < size) {
            value |= static_cast<uint32_t>(data[index + 1]) << 8;
        }
        if (index + 2 < size) {
            value |= static_cast<uint32_t>(data[index + 2]);
        }
        output += alphabet[(value >> 18) & 63];
        output += alphabet[(value >> 12) & 63];
        output += index + 1 < size ? alphabet[(value >> 6) & 63] : '=';
        output += index + 2 < size ? alphabet[value & 63] : '=';
    }
    for (char& value : output) {
        if (value == '+') value = '-';
        else if (value == '/') value = '_';
    }
    while (!output.empty() && output.back() == '=') output.pop_back();
    return output;
}

std::string base64url_encode(const std::string& value) {
    return base64url_encode(
        reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::string make_hs256_jwt(const std::string& payload,
                           const std::string& secret) {
    const std::string header =
        base64url_encode(R"({"alg":"HS256","typ":"JWT"})");
    const std::string header_payload =
        header + "." + base64url_encode(payload);
    unsigned int hmac_size = 0;
    unsigned char hmac[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(header_payload.data()),
         header_payload.size(), hmac, &hmac_size);
    return header_payload + "." + base64url_encode(hmac, hmac_size);
}

std::string make_unsigned_test_license(uint32_t features) {
    const std::string payload =
        R"({"edition":"enterprise","features":)" +
        std::to_string(features) + R"(,"max_nodes":1})";
    return base64url_encode(R"({"alg":"none"})") + "." +
           base64url_encode(payload) + ".test";
}

// Low-level raw POST helper: returns {status_code, body} and supports
// authenticated test headers.
struct HttpResp {
    int status = 0;
    std::string headers;
    std::string body;
};

HttpResp http_post_path(uint16_t port,
                        const std::string& path,
                        const std::string& body,
                        const std::string& extra_headers = "") {
    HttpResp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd); return r;
    }

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n";
    req += extra_headers;
    req += "\r\n";
    req += body;
    ::send(fd, req.data(), req.size(), 0);

    std::string raw; char buf[4096];
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    // Parse "HTTP/1.1 <code> ..."
    if (raw.size() > 12) r.status = std::atoi(raw.c_str() + 9);
    auto pos = raw.find("\r\n\r\n");
    r.headers = pos != std::string::npos ? raw.substr(0, pos) : raw;
    r.body = (pos != std::string::npos) ? raw.substr(pos + 4) : "";
    return r;
}

HttpResp http_post(uint16_t port,
                   const std::string& body,
                   const std::string& extra_headers = "") {
    return http_post_path(port, "/", body, extra_headers);
}

HttpResp http_get(uint16_t port,
                  const std::string& path,
                  const std::string& extra_headers = "") {
    HttpResp response;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return response;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) < 0) {
        ::close(fd);
        return response;
    }

    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n";
    request += extra_headers;
    request += "\r\n";
    ::send(fd, request.data(), request.size(), 0);

    std::string raw;
    char buffer[4096];
    timeval timeout{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ssize_t received = 0;
    while ((received = ::recv(fd, buffer, sizeof(buffer), 0)) > 0) {
        raw.append(buffer, static_cast<size_t>(received));
    }
    ::close(fd);

    if (raw.size() > 12) response.status = std::atoi(raw.c_str() + 9);
    const auto body_pos = raw.find("\r\n\r\n");
    response.headers = body_pos != std::string::npos
        ? raw.substr(0, body_pos)
        : raw;
    response.body = body_pos != std::string::npos
        ? raw.substr(body_pos + 4)
        : std::string{};
    return response;
}

std::string bearer_headers(const std::string& key,
                           const std::string& extra_headers = "") {
    return "Authorization: Bearer " + key + "\r\n" + extra_headers;
}

std::string cookie_headers(const std::string& session_id,
                           const std::string& extra_headers = "") {
    return "Cookie: zepto_sid=" + session_id + "\r\n" + extra_headers;
}

std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

class TableAclDdlTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = zepto_test_util::pick_free_port();
        key_file_ = zepto_test_util::unique_test_path("table_acl_keys");
        std::ofstream{key_file_}.close();
        zeptodb::auth::AuthManager::Config auth_config;
        auth_config.enabled = true;
        auth_config.api_keys_file = key_file_.string();
        auth_config.rate_limit_enabled = false;
        auth_config.audit_enabled = false;
        auth_config.audit_buffer_enabled = false;
        auth_ = std::make_shared<zeptodb::auth::AuthManager>(auth_config);
        admin_key_ = auth_->create_api_key(
            "table-acl-admin", zeptodb::auth::Role::ADMIN);
        table_key_ = auth_->create_api_key(
            "table-acl-restricted", zeptodb::auth::Role::ADMIN, {}, {"trades"});
        tenant_a_key_ = auth_->create_api_key(
            "tenant-a-admin", zeptodb::auth::Role::ADMIN, {}, {}, "tA");
        tenant_b_key_ = auth_->create_api_key(
            "tenant-b-admin", zeptodb::auth::Role::ADMIN, {}, {}, "tB");
        tenant_c_key_ = auth_->create_api_key(
            "tenant-c-admin", zeptodb::auth::Role::ADMIN, {}, {}, "tC");

        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
        server_ = std::make_unique<zeptodb::server::HttpServer>(
            *executor_, port_, zeptodb::auth::TlsConfig{}, auth_);
        tenant_ = std::make_shared<zeptodb::auth::TenantManager>();
        server_->set_tenant_manager(tenant_);
        // Pre-create "trades" so DESCRIBE against allowed table succeeds.
        executor_->execute(
            "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp INT64)",
            nullptr);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }
    void TearDown() override {
        server_->stop();
        std::error_code error;
        std::filesystem::remove(key_file_, error);
    }

    uint16_t port_{};
    std::filesystem::path key_file_;
    std::unique_ptr<ZeptoPipeline>                 pipeline_;
    std::unique_ptr<QueryExecutor>                 executor_;
    std::unique_ptr<zeptodb::server::HttpServer>   server_;
    std::shared_ptr<zeptodb::auth::AuthManager>    auth_;
    std::shared_ptr<zeptodb::auth::TenantManager>  tenant_;
    std::string admin_key_;
    std::string table_key_;
    std::string tenant_a_key_;
    std::string tenant_b_key_;
    std::string tenant_c_key_;
};

// --------------------------------------------------------------------------
// E1: allowed_tables covers DDL + DESCRIBE
// --------------------------------------------------------------------------
TEST_F(TableAclDdlTest, TableACL_DenyCreateTable_NotInAllowedList) {
    auto r = http_post(port_,
        "CREATE TABLE forbidden (symbol INT64, price INT64, volume INT64, timestamp INT64)",
        bearer_headers(table_key_));
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(r.body.find("forbidden"), std::string::npos);
}

TEST_F(TableAclDdlTest, TableACL_DenyDropTable_NotInAllowedList) {
    auto r = http_post(port_, "DROP TABLE forbidden",
                       bearer_headers(table_key_));
    EXPECT_EQ(r.status, 403);
}

TEST_F(TableAclDdlTest, TableACL_DenyAlterTable_NotInAllowedList) {
    auto r = http_post(port_, "ALTER TABLE forbidden ADD COLUMN flag INT64",
                       bearer_headers(table_key_));
    EXPECT_EQ(r.status, 403);
}

TEST_F(TableAclDdlTest, TableACL_AllowDescribeAllowedTable) {
    auto r = http_post(port_, "DESCRIBE trades",
                       bearer_headers(table_key_));
    EXPECT_EQ(r.status, 200);
}

TEST_F(TableAclDdlTest, TableACL_DenyDescribeForbiddenTable) {
    auto r = http_post(port_, "DESCRIBE secret",
                       bearer_headers(table_key_));
    EXPECT_EQ(r.status, 403);
}

// --------------------------------------------------------------------------
// E2: Tenant namespace enforcement
// --------------------------------------------------------------------------
TEST_F(TableAclDdlTest, TenantNamespace_DenyTableOutsideNamespace) {
    zeptodb::auth::TenantConfig c;
    c.tenant_id = "tA"; c.table_namespace = "deskA.";
    ASSERT_TRUE(tenant_->create_tenant(c));
    auto r = http_post(port_, "SELECT * FROM deskB.trades",
                       bearer_headers(tenant_a_key_));
    EXPECT_EQ(r.status, 403);
}

TEST_F(TableAclDdlTest, TenantNamespace_AllowTableInsideNamespace) {
    // Use an unquoted-identifier-safe namespace so the table can be named
    // without quoting; this lets us assert a strict 200 (devlog 091 F3).
    zeptodb::auth::TenantConfig c;
    c.tenant_id = "tB"; c.table_namespace = "deska_";
    ASSERT_TRUE(tenant_->create_tenant(c));
    auto create_res = http_post(port_,
        "CREATE TABLE deska_trades (symbol INT64, price INT64, volume INT64, timestamp INT64)",
        bearer_headers(tenant_b_key_));
    ASSERT_EQ(create_res.status, 200);
    auto r = http_post(port_, "SELECT * FROM deska_trades",
                       bearer_headers(tenant_b_key_));
    EXPECT_EQ(r.status, 200);
}

TEST_F(TableAclDdlTest, TenantNamespace_NoNamespace_Unrestricted) {
    zeptodb::auth::TenantConfig c;
    c.tenant_id = "tC"; // empty table_namespace
    ASSERT_TRUE(tenant_->create_tenant(c));
    auto r = http_post(port_, "SELECT * FROM trades",
                       bearer_headers(tenant_c_key_));
    EXPECT_EQ(r.status, 200);
}

TEST_F(TableAclDdlTest, TenantNamespace_NoTenant_Unrestricted) {
    // The unrestricted admin key has no tenant id, so enforcement is skipped.
    auto r = http_post(port_, "SELECT * FROM trades",
                       bearer_headers(admin_key_));
    EXPECT_EQ(r.status, 200);
}

// --------------------------------------------------------------------------
// Authenticated HTTP requests execute the request-owned authorized AST and do
// not prime the process-wide prepared-statement cache.
// --------------------------------------------------------------------------
TEST_F(TableAclDdlTest, HttpPostAuthorizedAstDoesNotPrimeSharedPreparedCache) {
    executor_->clear_prepared_cache();
    const size_t before = executor_->prepared_cache_size();
    (void)http_post(port_, "SELECT * FROM trades", bearer_headers(admin_key_));
    const size_t after = executor_->prepared_cache_size();
    EXPECT_EQ(after, before);
}

// --------------------------------------------------------------------------
// SQL statement RBAC at both POST / and GET /?query= boundaries
// --------------------------------------------------------------------------
class SqlRbacHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = zepto_test_util::pick_free_port();
        key_file_ = zepto_test_util::unique_test_path("sql_rbac_keys");
        std::ofstream{key_file_}.close();

        zeptodb::auth::AuthManager::Config auth_config;
        auth_config.enabled = true;
        auth_config.api_keys_file = key_file_.string();
        auth_config.jwt_enabled = true;
        auth_config.jwt.hs256_secret = kSqlRbacJwtSecret;
        auth_config.rate_limit_enabled = false;
        auth_config.audit_enabled = true;
        auth_config.audit_buffer_enabled = true;
        auth_config.sessions_enabled = true;
        auth_ = std::make_shared<zeptodb::auth::AuthManager>(auth_config);

        admin_key_ = auth_->create_api_key(
            "sql-admin", zeptodb::auth::Role::ADMIN);
        writer_key_ = auth_->create_api_key(
            "sql-writer", zeptodb::auth::Role::WRITER);
        reader_key_ = auth_->create_api_key(
            "sql-reader", zeptodb::auth::Role::READER);
        metrics_key_ = auth_->create_api_key(
            "sql-metrics", zeptodb::auth::Role::METRICS);
        table_reader_key_ = auth_->create_api_key(
            "table-reader", zeptodb::auth::Role::READER, {}, {"trades"});
        tenant_reader_key_ = auth_->create_api_key(
            "tenant-reader", zeptodb::auth::Role::READER, {}, {}, "tenant_a");
        symbol_reader_key_ = auth_->create_api_key(
            "symbol-reader", zeptodb::auth::Role::READER, {"AAPL"});
        jwt_table_reader_ = make_hs256_jwt(
            R"({"sub":"jwt-table-reader","zepto_role":"reader",)"
            R"("allowed_tables":["trades"],"exp":4102444800})",
            kSqlRbacJwtSecret);
        jwt_tenant_reader_ = make_hs256_jwt(
            R"({"sub":"jwt-tenant-reader","zepto_role":"reader",)"
            R"("tenant_id":"tenant_a","exp":4102444800})",
            kSqlRbacJwtSecret);
        auto* session_store = auth_->session_store();
        ASSERT_NE(session_store, nullptr);
        table_reader_session_ = session_store->create(
            "table-session-reader", "table-session-reader",
            zeptodb::auth::Role::READER, "session", {}, "", "", {"trades"});
        tenant_reader_session_ = session_store->create(
            "tenant-session-reader", "tenant-session-reader",
            zeptodb::auth::Role::READER, "session", {}, "tenant_a");
        writer_session_ = session_store->create(
            "writer-session", "writer-session",
            zeptodb::auth::Role::WRITER, "session");

        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
        ASSERT_TRUE(executor_->execute(
            "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, "
            "timestamp TIMESTAMP_NS)").ok());
        ASSERT_TRUE(executor_->execute(
            "CREATE TABLE secret (symbol INT64, price INT64, volume INT64, "
            "timestamp TIMESTAMP_NS)").ok());
        ASSERT_TRUE(executor_->execute(
            "CREATE TABLE tenant_a_trades (symbol INT64, price INT64, "
            "volume INT64, timestamp TIMESTAMP_NS)").ok());
        ASSERT_TRUE(executor_->execute(
            "CREATE TABLE tenant_b_trades (symbol INT64, price INT64, "
            "volume INT64, timestamp TIMESTAMP_NS)").ok());
        ASSERT_TRUE(executor_->execute(
            "INSERT INTO trades VALUES (1, 100, 10, 1000)").ok());

        tenant_ = std::make_shared<zeptodb::auth::TenantManager>();
        zeptodb::auth::TenantConfig tenant_config;
        tenant_config.tenant_id = "tenant_a";
        tenant_config.table_namespace = "tenant_a_";
        ASSERT_TRUE(tenant_->create_tenant(tenant_config));

        server_ = std::make_unique<zeptodb::server::HttpServer>(
            *executor_, port_, zeptodb::auth::TlsConfig{}, auth_);
        server_->set_tenant_manager(tenant_);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }

    void TearDown() override {
        server_->stop();
        std::error_code error;
        std::filesystem::remove(key_file_, error);
    }

    uint16_t port_{};
    std::filesystem::path key_file_;
    std::unique_ptr<ZeptoPipeline> pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
    std::shared_ptr<zeptodb::auth::AuthManager> auth_;
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
    std::string admin_key_;
    std::string writer_key_;
    std::string reader_key_;
    std::string metrics_key_;
    std::string table_reader_key_;
    std::string tenant_reader_key_;
    std::string symbol_reader_key_;
    std::string jwt_table_reader_;
    std::string jwt_tenant_reader_;
    std::string table_reader_session_;
    std::string tenant_reader_session_;
    std::string writer_session_;
};

TEST_F(SqlRbacHttpTest, ReaderAllowsReadOnlySqlOnPostAndGet) {
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades",
                        bearer_headers(reader_key_)).status, 200);
    EXPECT_EQ(http_post(port_, "DESCRIBE trades",
                        bearer_headers(reader_key_)).status, 200);
    EXPECT_EQ(http_post(port_, "SHOW TABLES",
                        bearer_headers(reader_key_)).status, 200);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20trades",
                       bearer_headers(reader_key_)).status, 200);
}

TEST_F(SqlRbacHttpTest, ReaderIsDeniedDmlAndDdl) {
    const std::string headers = bearer_headers(reader_key_);
    EXPECT_EQ(http_post(port_,
        "INSERT INTO trades VALUES (2, 200, 20, 2000)", headers).status, 403);
    EXPECT_EQ(http_post(port_,
        "UPDATE trades SET volume = 20 WHERE symbol = 1", headers).status, 403);
    EXPECT_EQ(http_post(port_,
        "DELETE FROM trades WHERE symbol = 1", headers).status, 403);
    EXPECT_EQ(http_post(port_,
        "CREATE TABLE denied_create (symbol INT64)", headers).status, 403);
    EXPECT_EQ(http_post(port_, "DROP TABLE trades", headers).status, 403);
    EXPECT_EQ(http_post(port_,
        "ALTER TABLE trades ADD COLUMN denied INT64", headers).status, 403);
}

TEST_F(SqlRbacHttpTest, WriterAllowsDmlButIsDeniedDdl) {
    const std::string headers = bearer_headers(writer_key_);
    EXPECT_EQ(http_post(port_,
        "INSERT INTO trades VALUES (2, 200, 20, 2000)", headers).status, 200);
    EXPECT_EQ(http_post(port_,
        "UPDATE trades SET volume = 30 WHERE symbol = 2", headers).status, 200);
    EXPECT_EQ(http_post(port_,
        "DELETE FROM trades WHERE symbol = 2", headers).status, 200);
    EXPECT_EQ(http_post(port_,
        "CREATE TABLE denied_placement (symbol INT64) "
        "WITH (placement = pinned_node, node_id = 8)", headers).status, 403);
    EXPECT_EQ(http_get(
        port_, "/?query=DROP%20TABLE%20trades", headers).status, 403);
}

TEST_F(SqlRbacHttpTest, GetQueryIsReadOnlyForPrivilegedAndSessionAuth) {
    const auto writer_get = http_get(
        port_,
        "/?query=INSERT%20INTO%20trades%20VALUES%20%282%2C200%2C20%2C2000%29",
        bearer_headers(writer_key_));
    EXPECT_EQ(writer_get.status, 405);
    EXPECT_NE(writer_get.body.find("GET SQL is read-only"), std::string::npos);

    const auto session_get = http_get(
        port_,
        "/?query=DELETE%20FROM%20trades%20WHERE%20symbol%20%3D%201",
        cookie_headers(writer_session_));
    EXPECT_EQ(session_get.status, 405);

    const auto admin_get = http_get(
        port_, "/?query=DROP%20TABLE%20trades",
        bearer_headers(admin_key_));
    EXPECT_EQ(admin_get.status, 405);

    const auto rows = executor_->execute("SELECT * FROM trades");
    ASSERT_TRUE(rows.ok()) << rows.error;
    EXPECT_EQ(rows.rows.size(), 1u);
}

TEST_F(SqlRbacHttpTest, AdminAllowsDdlIncludingPlacement) {
    EXPECT_EQ(http_post(port_,
        "CREATE TABLE admin_placement (symbol INT64) "
        "WITH (placement = hash_by_table)",
        bearer_headers(admin_key_)).status, 200);
}

TEST_F(SqlRbacHttpTest, MetricsRoleCannotExecuteSql) {
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades",
                        bearer_headers(metrics_key_)).status, 403);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20trades",
                       bearer_headers(metrics_key_)).status, 403);
}

TEST_F(SqlRbacHttpTest, NonSqlDataSurfacesEnforceCapabilities) {
    const auto reader = bearer_headers(reader_key_);
    const auto writer = bearer_headers(writer_key_);
    const auto metrics = bearer_headers(metrics_key_);

    EXPECT_EQ(http_get(port_, "/metrics", metrics).status, 200);
    EXPECT_EQ(http_get(port_, "/stats", metrics).status, 200);
    EXPECT_EQ(http_get(port_, "/api/ai/stats", metrics).status, 200);
    EXPECT_EQ(http_get(port_, "/metrics", reader).status, 403);
    EXPECT_EQ(http_get(port_, "/stats", reader).status, 403);
    EXPECT_EQ(http_get(port_, "/api/ai/stats", reader).status, 403);

    EXPECT_EQ(http_get(port_, "/api/ai/memories/missing", metrics).status,
              403);
    EXPECT_EQ(http_get(port_, "/api/ai/memories/missing", reader).status,
              404);
    EXPECT_EQ(http_post_path(port_, "/api/ai/memories", "", reader).status,
              403);
    EXPECT_EQ(http_post_path(port_, "/api/ai/memories", "", writer).status,
              400);

    EXPECT_EQ(http_post_path(
        port_, "/insert/msgpack?table=trades", "", reader).status, 403);
    EXPECT_EQ(http_post_path(
        port_, "/insert/msgpack?table=trades", "", writer).status, 400);
    EXPECT_EQ(http_post_path(
        port_, "/insert/arrow?table=trades", "", reader).status, 403);
}

TEST_F(SqlRbacHttpTest, SymbolScopedDataAccessFailsClosed) {
    const auto headers = bearer_headers(symbol_reader_key_);
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades", headers).status, 403);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20trades", headers).status,
              403);
    EXPECT_EQ(http_post_path(
        port_, "/insert/msgpack?table=trades", "", headers).status, 403);
    EXPECT_EQ(http_post_path(
        port_, "/insert/arrow?table=trades", "", headers).status, 403);
    EXPECT_EQ(http_get(port_, "/api/ai/memories/missing", headers).status,
              403);
    EXPECT_EQ(http_post_path(
        port_, "/api/ai/memories", "", headers).status, 403);
    EXPECT_EQ(http_post_path(
        port_, "/api/ai/memories/search", "", headers).status, 403);
    EXPECT_EQ(http_post_path(
        port_, "/api/ai/cache/lookup", "", headers).status, 403);
}

TEST_F(SqlRbacHttpTest, UnknownOrUnconfiguredTenantScopeFailsClosed) {
    const auto unknown_tenant_key = auth_->create_api_key(
        "unknown-tenant", zeptodb::auth::Role::READER, {}, {}, "missing");
    const auto headers = bearer_headers(unknown_tenant_key);
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades", headers).status, 403);

    server_->set_tenant_manager(nullptr);
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades",
                        bearer_headers(tenant_reader_key_)).status, 403);
    server_->set_tenant_manager(tenant_);
}

TEST_F(SqlRbacHttpTest, TenantQueryUsageIsAcquiredAndReleased) {
    const auto before = tenant_->usage("tenant_a");
    ASSERT_TRUE(before.has_value());
    const uint64_t total_before = before->total_queries;

    EXPECT_EQ(http_post(
        port_, "SELECT * FROM tenant_a_trades",
        bearer_headers(tenant_reader_key_)).status, 200);

    const auto after = tenant_->usage("tenant_a");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->active_queries, 0u);
    EXPECT_EQ(after->total_queries, total_before + 1);
}

TEST_F(SqlRbacHttpTest, RequestAndResultLimitsFailClosed) {
    server_->set_max_request_body_bytes(16);
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades WHERE symbol = 12345",
                        bearer_headers(reader_key_)).status, 413);

    server_->set_max_request_body_bytes(64U * 1024U * 1024U);
    server_->set_result_limits(0, 64U * 1024U * 1024U);
    const auto response = http_post(
        port_, "SELECT * FROM trades", bearer_headers(reader_key_));
    EXPECT_EQ(response.status, 400);
    EXPECT_NE(response.body.find("row limit"), std::string::npos);
}

TEST_F(SqlRbacHttpTest, MalformedAndUnclassifiedSqlFailClosed) {
    const std::string headers = bearer_headers(reader_key_);
    EXPECT_EQ(http_post(port_, "VACUUM trades", headers).status, 403);
    EXPECT_EQ(http_post(port_, "SELECT FROM", headers).status, 403);
    EXPECT_EQ(http_post(port_, "", headers).status, 403);
}

TEST_F(SqlRbacHttpTest, SqlPermissionDenialIsAudited) {
    ASSERT_EQ(http_post(port_, "CREATE TABLE audit_denied (symbol INT64)",
                        bearer_headers(reader_key_)).status, 403);
    const auto events = auth_->audit_buffer().last();
    const auto denied = std::find_if(
        events.begin(), events.end(), [](const zeptodb::auth::AuditEvent& event) {
            return event.action == "POST /" &&
                   event.detail == "sql-ADMIN-forbidden";
        });
    EXPECT_NE(denied, events.end());
}

TEST_F(SqlRbacHttpTest, ClientCannotSpoofInternalAuthorizationHeaders) {
    const std::string spoofed_role = bearer_headers(
        reader_key_, "X-Zepto-Role: admin\r\nX-Zepto-Subject: spoofed-admin\r\n");
    EXPECT_EQ(http_post(port_, "CREATE TABLE spoofed (symbol INT64)",
                        spoofed_role).status, 403);
    EXPECT_EQ(http_get(port_, "/admin/version", spoofed_role).status, 403);

    const std::string spoofed_tables = bearer_headers(
        table_reader_key_, "X-Zepto-Allowed-Tables: secret\r\n");
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20secret",
                       spoofed_tables).status, 403);

    const std::string spoofed_tenant = bearer_headers(
        tenant_reader_key_, "X-Zepto-Tenant-Id: tenant_b\r\n");
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_b_trades",
                       spoofed_tenant).status, 403);
}

TEST_F(SqlRbacHttpTest, DuplicateCredentialHeadersFailClosed) {
    const auto duplicate_authorization = http_get(
        port_, "/admin/version",
        bearer_headers(admin_key_,
                       "Authorization: Bearer " + reader_key_ + "\r\n"));
    EXPECT_EQ(duplicate_authorization.status, 400);
    EXPECT_NE(duplicate_authorization.body.find("duplicate credential"),
              std::string::npos);

    const auto duplicate_cookie = http_get(
        port_, "/?query=SELECT%20*%20FROM%20trades",
        cookie_headers(table_reader_session_,
                       "Cookie: zepto_sid=" + tenant_reader_session_ + "\r\n"));
    EXPECT_EQ(duplicate_cookie.status, 400);
}

TEST_F(SqlRbacHttpTest, AdminKeyJsonEscapesPersistedIdentityFields) {
    (void)auth_->create_api_key(
        "quoted\"name", zeptodb::auth::Role::READER,
        {"AAPL\"quoted"}, {"table\"name"}, "tenant\"name");

    const auto response = http_get(
        port_, "/admin/keys", bearer_headers(admin_key_));
    ASSERT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("quoted\\\"name"), std::string::npos);
    EXPECT_NE(response.body.find("AAPL\\\"quoted"), std::string::npos);
    EXPECT_NE(response.body.find("table\\\"name"), std::string::npos);
    EXPECT_NE(response.body.find("tenant\\\"name"), std::string::npos);
}

TEST_F(SqlRbacHttpTest, AdminVersionMatchesReleaseMetadata) {
    const auto response = http_get(
        port_, "/admin/version", bearer_headers(admin_key_));
    ASSERT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("\"version\":\"" ZEPTO_VERSION "\""),
              std::string::npos);
}

TEST_F(SqlRbacHttpTest, GetQueryEnforcesTableAndTenantAcl) {
    const std::string table_headers = bearer_headers(table_reader_key_);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20trades",
                       table_headers).status, 200);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20secret",
                       table_headers).status, 403);

    const std::string tenant_headers = bearer_headers(tenant_reader_key_);
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_a_trades",
                       tenant_headers).status, 200);
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_b_trades",
                       tenant_headers).status, 403);
}

TEST_F(SqlRbacHttpTest, JwtClaimsEnforceTableAndTenantAcl) {
    const std::string table_headers = bearer_headers(jwt_table_reader_);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20trades",
                       table_headers).status, 200);
    EXPECT_EQ(http_post(port_, "SELECT * FROM secret", table_headers).status,
              403);
    const auto table_show = http_get(
        port_, "/?query=SHOW%20TABLES", table_headers);
    ASSERT_EQ(table_show.status, 200);
    EXPECT_NE(table_show.body.find("trades"), std::string::npos);
    EXPECT_EQ(table_show.body.find("secret"), std::string::npos);

    const std::string tenant_headers = bearer_headers(jwt_tenant_reader_);
    EXPECT_EQ(http_post(port_, "SELECT * FROM tenant_a_trades",
                        tenant_headers).status, 200);
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_b_trades",
                       tenant_headers).status, 403);
}

TEST_F(SqlRbacHttpTest, NestedSelectSourcesCannotBypassTableAcl) {
    const std::string headers = bearer_headers(table_reader_key_);
    EXPECT_EQ(http_post(
        port_,
        "SELECT t.symbol FROM trades t JOIN secret s "
        "ON t.symbol = s.symbol",
        headers).status, 403);
    EXPECT_EQ(http_get(
        port_,
        "/?query=WITH%20hidden%20AS%20%28SELECT%20*%20FROM%20secret%29"
        "%20SELECT%20*%20FROM%20hidden",
        headers).status, 403);
    EXPECT_EQ(http_post(
        port_,
        "SELECT * FROM trades UNION ALL SELECT * FROM secret",
        headers).status, 403);
    EXPECT_EQ(http_post(
        port_,
        "SELECT * FROM trades WHERE symbol IN "
        "(SELECT symbol FROM secret)",
        headers).status, 403);
    const std::string shadowing_cte =
        "WITH a AS (SELECT * FROM secret), "
        "secret AS (SELECT * FROM trades) SELECT * FROM a";
    EXPECT_EQ(http_post(port_, shadowing_cte, headers).status, 403);
    EXPECT_EQ(http_get(
        port_,
        "/?query=WITH%20a%20AS%20%28SELECT%20*%20FROM%20secret%29%2C"
        "%20secret%20AS%20%28SELECT%20*%20FROM%20trades%29"
        "%20SELECT%20*%20FROM%20a",
        headers).status, 403);
}

TEST_F(SqlRbacHttpTest, SessionCookiePreservesRoleTableAndTenantRestrictions) {
    const std::string table_session = cookie_headers(table_reader_session_);
    EXPECT_EQ(http_post(port_, "SELECT * FROM trades", table_session).status,
              200);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20trades",
                       table_session).status, 200);
    EXPECT_EQ(http_post(port_,
        "INSERT INTO trades VALUES (2, 200, 20, 2000)",
        table_session).status, 403);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20secret",
                       table_session).status, 403);
    const auto table_show = http_post(port_, "SHOW TABLES", table_session);
    ASSERT_EQ(table_show.status, 200);
    EXPECT_NE(table_show.body.find("[\"trades\","), std::string::npos);
    EXPECT_EQ(table_show.body.find("secret"), std::string::npos);
    EXPECT_EQ(table_show.body.find("tenant_a_trades"), std::string::npos);

    const std::string tenant_session = cookie_headers(tenant_reader_session_);
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_a_trades",
                       tenant_session).status, 200);
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_b_trades",
                       tenant_session).status, 403);
    const auto tenant_show = http_get(
        port_, "/?query=SHOW%20TABLES", tenant_session);
    ASSERT_EQ(tenant_show.status, 200);
    EXPECT_NE(tenant_show.body.find("tenant_a_trades"), std::string::npos);
    EXPECT_EQ(tenant_show.body.find("tenant_b_trades"), std::string::npos);
    EXPECT_EQ(tenant_show.body.find("[\"trades\","), std::string::npos);
}

TEST_F(SqlRbacHttpTest, SessionCookieCannotSpoofInternalAuthorizationHeaders) {
    const std::string spoofed_role = cookie_headers(
        table_reader_session_,
        "X-Zepto-Role: admin\r\n"
        "X-Zepto-Allowed-Tables: secret\r\n"
        "X-Zepto-Subject: spoofed-session-admin\r\n");
    EXPECT_EQ(http_post(port_, "CREATE TABLE session_spoof (symbol INT64)",
                        spoofed_role).status, 403);
    EXPECT_EQ(http_get(port_, "/?query=SELECT%20*%20FROM%20secret",
                       spoofed_role).status, 403);

    const std::string spoofed_tenant = cookie_headers(
        tenant_reader_session_, "X-Zepto-Tenant-Id: tenant_b\r\n");
    EXPECT_EQ(http_get(port_,
                       "/?query=SELECT%20*%20FROM%20tenant_b_trades",
                       spoofed_tenant).status, 403);
}

TEST(SqlRbacNoAuthTest, DisabledAuthPreservesExecutorErrorBehavior) {
    auto pipeline = make_pipeline();
    auto executor = std::make_unique<QueryExecutor>(*pipeline);
    zeptodb::auth::AuthManager::Config auth_config;
    auth_config.enabled = false;
    auth_config.rate_limit_enabled = false;
    auth_config.audit_enabled = false;
    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_config);
    const uint16_t port = zepto_test_util::pick_free_port();
    zeptodb::server::HttpServer server{
        *executor, port, zeptodb::auth::TlsConfig{}, auth};
    server.start_async();
    std::this_thread::sleep_for(60ms);

    EXPECT_EQ(http_post(port, "VACUUM trades").status, 400);
    EXPECT_EQ(http_post(port, "CREATE TABLE no_auth (symbol INT64)").status, 200);
    server.stop();
}

TEST(SqlRbacNoAuthTest, DisabledAuthPreservesCallerTenantScoping) {
    auto pipeline = make_pipeline();
    auto executor = std::make_unique<QueryExecutor>(*pipeline);
    ASSERT_TRUE(executor->execute(
        "CREATE TABLE tenant_a_trades (symbol INT64)").ok());
    ASSERT_TRUE(executor->execute(
        "CREATE TABLE tenant_b_trades (symbol INT64)").ok());

    auto tenants = std::make_shared<zeptodb::auth::TenantManager>();
    zeptodb::auth::TenantConfig tenant;
    tenant.tenant_id = "tenant_a";
    tenant.table_namespace = "tenant_a_";
    ASSERT_TRUE(tenants->create_tenant(tenant));

    zeptodb::auth::AuthManager::Config auth_config;
    auth_config.enabled = false;
    auth_config.rate_limit_enabled = false;
    auth_config.audit_enabled = false;
    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_config);
    const uint16_t port = zepto_test_util::pick_free_port();
    zeptodb::server::HttpServer server{
        *executor, port, zeptodb::auth::TlsConfig{}, auth};
    server.set_tenant_manager(tenants);
    server.start_async();
    std::this_thread::sleep_for(60ms);

    const std::string tenant_header =
        "X-Zepto-Tenant-Id: tenant_a\r\n";
    EXPECT_EQ(http_post(port, "SELECT * FROM tenant_a_trades",
                        tenant_header).status, 200);
    EXPECT_EQ(http_post(port, "SELECT * FROM tenant_b_trades",
                        tenant_header).status, 403);
    server.stop();
}

TEST(HttpTlsConfigTest, InvalidTlsConfigurationFailsClosed) {
    auto pipeline = make_pipeline();
    QueryExecutor executor(*pipeline);
    zeptodb::auth::TlsConfig tls;
    tls.enabled = true;
    tls.cert_path = "/definitely/missing/zeptodb-cert.pem";
    tls.key_path = "/definitely/missing/zeptodb-key.pem";
    EXPECT_THROW(
        zeptodb::server::HttpServer(
            executor, zepto_test_util::pick_free_port(), tls, nullptr),
        std::runtime_error);
}

class AuthHttpSinglePassRateLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto sso_feature = static_cast<uint32_t>(
            zeptodb::auth::Feature::SSO);
        ASSERT_TRUE(zeptodb::auth::license().load_from_jwt_string_for_testing(
            make_unsigned_test_license(sso_feature)));

        port_ = zepto_test_util::pick_free_port();
        key_file_ = zepto_test_util::unique_test_path("auth_http_rate_keys");
        std::ofstream{key_file_}.close();

        zeptodb::auth::AuthManager::Config auth_config;
        auth_config.enabled = true;
        auth_config.api_keys_file = key_file_.string();
        auth_config.rate_limit_enabled = true;
        auth_config.rate_limit.requests_per_minute = 1;
        auth_config.rate_limit.burst_capacity = 1;
        auth_config.rate_limit.per_ip_rpm = 0;
        auth_config.audit_enabled = false;
        auth_config.sessions_enabled = true;
        auth_ = std::make_shared<zeptodb::auth::AuthManager>(auth_config);

        whoami_key_ = auth_->create_api_key(
            "whoami-key", zeptodb::auth::Role::READER);
        auth_me_key_ = auth_->create_api_key(
            "auth-me-key", zeptodb::auth::Role::WRITER);
        auth_session_key_ = auth_->create_api_key(
            "auth-session-key", zeptodb::auth::Role::READER);
        auto* sessions = auth_->session_store();
        ASSERT_NE(sessions, nullptr);
        whoami_session_ = sessions->create(
            "whoami-session", "whoami-session",
            zeptodb::auth::Role::READER, "session");
        auth_me_session_ = sessions->create(
            "auth-me-session", "auth-me-session",
            zeptodb::auth::Role::WRITER, "session");

        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
        server_ = std::make_unique<zeptodb::server::HttpServer>(
            *executor_, port_, zeptodb::auth::TlsConfig{}, auth_);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }

    void TearDown() override {
        if (server_) server_->stop();
        zeptodb::auth::license().load_from_jwt_string_for_testing("");
        std::error_code error;
        std::filesystem::remove(key_file_, error);
    }

    uint16_t port_{};
    std::filesystem::path key_file_;
    std::unique_ptr<ZeptoPipeline> pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
    std::shared_ptr<zeptodb::auth::AuthManager> auth_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
    std::string whoami_key_;
    std::string auth_me_key_;
    std::string auth_session_key_;
    std::string whoami_session_;
    std::string auth_me_session_;
};

TEST_F(AuthHttpSinglePassRateLimitTest,
       BearerIdentityEndpointsConsumeOneTokenPerRequest) {
    const std::string whoami_headers = bearer_headers(whoami_key_);
    EXPECT_EQ(http_get(port_, "/whoami", whoami_headers).status, 200);
    EXPECT_EQ(http_get(port_, "/whoami", whoami_headers).status, 403);

    const std::string auth_me_headers = bearer_headers(auth_me_key_);
    EXPECT_EQ(http_get(port_, "/auth/me", auth_me_headers).status, 200);
    EXPECT_EQ(http_get(port_, "/auth/me", auth_me_headers).status, 403);
}

TEST_F(AuthHttpSinglePassRateLimitTest,
       SessionIdentityEndpointsConsumeOneTokenPerRequest) {
    const std::string whoami_headers = cookie_headers(whoami_session_);
    EXPECT_EQ(http_get(port_, "/whoami", whoami_headers).status, 200);
    EXPECT_EQ(http_get(port_, "/whoami", whoami_headers).status, 403);

    const std::string auth_me_headers = cookie_headers(auth_me_session_);
    EXPECT_EQ(http_get(port_, "/auth/me", auth_me_headers).status, 200);
    EXPECT_EQ(http_get(port_, "/auth/me", auth_me_headers).status, 403);
}

TEST_F(AuthHttpSinglePassRateLimitTest,
       AuthSessionValidatesBearerOnce) {
    const std::string headers = bearer_headers(auth_session_key_);
    EXPECT_EQ(http_post_path(port_, "/auth/session", "", headers).status,
              200);
    EXPECT_EQ(http_post_path(port_, "/auth/session", "", headers).status,
              403);
}

TEST_F(AuthHttpSinglePassRateLimitTest, SensitiveAuthResponsesAreNeverCached) {
    auto expect_no_store = [](const HttpResp& response) {
        EXPECT_NE(response.headers.find("Cache-Control: no-store, private"),
                  std::string::npos)
            << response.headers;
        EXPECT_NE(response.headers.find("Pragma: no-cache"),
                  std::string::npos)
            << response.headers;
    };

    expect_no_store(http_get(port_, "/auth/login"));
    expect_no_store(http_get(port_, "/auth/callback"));
    expect_no_store(http_get(
        port_, "/auth/me", bearer_headers(auth_me_key_)));
    expect_no_store(http_post_path(port_, "/auth/logout", ""));
    expect_no_store(http_post_path(port_, "/auth/refresh", ""));
}

} // namespace
