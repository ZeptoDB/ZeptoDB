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

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

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

// Low-level raw POST helper: returns {status_code, body}.
// Allows injecting arbitrary request headers (used to simulate the middleware
// having stashed X-Zepto-Allowed-Tables / X-Zepto-Tenant-Id).
struct HttpResp { int status = 0; std::string body; };

HttpResp http_post(uint16_t port,
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

    std::string req = "POST / HTTP/1.1\r\nHost: localhost\r\n"
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
    r.body = (pos != std::string::npos) ? raw.substr(pos + 4) : "";
    return r;
}

std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

class TableAclDdlTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_     = zepto_test_util::pick_free_port();
        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
        server_   = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
        tenant_   = std::make_shared<zeptodb::auth::TenantManager>();
        server_->set_tenant_manager(tenant_);
        // Pre-create "trades" so DESCRIBE against allowed table succeeds.
        executor_->execute(
            "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp INT64)",
            nullptr);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }
    void TearDown() override { server_->stop(); }

    uint16_t port_{};
    std::unique_ptr<ZeptoPipeline>                 pipeline_;
    std::unique_ptr<QueryExecutor>                 executor_;
    std::unique_ptr<zeptodb::server::HttpServer>   server_;
    std::shared_ptr<zeptodb::auth::TenantManager>  tenant_;
};

// --------------------------------------------------------------------------
// E1: allowed_tables covers DDL + DESCRIBE
// --------------------------------------------------------------------------
TEST_F(TableAclDdlTest, TableACL_DenyCreateTable_NotInAllowedList) {
    auto r = http_post(port_,
        "CREATE TABLE forbidden (symbol INT64, price INT64, volume INT64, timestamp INT64)",
        "X-Zepto-Allowed-Tables: trades\r\n");
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(r.body.find("forbidden"), std::string::npos);
}

TEST_F(TableAclDdlTest, TableACL_DenyDropTable_NotInAllowedList) {
    auto r = http_post(port_, "DROP TABLE forbidden",
                       "X-Zepto-Allowed-Tables: trades\r\n");
    EXPECT_EQ(r.status, 403);
}

TEST_F(TableAclDdlTest, TableACL_DenyAlterTable_NotInAllowedList) {
    auto r = http_post(port_, "ALTER TABLE forbidden ADD COLUMN flag INT64",
                       "X-Zepto-Allowed-Tables: trades\r\n");
    EXPECT_EQ(r.status, 403);
}

TEST_F(TableAclDdlTest, TableACL_AllowDescribeAllowedTable) {
    auto r = http_post(port_, "DESCRIBE trades",
                       "X-Zepto-Allowed-Tables: trades\r\n");
    EXPECT_EQ(r.status, 200);
}

TEST_F(TableAclDdlTest, TableACL_DenyDescribeForbiddenTable) {
    auto r = http_post(port_, "DESCRIBE secret",
                       "X-Zepto-Allowed-Tables: trades\r\n");
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
                       "X-Zepto-Tenant-Id: tA\r\n");
    EXPECT_EQ(r.status, 403);
}

TEST_F(TableAclDdlTest, TenantNamespace_AllowTableInsideNamespace) {
    // Use an unquoted-identifier-safe namespace so the table can be named
    // without quoting; this lets us assert a strict 200 (devlog 091 F3).
    zeptodb::auth::TenantConfig c;
    c.tenant_id = "tB"; c.table_namespace = "deska_";
    ASSERT_TRUE(tenant_->create_tenant(c));
    auto create_res = http_post(port_,
        "CREATE TABLE deska_trades (symbol INT64, price INT64, volume INT64, timestamp INT64)");
    ASSERT_EQ(create_res.status, 200);
    auto r = http_post(port_, "SELECT * FROM deska_trades",
                       "X-Zepto-Tenant-Id: tB\r\n");
    EXPECT_EQ(r.status, 200);
}

TEST_F(TableAclDdlTest, TenantNamespace_NoNamespace_Unrestricted) {
    zeptodb::auth::TenantConfig c;
    c.tenant_id = "tC"; // empty table_namespace
    ASSERT_TRUE(tenant_->create_tenant(c));
    auto r = http_post(port_, "SELECT * FROM trades",
                       "X-Zepto-Tenant-Id: tC\r\n");
    EXPECT_EQ(r.status, 200);
}

TEST_F(TableAclDdlTest, TenantNamespace_NoTenant_Unrestricted) {
    // No X-Zepto-Tenant-Id header → enforcement skipped.
    auto r = http_post(port_, "SELECT * FROM trades");
    EXPECT_EQ(r.status, 200);
}

// --------------------------------------------------------------------------
// F4: HTTP POST primes executor prepared-statement cache (devlog 091)
// --------------------------------------------------------------------------
TEST_F(TableAclDdlTest, HttpPostCachePriming_PostPrimesExecutorCache) {
    executor_->clear_prepared_cache();
    const size_t before = executor_->prepared_cache_size();
    // 200 or 400 is fine — what matters is that the ACL-block parse
    // was threaded through cache_prepared() before executor dispatch.
    (void)http_post(port_, "SELECT * FROM trades");
    const size_t after = executor_->prepared_cache_size();
    EXPECT_GT(after, before);
}

} // namespace
