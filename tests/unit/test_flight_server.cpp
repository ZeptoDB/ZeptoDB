// ============================================================================
// ZeptoDB: Arrow Flight Server Tests
// ============================================================================
#include <gtest/gtest.h>
#include "zeptodb/server/flight_server.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"

#ifdef ZEPTO_FLIGHT_ENABLED

#include <arrow/api.h>
#include <arrow/flight/api.h>
#include <arrow/flight/client.h>
#include <arrow/testing/gtest_util.h>
#include <filesystem>
#include <set>
#include <thread>
#include <chrono>

namespace flight = arrow::flight;

class FlightServerTest : public ::testing::Test {
protected:
    virtual void configure_security() {}
    virtual void cleanup_security() {}
    virtual zeptodb::server::FlightServerConfig server_config() {
        zeptodb::server::FlightServerConfig config;
        config.port = 0;
        // Existing ingestion tests exercise the explicitly opted-in legacy
        // path. Production/default behavior is covered separately below.
        config.allow_non_atomic_put = true;
        return config;
    }

    void SetUp() override {
        zeptodb::core::PipelineConfig cfg;
        cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
        pipeline_ = std::make_unique<zeptodb::core::ZeptoPipeline>(cfg);

        executor_ = std::make_unique<zeptodb::sql::QueryExecutor>(*pipeline_);
        executor_->execute("CREATE TABLE IF NOT EXISTS trades "
                           "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)");
        const uint16_t tid = pipeline_->schema_registry().get_table_id("trades");

        // Seed data
        for (int i = 0; i < 100; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = static_cast<uint32_t>(1 + (i % 3));
            msg.price     = (15000 + i) * 1'000'000LL;
            msg.volume    = 100 + i;
            msg.recv_ts   = 1711234567'000'000'000LL + static_cast<int64_t>(i) * 1'000'000LL;
            msg.table_id  = tid;
            pipeline_->ingest_tick(msg);
        }
        pipeline_->drain_sync(200);

        configure_security();
        server_ = std::make_unique<zeptodb::server::FlightServer>(
            *executor_, auth_, tenant_manager_);
        ASSERT_TRUE(server_->start_async(server_config()));
        ASSERT_TRUE(server_->running());
        port_ = server_->port();
        ASSERT_GT(port_, 0);
    }

    void TearDown() override {
        server_->stop();
        cleanup_security();
    }

    std::unique_ptr<flight::FlightClient> connect() {
        auto loc = flight::Location::ForGrpcTcp("localhost", port_);
        EXPECT_TRUE(loc.ok());
        auto client = flight::FlightClient::Connect(*loc);
        EXPECT_TRUE(client.ok());
        return std::move(*client);
    }

    std::unique_ptr<zeptodb::core::ZeptoPipeline> pipeline_;
    std::unique_ptr<zeptodb::sql::QueryExecutor> executor_;
    std::shared_ptr<zeptodb::auth::AuthManager> auth_;
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_manager_;
    std::unique_ptr<zeptodb::server::FlightServer> server_;
    int port_ = 0;
};

static flight::FlightCallOptions auth_options(
    const std::string& key,
    std::vector<std::pair<std::string, std::string>> extra_headers = {}) {
    flight::FlightCallOptions options;
    options.headers.emplace_back("authorization", "Bearer " + key);
    options.headers.insert(
        options.headers.end(),
        std::make_move_iterator(extra_headers.begin()),
        std::make_move_iterator(extra_headers.end()));
    return options;
}

static void expect_flight_status(
    const arrow::Status& status,
    flight::FlightStatusCode expected) {
    const auto detail = flight::FlightStatusDetail::UnwrapStatus(status);
    ASSERT_NE(detail, nullptr) << status.ToString();
    EXPECT_EQ(detail->code(), expected) << status.ToString();
}

class FlightAuthServerTest : public FlightServerTest {
protected:
    void configure_security() override {
        ASSERT_TRUE(executor_->execute(
            "CREATE TABLE secret "
            "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)").ok());

        key_file_ = std::filesystem::temp_directory_path() /
            ("zeptodb-flight-auth-" +
             std::to_string(reinterpret_cast<uintptr_t>(this)) + ".keys");
        std::error_code ignored;
        std::filesystem::remove(key_file_, ignored);

        zeptodb::auth::AuthManager::Config config;
        config.api_keys_file = key_file_.string();
        config.rate_limit_enabled = false;
        config.audit_enabled = true;
        config.audit_buffer_enabled = true;
        auth_ = std::make_shared<zeptodb::auth::AuthManager>(config);

        reader_key_ = auth_->create_api_key(
            "flight-reader", zeptodb::auth::Role::READER);
        writer_key_ = auth_->create_api_key(
            "flight-writer", zeptodb::auth::Role::WRITER);
        admin_key_ = auth_->create_api_key(
            "flight-admin", zeptodb::auth::Role::ADMIN);
        limited_reader_key_ = auth_->create_api_key(
            "flight-limited-reader", zeptodb::auth::Role::READER,
            {}, {"trades"});
        limited_writer_key_ = auth_->create_api_key(
            "flight-limited-writer", zeptodb::auth::Role::WRITER,
            {}, {"trades"});
        tenant_reader_key_ = auth_->create_api_key(
            "flight-tenant-reader", zeptodb::auth::Role::READER,
            {}, {}, "tenant-a");
        symbol_reader_key_ = auth_->create_api_key(
            "flight-symbol-reader", zeptodb::auth::Role::READER,
            {"AAPL"});

        tenant_manager_ =
            std::make_shared<zeptodb::auth::TenantManager>();
        zeptodb::auth::TenantConfig tenant;
        tenant.tenant_id = "tenant-a";
        tenant.name = "Tenant A";
        tenant.max_concurrent_queries = 1;
        tenant.table_namespace = "tenant_a.";
        ASSERT_TRUE(tenant_manager_->create_tenant(std::move(tenant)));
    }

    void cleanup_security() override {
        std::error_code ignored;
        std::filesystem::remove(key_file_, ignored);
    }

    std::filesystem::path key_file_;
    std::string reader_key_;
    std::string writer_key_;
    std::string admin_key_;
    std::string limited_reader_key_;
    std::string limited_writer_key_;
    std::string tenant_reader_key_;
    std::string symbol_reader_key_;
};

class FlightDisabledAuthServerTest : public FlightServerTest {
protected:
    void configure_security() override {
        zeptodb::auth::AuthManager::Config config;
        config.enabled = false;
        config.rate_limit_enabled = false;
        config.audit_enabled = false;
        config.audit_buffer_enabled = false;
        auth_ = std::make_shared<zeptodb::auth::AuthManager>(config);
    }
};

class FlightLimitServerTest : public FlightServerTest {
protected:
    zeptodb::server::FlightServerConfig server_config() override {
        auto config = FlightServerTest::server_config();
        config.max_query_rows = 5;
        config.max_query_bytes = 1U * 1024U * 1024U;
        config.max_put_rows = 3;
        config.max_put_bytes = 1U * 1024U * 1024U;
        return config;
    }
};

class FlightPutDisabledServerTest : public FlightServerTest {
protected:
    zeptodb::server::FlightServerConfig server_config() override {
        auto config = FlightServerTest::server_config();
        config.allow_non_atomic_put = false;
        return config;
    }
};

class FlightTimeoutServerTest : public FlightServerTest {
protected:
    zeptodb::server::FlightServerConfig server_config() override {
        auto config = FlightServerTest::server_config();
        config.query_timeout_ms = 1;
        config.max_query_rows = 500000;
        config.max_query_bytes = 256U * 1024U * 1024U;
        return config;
    }
};

TEST_F(FlightServerTest, DoGetSelectCount) {
    auto client = connect();
    flight::Ticket ticket;
    ticket.ticket = "SELECT COUNT(*) FROM trades";
    auto stream = client->DoGet(ticket);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();

    auto table = (*stream)->ToTable();
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->num_rows(), 1);
    EXPECT_GE((*table)->num_columns(), 1);

    auto col = (*table)->column(0);
    auto chunk = std::static_pointer_cast<arrow::Int64Array>(col->chunk(0));
    EXPECT_EQ(chunk->Value(0), 100);
}

TEST_F(FlightServerTest, DoGetSelectAll) {
    auto client = connect();
    flight::Ticket ticket;
    ticket.ticket = "SELECT * FROM trades LIMIT 10";
    auto stream = client->DoGet(ticket);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();

    auto table = (*stream)->ToTable();
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->num_rows(), 10);
    EXPECT_GE((*table)->num_columns(), 4);  // symbol, price, volume, timestamp
}

TEST_F(FlightLimitServerTest, ReadWithoutLimitFailsAtConfiguredRowCap) {
    auto client = connect();
    const flight::Ticket ticket{"SELECT * FROM trades"};
    const auto stream = client->DoGet(ticket);
    ASSERT_FALSE(stream.ok());
    EXPECT_NE(stream.status().ToString().find("row limit"), std::string::npos);
}

TEST_F(FlightLimitServerTest, DoPutFailsBeforeOversizedBatchMutation) {
    auto client = connect();
    const auto schema = arrow::schema({
        arrow::field("symbol", arrow::int64()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
        arrow::field("timestamp", arrow::int64()),
    });
    auto put = client->DoPut(
        flight::FlightCallOptions{},
        flight::FlightDescriptor::Command("trades"), schema);
    ASSERT_TRUE(put.ok()) << put.status().ToString();
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (int index = 0; index < 4; ++index) {
        columns.push_back(std::make_shared<arrow::Int64Array>(
            4, arrow::Buffer::FromString(std::string(32, '\0'))));
    }
    const auto batch = arrow::RecordBatch::Make(schema, 4, std::move(columns));
    ASSERT_TRUE(put->writer->WriteRecordBatch(*batch).ok());
    const auto close = put->writer->Close();
    EXPECT_FALSE(close.ok());
    EXPECT_NE(close.ToString().find("row or byte limit"), std::string::npos);

    const auto count = executor_->execute("SELECT COUNT(*) FROM trades");
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.rows[0][0], 100);
}

TEST_F(FlightPutDisabledServerTest, DoPutIsDisabledByProductionDefault) {
    auto client = connect();
    const auto schema = arrow::schema({
        arrow::field("symbol", arrow::int64()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
        arrow::field("timestamp", arrow::int64()),
    });
    auto put = client->DoPut(
        flight::FlightCallOptions{},
        flight::FlightDescriptor::Command("trades"), schema);
    if (put.ok()) {
        const auto close = put->writer->Close();
        ASSERT_FALSE(close.ok());
        EXPECT_NE(close.ToString().find("atomic stream commit"),
                  std::string::npos);
    } else {
        EXPECT_NE(put.status().ToString().find("atomic stream commit"),
                  std::string::npos);
    }

    const auto count = executor_->execute("SELECT COUNT(*) FROM trades");
    ASSERT_TRUE(count.ok()) << count.error;
    EXPECT_EQ(count.rows[0][0], 100);
}

TEST_F(FlightTimeoutServerTest, ReadQueryHonorsCooperativeTimeout) {
    const uint16_t table_id =
        pipeline_->schema_registry().get_table_id("trades");
    constexpr int extra_rows = 200000;
    for (int index = 0; index < extra_rows; ++index) {
        zeptodb::ingestion::TickMessage message{};
        message.symbol_id = static_cast<uint32_t>(1 + (index % 32));
        message.price = index;
        message.volume = index + 1;
        message.recv_ts = index + 1;
        message.table_id = table_id;
        ASSERT_TRUE(pipeline_->ingest_tick(message));
        if ((index + 1) % 8192 == 0) pipeline_->drain_sync(8192);
    }
    pipeline_->drain_sync(extra_rows);

    auto client = connect();
    const flight::Ticket ticket{
        "SELECT * FROM trades ORDER BY price DESC"};
    const auto stream = client->DoGet(ticket);
    ASSERT_FALSE(stream.ok());
    expect_flight_status(
        stream.status(), flight::FlightStatusCode::TimedOut);
    EXPECT_NE(stream.status().ToString().find("timed out"),
              std::string::npos);
}

TEST_F(FlightServerTest, GetFlightInfo) {
    auto client = connect();
    auto desc = flight::FlightDescriptor::Command("SELECT COUNT(*) FROM trades");
    auto info = client->GetFlightInfo(desc);
    ASSERT_TRUE(info.ok()) << info.status().ToString();
    EXPECT_EQ((*info)->total_records(), 1);
    EXPECT_GE((*info)->endpoints().size(), 1u);
}

TEST_F(FlightServerTest, DoActionPing) {
    auto client = connect();
    flight::Action action;
    action.type = "ping";
    auto stream = client->DoAction(action);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();

    auto result = (*stream)->Next();
    ASSERT_TRUE(result.ok());
    ASSERT_NE(*result, nullptr);
    std::string body(reinterpret_cast<const char*>((*result)->body->data()),
                     (*result)->body->size());
    EXPECT_NE(body.find("ok"), std::string::npos);
}

TEST_F(FlightServerTest, ListActions) {
    auto client = connect();
    auto actions = client->ListActions();
    ASSERT_TRUE(actions.ok());
    EXPECT_GE(actions->size(), 2u);
}

TEST_F(FlightServerTest, DoGetInvalidSQL) {
    auto client = connect();
    flight::Ticket ticket;
    ticket.ticket = "SELECT * FROM nonexistent_table";
    auto stream = client->DoGet(ticket);
    // Should fail with execution error
    if (stream.ok()) {
        auto table = (*stream)->ToTable();
        // Either the stream creation or table read should fail
        // (depends on whether executor returns error or empty result)
    }
    // If we get here without crash, the error was handled gracefully
    SUCCEED();
}

TEST_F(FlightServerTest, ServerStartStop) {
    // Server is already running from SetUp
    EXPECT_TRUE(server_->running());
    server_->stop();
    EXPECT_FALSE(server_->running());
}

TEST_F(FlightAuthServerTest, MissingAndInvalidCredentialsAreUnauthenticated) {
    auto client = connect();
    const flight::Ticket ticket{"SELECT COUNT(*) FROM trades"};

    auto missing = client->DoGet(ticket);
    ASSERT_FALSE(missing.ok());
    expect_flight_status(
        missing.status(), flight::FlightStatusCode::Unauthenticated);

    auto invalid = client->DoGet(auth_options("not-a-valid-key"), ticket);
    ASSERT_FALSE(invalid.ok());
    expect_flight_status(
        invalid.status(), flight::FlightStatusCode::Unauthenticated);
}

TEST_F(FlightAuthServerTest, GetFlightInfoRequiresAuthAndAcceptsReaderMetadata) {
    auto client = connect();
    const auto descriptor = flight::FlightDescriptor::Command(
        "SELECT COUNT(*) FROM trades");

    auto missing = client->GetFlightInfo(descriptor);
    ASSERT_FALSE(missing.ok());
    expect_flight_status(
        missing.status(), flight::FlightStatusCode::Unauthenticated);

    auto allowed = client->GetFlightInfo(
        auth_options(reader_key_), descriptor);
    ASSERT_TRUE(allowed.ok()) << allowed.status().ToString();
    EXPECT_EQ((*allowed)->total_records(), 1);
}

TEST_F(FlightAuthServerTest, ReaderCanSelectWithAuthorizationMetadata) {
    auto client = connect();
    const flight::Ticket ticket{"SELECT COUNT(*) FROM trades"};
    auto stream = client->DoGet(auth_options(reader_key_), ticket);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();

    auto table = (*stream)->ToTable();
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    ASSERT_EQ((*table)->num_rows(), 1);
    const auto values = std::static_pointer_cast<arrow::Int64Array>(
        (*table)->column(0)->chunk(0));
    EXPECT_EQ(values->Value(0), 100);
}

TEST_F(FlightAuthServerTest, ReaderCanDescribeAsUtf8Columns) {
    auto client = connect();
    const flight::Ticket ticket{"DESCRIBE trades"};
    auto stream = client->DoGet(auth_options(reader_key_), ticket);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();

    auto table = (*stream)->ToTable();
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->num_rows(), 4);
    ASSERT_EQ((*table)->num_columns(), 2);
    EXPECT_EQ((*table)->schema()->field(0)->type()->id(), arrow::Type::STRING);
    EXPECT_EQ((*table)->schema()->field(1)->type()->id(), arrow::Type::STRING);
}

TEST_F(FlightAuthServerTest, AuthManagerCheckRunsOncePerRpc) {
    ASSERT_EQ(auth_->audit_buffer().size(), 0u);
    auto client = connect();
    const flight::Ticket ticket{"SELECT COUNT(*) FROM trades"};
    auto stream = client->DoGet(auth_options(reader_key_), ticket);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();
    ASSERT_TRUE((*stream)->ToTable().ok());
    EXPECT_EQ(auth_->audit_buffer().size(), 1u);
}

TEST_F(FlightAuthServerTest, QueryTicketsRejectMutationEvenForAdminAndSpoofedRole) {
    auto client = connect();
    const flight::Ticket insert{
        "INSERT INTO trades VALUES (1, 1, 1, 1)"};

    auto spoofed = client->DoGet(
        auth_options(reader_key_, {{"x-zepto-role", "admin"}}), insert);
    ASSERT_FALSE(spoofed.ok());
    expect_flight_status(
        spoofed.status(), flight::FlightStatusCode::Unauthorized);

    auto admin = client->DoGet(auth_options(admin_key_), insert);
    ASSERT_FALSE(admin.ok());
    expect_flight_status(
        admin.status(), flight::FlightStatusCode::Unauthorized);

    const auto count = executor_->execute("SELECT COUNT(*) FROM trades");
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.rows[0][0], 100);
}

TEST_F(FlightAuthServerTest, AuthenticatedMalformedSqlFailsClosed) {
    auto client = connect();
    const flight::Ticket malformed{"SELEC * FROM trades"};
    auto result = client->DoGet(auth_options(reader_key_), malformed);
    ASSERT_FALSE(result.ok());
    expect_flight_status(
        result.status(), flight::FlightStatusCode::Unauthorized);
}

TEST_F(FlightAuthServerTest, RecursiveJoinAndSequentialCteAclCannotBeBypassed) {
    auto client = connect();
    const auto options = auth_options(limited_reader_key_);

    const flight::Ticket join{
        "SELECT trades.price FROM trades "
        "JOIN secret ON trades.timestamp = secret.timestamp"};
    auto join_result = client->DoGet(options, join);
    ASSERT_FALSE(join_result.ok());
    expect_flight_status(
        join_result.status(), flight::FlightStatusCode::Unauthorized);

    const flight::Ticket forward_shadow{
        "WITH a AS (SELECT * FROM secret), "
        "secret AS (SELECT * FROM trades) SELECT * FROM a"};
    auto cte_result = client->DoGet(options, forward_shadow);
    ASSERT_FALSE(cte_result.ok());
    expect_flight_status(
        cte_result.status(), flight::FlightStatusCode::Unauthorized);
}

TEST_F(FlightAuthServerTest, TenantNamespaceRejectsOutOfScopeTable) {
    auto client = connect();
    const flight::Ticket ticket{"SELECT COUNT(*) FROM trades"};
    auto result = client->DoGet(auth_options(tenant_reader_key_), ticket);
    ASSERT_FALSE(result.ok());
    expect_flight_status(
        result.status(), flight::FlightStatusCode::Unauthorized);
}

TEST_F(FlightAuthServerTest, TenantConcurrencyQuotaAppliesToFlightReads) {
    ASSERT_TRUE(tenant_manager_->acquire_query_slot("tenant-a"));

    auto client = connect();
    const flight::Ticket ticket{"SHOW TABLES"};
    const auto blocked =
        client->DoGet(auth_options(tenant_reader_key_), ticket);
    tenant_manager_->release_query_slot("tenant-a");

    ASSERT_FALSE(blocked.ok());
    expect_flight_status(
        blocked.status(), flight::FlightStatusCode::Unavailable);
    EXPECT_NE(blocked.status().ToString().find("concurrency limit"),
              std::string::npos);

    auto allowed = client->DoGet(auth_options(tenant_reader_key_), ticket);
    ASSERT_TRUE(allowed.ok()) << allowed.status().ToString();
    ASSERT_TRUE((*allowed)->ToTable().ok());

    const auto usage = tenant_manager_->usage("tenant-a");
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->active_queries, 0u);
    EXPECT_EQ(usage->rejected_queries, 1u);
}

TEST_F(FlightAuthServerTest, SymbolScopedIdentityFailsClosed) {
    auto client = connect();
    const flight::Ticket ticket{"SELECT COUNT(*) FROM trades"};
    auto result = client->DoGet(auth_options(symbol_reader_key_), ticket);
    ASSERT_FALSE(result.ok());
    expect_flight_status(
        result.status(), flight::FlightStatusCode::Unauthorized);
}

TEST_F(FlightAuthServerTest, ListFlightsAndShowTablesFilterRestrictedTables) {
    auto client = connect();
    const auto options = auth_options(limited_reader_key_);

    auto listing = client->ListFlights(options, flight::Criteria{});
    ASSERT_TRUE(listing.ok()) << listing.status().ToString();
    std::set<std::string> listed_tables;
    while (true) {
        auto next = (*listing)->Next();
        ASSERT_TRUE(next.ok()) << next.status().ToString();
        if (!*next) break;
        listed_tables.insert((*next)->descriptor().cmd);
    }
    EXPECT_EQ(listed_tables, std::set<std::string>{"trades"});

    const flight::Ticket show_tables{"SHOW TABLES"};
    auto stream = client->DoGet(options, show_tables);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();
    auto table = (*stream)->ToTable();
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->num_rows(), 1);
}

TEST_F(FlightAuthServerTest, HealthActionsRemainPublicWithAuthEnabled) {
    auto client = connect();
    flight::Action action;
    action.type = "healthcheck";
    auto results = client->DoAction(action);
    ASSERT_TRUE(results.ok()) << results.status().ToString();
    auto first = (*results)->Next();
    ASSERT_TRUE(first.ok()) << first.status().ToString();
    ASSERT_NE(*first, nullptr);
}

TEST_F(FlightAuthServerTest, DoPutRequiresWriteAndTableScope) {
    auto client = connect();
    const auto schema = arrow::schema({
        arrow::field("symbol", arrow::int64()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
        arrow::field("timestamp", arrow::int64()),
    });

    auto reader_put = client->DoPut(
        auth_options(reader_key_),
        flight::FlightDescriptor::Command("trades"), schema);
    if (reader_put.ok()) {
        const auto status = reader_put->writer->Close();
        ASSERT_FALSE(status.ok());
        expect_flight_status(status, flight::FlightStatusCode::Unauthorized);
    } else {
        expect_flight_status(
            reader_put.status(), flight::FlightStatusCode::Unauthorized);
    }

    auto restricted_put = client->DoPut(
        auth_options(limited_writer_key_),
        flight::FlightDescriptor::Command("secret"), schema);
    if (restricted_put.ok()) {
        const auto status = restricted_put->writer->Close();
        ASSERT_FALSE(status.ok());
        expect_flight_status(status, flight::FlightStatusCode::Unauthorized);
    } else {
        expect_flight_status(
            restricted_put.status(), flight::FlightStatusCode::Unauthorized);
    }

    auto writer_put = client->DoPut(
        auth_options(writer_key_),
        flight::FlightDescriptor::Command("trades"), schema);
    ASSERT_TRUE(writer_put.ok()) << writer_put.status().ToString();
    const auto batch = arrow::RecordBatch::Make(
        schema, 1,
        {
            std::make_shared<arrow::Int64Array>(
                1, arrow::Buffer::FromString(std::string(8, '\0'))),
            std::make_shared<arrow::Int64Array>(
                1, arrow::Buffer::FromString(std::string(8, '\0'))),
            std::make_shared<arrow::Int64Array>(
                1, arrow::Buffer::FromString(std::string(8, '\0'))),
            std::make_shared<arrow::Int64Array>(
                1, arrow::Buffer::FromString(std::string(8, '\0'))),
        });
    ASSERT_TRUE(writer_put->writer->WriteRecordBatch(*batch).ok());
    ASSERT_TRUE(writer_put->writer->Close().ok());

    const auto count = executor_->execute("SELECT COUNT(*) FROM trades");
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count.rows[0][0], 101);
}

TEST_F(FlightDisabledAuthServerTest, DisabledAuthKeepsReadCompatibility) {
    auto client = connect();
    const flight::Ticket ticket{"SELECT COUNT(*) FROM trades"};
    auto stream = client->DoGet(ticket);
    ASSERT_TRUE(stream.ok()) << stream.status().ToString();
    ASSERT_TRUE((*stream)->ToTable().ok());
}

TEST(FlightServerConfigTest, RejectsInsecureOrIncompleteTlsConfiguration) {
    zeptodb::core::PipelineConfig pipeline_config;
    pipeline_config.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pipeline_config);
    zeptodb::sql::QueryExecutor executor(pipeline);

    zeptodb::server::FlightServer insecure_server(executor);
    zeptodb::server::FlightServerConfig insecure;
    insecure.host = "0.0.0.0";
    insecure.port = 0;
    EXPECT_FALSE(insecure_server.start_async(insecure));
    EXPECT_FALSE(insecure_server.running());

    zeptodb::server::FlightServer loopback_prefix_server(executor);
    auto loopback_prefix = insecure;
    loopback_prefix.host = "127.attacker.example";
    EXPECT_FALSE(loopback_prefix_server.start_async(loopback_prefix));
    EXPECT_FALSE(loopback_prefix_server.running());

    zeptodb::server::FlightServer incomplete_tls_server(executor);
    zeptodb::server::FlightServerConfig incomplete_tls;
    incomplete_tls.port = 0;
    incomplete_tls.tls_cert_path = "/tmp/cert-only.pem";
    EXPECT_FALSE(incomplete_tls_server.start_async(incomplete_tls));
    EXPECT_FALSE(incomplete_tls_server.running());

    zeptodb::server::FlightServer zero_limit_server(executor);
    zeptodb::server::FlightServerConfig zero_limit;
    zero_limit.port = 0;
    zero_limit.max_put_rows = 0;
    EXPECT_FALSE(zero_limit_server.start_async(zero_limit));
    EXPECT_FALSE(zero_limit_server.running());
}

#else // !ZEPTO_FLIGHT_ENABLED

TEST(FlightServerStub, StubCompiles) {
    // Verify stub compiles and doesn't crash
    zeptodb::core::PipelineConfig cfg;
    cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(cfg);
    zeptodb::sql::QueryExecutor executor(pipeline);
    zeptodb::server::FlightServer server(executor);
    EXPECT_FALSE(server.running());
}

#endif // ZEPTO_FLIGHT_ENABLED
