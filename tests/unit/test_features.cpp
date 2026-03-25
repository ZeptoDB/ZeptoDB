// ============================================================================
// ZeptoDB: Feature Tests
// Tests for:
//   1. s# sorted column attribute hint — Partition::set_sorted / sorted_range
//      and executor SQL query optimization
//   2. Connection hooks — HttpServer on_connect / on_disconnect / list_sessions
//   3. \t <sql> one-shot timer — tested via BuiltinCommands logic
//   4. HttpServer::add_metrics_provider — extensible /metrics endpoint
// ============================================================================

#include <gtest/gtest.h>

// --- Storage ---
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/storage/column_store.h"

// --- SQL ---
#include "zeptodb/sql/executor.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/ingestion/tick_plant.h"

// --- Server ---
#include "zeptodb/server/http_server.h"

// --- Feeds ---
#include "zeptodb/feeds/kafka_consumer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace zeptodb::storage;
using namespace zeptodb::sql;
using namespace zeptodb::core;
using namespace std::chrono_literals;

// ============================================================================
// Helpers
// ============================================================================

static std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

// Send a raw HTTP request to localhost:port, return response body string.
// is_closing=true adds "Connection: close" header.
static std::string http_get(int port, const std::string& path,
                             bool is_closing = false) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return "";
    }

    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: localhost\r\n";
    if (is_closing)
        req += "Connection: close\r\n";
    req += "\r\n";

    ::send(fd, req.c_str(), req.size(), 0);

    std::string raw;
    char buf[4096];
    ssize_t n;
    // set a small receive timeout so we don't hang
    struct timeval tv{ 1, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    auto pos = raw.find("\r\n\r\n");
    return (pos != std::string::npos) ? raw.substr(pos + 4) : raw;
}

// ============================================================================
// Part 1: s# Sorted Column — Partition level via pipeline
// ============================================================================

// We test sorted_range / is_sorted via a live partition obtained from the
// pipeline's PartitionManager (the normal append path).
class SortedColumnTest : public ::testing::Test {
protected:
    void SetUp() override {
        pipeline_ = make_pipeline();
        executor_  = std::make_unique<QueryExecutor>(*pipeline_);

        // Insert 5 rows for symbol=1 with monotonically increasing price
        // prices: 100, 200, 300, 400, 500
        for (int i = 0; i < 5; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = 1;
            msg.recv_ts   = 1000LL + i;
            msg.price     = (i + 1) * 100;  // 100,200,300,400,500
            msg.volume    = 10 + i;
            msg.msg_type  = 0;
            pipeline_->ingest_tick(msg);
        }
        pipeline_->drain_sync(5);

        // Grab the single partition created
        auto parts = pipeline_->partition_manager().get_all_partitions();
        ASSERT_FALSE(parts.empty());
        part_ = parts[0];
    }

    std::unique_ptr<ZeptoPipeline>  pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
    Partition*                     part_ = nullptr;
};

TEST_F(SortedColumnTest, DefaultNotSorted) {
    EXPECT_FALSE(part_->is_sorted("price"));
}

TEST_F(SortedColumnTest, SetAndCheckSorted) {
    part_->set_sorted("price");
    EXPECT_TRUE(part_->is_sorted("price"));
    EXPECT_FALSE(part_->is_sorted("volume"));  // other column untouched
}

TEST_F(SortedColumnTest, SortedRangeFullSpan) {
    part_->set_sorted("price");
    auto [lo, hi] = part_->sorted_range("price", 100, 500);
    EXPECT_EQ(lo, 0u);
    EXPECT_EQ(hi, 5u);
}

TEST_F(SortedColumnTest, SortedRangeMiddle) {
    part_->set_sorted("price");
    // [200, 400] → indices 1,2,3
    auto [lo, hi] = part_->sorted_range("price", 200, 400);
    EXPECT_EQ(lo, 1u);
    EXPECT_EQ(hi, 4u);
}

TEST_F(SortedColumnTest, SortedRangeExactMatch) {
    part_->set_sorted("price");
    auto [lo, hi] = part_->sorted_range("price", 300, 300);
    EXPECT_EQ(lo, 2u);
    EXPECT_EQ(hi, 3u);
}

TEST_F(SortedColumnTest, SortedRangeBelowAll) {
    part_->set_sorted("price");
    auto [lo, hi] = part_->sorted_range("price", 0, 50);
    EXPECT_EQ(lo, hi);  // empty
}

TEST_F(SortedColumnTest, SortedRangeAboveAll) {
    part_->set_sorted("price");
    auto [lo, hi] = part_->sorted_range("price", 600, 999);
    EXPECT_EQ(lo, hi);  // empty
}

TEST_F(SortedColumnTest, SortedRangeUnknownColumn) {
    // Non-existent column returns {0,0}
    auto [lo, hi] = part_->sorted_range("nonexistent", 0, 999);
    EXPECT_EQ(lo, 0u);
    EXPECT_EQ(hi, 0u);
}

// ============================================================================
// Part 2: s# Sorted Column — SQL query optimization via executor
// ============================================================================

class SortedColumnQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        pipeline_ = make_pipeline();
        executor_  = std::make_unique<QueryExecutor>(*pipeline_);

        // Insert 20 rows for symbol=1: price 1000, 1010, ..., 1190 (step 10)
        for (int i = 0; i < 20; ++i) {
            zeptodb::ingestion::TickMessage msg{};
            msg.symbol_id = 1;
            msg.recv_ts   = 1000LL + i;
            msg.price     = 1000 + i * 10;
            msg.volume    = 50 + i;
            msg.msg_type  = 0;
            pipeline_->ingest_tick(msg);
        }
        pipeline_->drain_sync(20);

        // Mark price as sorted on all partitions
        for (auto* part : pipeline_->partition_manager().get_all_partitions())
            part->set_sorted("price");
    }

    std::unique_ptr<ZeptoPipeline>  pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
};

TEST_F(SortedColumnQueryTest, BetweenOnSortedColumn) {
    // price BETWEEN 1050 AND 1100 → rows at price 1050,1060,1070,1080,1090,1100
    auto r = executor_->execute(
        "SELECT count(*) FROM trades WHERE price BETWEEN 1050 AND 1100");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 6);
}

TEST_F(SortedColumnQueryTest, GELEOnSortedColumn) {
    // price >= 1080 AND price <= 1120 → 1080,1090,1100,1110,1120 = 5 rows
    auto r = executor_->execute(
        "SELECT count(*) FROM trades WHERE price >= 1080 AND price <= 1120");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 5);
}

TEST_F(SortedColumnQueryTest, EQOnSortedColumn) {
    // price = 1050 → exactly 1 row
    auto r = executor_->execute(
        "SELECT count(*) FROM trades WHERE price = 1050");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 1);
}

TEST_F(SortedColumnQueryTest, OutOfRangeOnSortedColumn) {
    // price > 9999 → 0 rows
    auto r = executor_->execute(
        "SELECT count(*) FROM trades WHERE price > 9999");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 0);
}

TEST_F(SortedColumnQueryTest, RowsScannedReduced) {
    // exec_simple_select (no aggregation) uses the s# optimization.
    // A narrow range on a sorted column should scan far fewer than 20 rows.
    auto r = executor_->execute(
        "SELECT price FROM trades WHERE price BETWEEN 1000 AND 1020");
    ASSERT_TRUE(r.ok()) << r.error;
    // Prices 1000,1010,1020 — 3 rows
    EXPECT_EQ(r.rows.size(), 3u);
    // rows_scanned should reflect only the 3 rows in range, not all 20
    EXPECT_LE(r.rows_scanned, 5u);
}

// ============================================================================
// Part 3: Connection hooks — HttpServer
// ============================================================================

class ConnectionHooksTest : public ::testing::Test {
protected:
    void SetUp() override {
        pipeline_ = make_pipeline();
        executor_  = std::make_unique<QueryExecutor>(*pipeline_);
        server_    = std::make_unique<zeptodb::server::HttpServer>(*executor_, test_port_);

        connect_count_.store(0);
        disconnect_count_.store(0);

        server_->set_on_connect([this](const zeptodb::server::ConnectionInfo& info) {
            connect_count_.fetch_add(1);
            last_connect_addr_ = info.remote_addr;
        });
        server_->set_on_disconnect([this](const zeptodb::server::ConnectionInfo& info) {
            disconnect_count_.fetch_add(1);
            last_disconnect_addr_ = info.remote_addr;
        });

        server_->start_async();
        std::this_thread::sleep_for(60ms);  // wait for server to bind
    }

    void TearDown() override {
        server_->stop();
    }

    static constexpr uint16_t test_port_ = 19871;

    std::unique_ptr<ZeptoPipeline>            pipeline_;
    std::unique_ptr<QueryExecutor>           executor_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;

    std::atomic<int> connect_count_{0};
    std::atomic<int> disconnect_count_{0};
    std::string      last_connect_addr_;
    std::string      last_disconnect_addr_;
};

TEST_F(ConnectionHooksTest, OnConnectFiresOnFirstRequest) {
    http_get(test_port_, "/ping");
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(connect_count_.load(), 1);
}

TEST_F(ConnectionHooksTest, OnConnectFiresOnlyOnce) {
    // Same remote addr → on_connect fires once, query_count increments
    http_get(test_port_, "/ping");
    http_get(test_port_, "/ping");
    std::this_thread::sleep_for(20ms);
    // Both requests come from 127.0.0.1 — session reused
    EXPECT_EQ(connect_count_.load(), 1);
}

TEST_F(ConnectionHooksTest, OnDisconnectFiresOnConnectionClose) {
    http_get(test_port_, "/ping");       // create session
    std::this_thread::sleep_for(20ms);
    http_get(test_port_, "/ping", true); // Connection: close
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(disconnect_count_.load(), 1);
}

TEST_F(ConnectionHooksTest, ListSessionsReturnsActiveSession) {
    http_get(test_port_, "/ping");
    std::this_thread::sleep_for(20ms);
    auto sessions = server_->list_sessions();
    EXPECT_GE(sessions.size(), 1u);
    // The session should have at least 1 query counted
    bool found = false;
    for (const auto& s : sessions)
        if (s.query_count >= 1) { found = true; break; }
    EXPECT_TRUE(found);
}

TEST_F(ConnectionHooksTest, EvictIdleSessionsFiresOnDisconnect) {
    http_get(test_port_, "/ping");
    std::this_thread::sleep_for(20ms);

    ASSERT_EQ(server_->list_sessions().size(), 1u);

    // Evict with 0ms timeout → everything is "idle"
    size_t evicted = server_->evict_idle_sessions(0);
    std::this_thread::sleep_for(20ms);

    EXPECT_EQ(evicted, 1u);
    EXPECT_EQ(disconnect_count_.load(), 1);
    EXPECT_EQ(server_->list_sessions().size(), 0u);
}

TEST_F(ConnectionHooksTest, EvictIdleSessionsKeepsRecentSessions) {
    http_get(test_port_, "/ping");
    std::this_thread::sleep_for(20ms);

    // With a long timeout, nothing should be evicted
    size_t evicted = server_->evict_idle_sessions(60000);  // 60 seconds
    EXPECT_EQ(evicted, 0u);
    EXPECT_EQ(server_->list_sessions().size(), 1u);
}

TEST_F(ConnectionHooksTest, QueryCountIncrements) {
    http_get(test_port_, "/ping");
    http_get(test_port_, "/ping");
    http_get(test_port_, "/ping");
    std::this_thread::sleep_for(30ms);

    auto sessions = server_->list_sessions();
    ASSERT_GE(sessions.size(), 1u);

    uint64_t total = 0;
    for (const auto& s : sessions) total += s.query_count;
    EXPECT_GE(total, 2u);  // at least 2 queries tracked (timing may vary)
}

// ============================================================================
// Part 4: HttpServer::add_metrics_provider — extensible /metrics endpoint
// ============================================================================

class MetricsProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        pipeline_ = make_pipeline();
        executor_  = std::make_unique<QueryExecutor>(*pipeline_);
        server_    = std::make_unique<zeptodb::server::HttpServer>(*executor_, test_port_);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }

    void TearDown() override {
        server_->stop();
    }

    static constexpr uint16_t test_port_ = 19872;

    std::unique_ptr<ZeptoPipeline>             pipeline_;
    std::unique_ptr<QueryExecutor>            executor_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
};

TEST_F(MetricsProviderTest, DefaultMetricsContainApexCounters) {
    const std::string body = http_get(test_port_, "/metrics");
    EXPECT_NE(body.find("zepto_ticks_ingested_total"), std::string::npos);
    EXPECT_NE(body.find("zepto_server_ready"),          std::string::npos);
}

TEST_F(MetricsProviderTest, RegisteredProviderAppearsInOutput) {
    server_->add_metrics_provider([]() {
        return std::string("# HELP my_custom_counter Test counter\n"
                           "# TYPE my_custom_counter counter\n"
                           "my_custom_counter 42\n");
    });

    const std::string body = http_get(test_port_, "/metrics");
    EXPECT_NE(body.find("my_custom_counter 42"), std::string::npos);
    EXPECT_NE(body.find("zepto_ticks_ingested_total"), std::string::npos);
}

TEST_F(MetricsProviderTest, MultipleProvidersAllAppear) {
    server_->add_metrics_provider([]() { return std::string("provider_a 1\n"); });
    server_->add_metrics_provider([]() { return std::string("provider_b 2\n"); });

    const std::string body = http_get(test_port_, "/metrics");
    EXPECT_NE(body.find("provider_a 1"), std::string::npos);
    EXPECT_NE(body.find("provider_b 2"), std::string::npos);
}

TEST_F(MetricsProviderTest, KafkaStatsProviderIntegration) {
    // Simulate a KafkaConsumer and register it with the server.
    zeptodb::feeds::KafkaConfig cfg;
    cfg.topic = "market-data";
    zeptodb::feeds::KafkaConsumer consumer(cfg);

    // Ingest one tick via on_message() so all stats (messages_consumed,
    // route_local) are populated from a realistic code path.
    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipe(pc);
    consumer.set_pipeline(&pipe);
    // JSON format: symbol_id=1, price=15000, volume=100
    const char* json = R"({"symbol_id":1,"price":15000,"volume":100})";
    consumer.on_message(json, strlen(json));

    server_->add_metrics_provider([&consumer]() {
        return zeptodb::feeds::KafkaConsumer::format_prometheus(
            "market-data", consumer.stats());
    });

    const std::string body = http_get(test_port_, "/metrics");
    EXPECT_NE(body.find("zepto_kafka_messages_consumed_total{consumer=\"market-data\"} 1"),
              std::string::npos);
    EXPECT_NE(body.find("zepto_kafka_route_local_total{consumer=\"market-data\"} 1"),
              std::string::npos);
    EXPECT_NE(body.find("zepto_kafka_ingest_failures_total{consumer=\"market-data\"} 0"),
              std::string::npos);
}

// ============================================================================
// Part 5: /whoami endpoint — role detection
// ============================================================================

TEST_F(ConnectionHooksTest, WhoamiReturnsAdminWhenNoAuth) {
    // No auth configured → /whoami returns admin role
    auto body = http_get(test_port_, "/whoami");
    EXPECT_NE(body.find("\"role\":\"admin\""), std::string::npos);
    EXPECT_NE(body.find("\"subject\":\"anonymous\""), std::string::npos);
}

// ============================================================================
// Part 6: Access logging — request ID and structured log
// ============================================================================

TEST_F(ConnectionHooksTest, ResponseContainsRequestId) {
    // Make a raw HTTP request and check X-Request-Id in response headers
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(test_port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);

    std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ::send(fd, req.c_str(), req.size(), 0);

    std::string raw;
    char buf[4096];
    struct timeval tv{ 2, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    // Response should contain X-Request-Id header
    EXPECT_NE(raw.find("X-Request-Id: r"), std::string::npos)
        << "Response missing X-Request-Id header. Raw:\n" << raw.substr(0, 500);
}
