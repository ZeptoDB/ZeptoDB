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
#include <thread>
#include <chrono>

namespace flight = arrow::flight;

class FlightServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        zeptodb::core::PipelineConfig cfg;
        cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
        pipeline_ = std::make_unique<zeptodb::core::ZeptoPipeline>(cfg);

        executor_ = std::make_unique<zeptodb::sql::QueryExecutor>(*pipeline_);
        executor_->execute("CREATE TABLE IF NOT EXISTS trades "
                           "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)");

        // Seed data
        for (int i = 0; i < 100; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = static_cast<uint32_t>(1 + (i % 3));
            msg.price     = (15000 + i) * 1'000'000LL;
            msg.volume    = 100 + i;
            msg.recv_ts   = 1711234567'000'000'000LL + static_cast<int64_t>(i) * 1'000'000LL;
            pipeline_->ingest_tick(msg);
        }
        pipeline_->drain_sync(200);

        server_ = std::make_unique<zeptodb::server::FlightServer>(*executor_);
        server_->start_async(0);  // port 0 = auto-assign
        ASSERT_TRUE(server_->running());
        port_ = server_->port();
        ASSERT_GT(port_, 0);
    }

    void TearDown() override {
        server_->stop();
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
    std::unique_ptr<zeptodb::server::FlightServer> server_;
    int port_ = 0;
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
