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
#include "zeptodb/auth/auth_manager.h"

// --- Feeds ---
#include "zeptodb/feeds/kafka_consumer.h"

#include "test_port_helper.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_set>
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

struct HttpResponse {
    int status = 0;
    std::string body;
};

static HttpResponse http_request(int port,
                                 const std::string& method,
                                 const std::string& path,
                                 const std::string& body = {},
                                 const std::string& auth = {}) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return {};
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n";
    if (!auth.empty()) {
        req += "Authorization: Bearer " + auth + "\r\n";
    }
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    ::send(fd, req.c_str(), req.size(), 0);

    std::string raw;
    char buf[4096];
    ssize_t n;
    struct timeval tv{ 1, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    HttpResponse out;
    const auto first_space = raw.find(' ');
    if (first_space != std::string::npos && first_space + 4 <= raw.size()) {
        out.status = std::atoi(raw.c_str() + first_space + 1);
    }
    const auto body_pos = raw.find("\r\n\r\n");
    out.body = body_pos != std::string::npos ? raw.substr(body_pos + 4) : raw;
    return out;
}

static bool wait_until(const std::function<bool()>& predicate) {
    for (int i = 0; i < 200; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
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
        test_port_ = zepto_test_util::pick_free_port();
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

    uint16_t test_port_ = 0;

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
        test_port_ = zepto_test_util::pick_free_port();
        pipeline_ = make_pipeline();
        executor_  = std::make_unique<QueryExecutor>(*pipeline_);
        server_    = std::make_unique<zeptodb::server::HttpServer>(*executor_, test_port_);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }

    void TearDown() override {
        server_->stop();
    }

    uint16_t test_port_ = 0;

    std::unique_ptr<ZeptoPipeline>             pipeline_;
    std::unique_ptr<QueryExecutor>            executor_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
};

TEST_F(MetricsProviderTest, DefaultMetricsContainApexCounters) {
    const std::string body = http_get(test_port_, "/metrics");
    EXPECT_NE(body.find("zepto_ticks_ingested_total"), std::string::npos);
    EXPECT_NE(body.find("zepto_server_ready"),          std::string::npos);
}

TEST_F(MetricsProviderTest, PrometheusMetricsExposesIngestRate) {
    // P8-I4 (devlog 117): /metrics must expose `zepto_ingest_ticks_per_sec`
    // as a gauge so prometheus-adapter can map it onto the HPA Pods metric.
    const std::string body = http_get(test_port_, "/metrics");

    EXPECT_NE(body.find("# HELP zepto_ingest_ticks_per_sec"), std::string::npos)
        << "missing HELP line for zepto_ingest_ticks_per_sec\n" << body;
    EXPECT_NE(body.find("# TYPE zepto_ingest_ticks_per_sec gauge"), std::string::npos)
        << "missing TYPE line (must be gauge)\n" << body;

    // Value line: "zepto_ingest_ticks_per_sec 0.00" or some non-negative
    // floating-point number. We require the prefix + a digit so that test
    // failure points at a missing or malformed value rather than just text.
    auto val_pos = body.find("\nzepto_ingest_ticks_per_sec ");
    ASSERT_NE(val_pos, std::string::npos) << "missing value line\n" << body;
    char first = body[val_pos + std::string("\nzepto_ingest_ticks_per_sec ").size()];
    EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(first)))
        << "value should start with a digit, got '" << first << "'";
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

TEST_F(MetricsProviderTest, EdgeFleetConnectorAdminLifecycleAndMetrics) {
    const std::string initial = http_get(test_port_, "/admin/edge-fleet-connector");
    EXPECT_NE(initial.find("\"configured\":false"), std::string::npos);
    EXPECT_NE(initial.find("\"enabled\":false"), std::string::npos);
    EXPECT_NE(initial.find("\"worker_running\":false"), std::string::npos);

    const auto post = http_request(
        test_port_,
        "POST",
        "/admin/edge-fleet-connector",
        R"({"name":"test-edge-fleet","enabled":true,"batch_limit":9,"max_inflight":4,"max_retries_per_event":2})");
    EXPECT_EQ(post.status, 200);
    EXPECT_NE(post.body.find("\"configured\":true"), std::string::npos);
    EXPECT_NE(post.body.find("\"enabled\":true"), std::string::npos);
    EXPECT_NE(post.body.find("\"batch_limit\":9"), std::string::npos);
    EXPECT_NE(post.body.find("\"max_inflight\":4"), std::string::npos);
    EXPECT_NE(post.body.find("\"worker_enabled\":false"), std::string::npos);

    const std::string metrics = http_get(test_port_, "/metrics");
    EXPECT_NE(metrics.find("zepto_edge_fleet_connector_enabled{connector=\"test-edge-fleet\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("zepto_edge_fleet_connector_start_total{connector=\"test-edge-fleet\"} 1"),
              std::string::npos);

    const auto del = http_request(test_port_, "DELETE", "/admin/edge-fleet-connector");
    EXPECT_EQ(del.status, 200);
    EXPECT_NE(del.body.find("\"configured\":false"), std::string::npos);
    EXPECT_NE(del.body.find("\"enabled\":false"), std::string::npos);
}

TEST_F(MetricsProviderTest, EdgeFleetConnectorAdminStartsWorkerWhenHooksInstalled) {
    std::mutex delivered_mu;
    std::vector<std::string> delivered;

    zeptodb::feeds::EdgeFleetConnectorRuntimeHooks hooks;
    hooks.load_outbox = [] {
        zeptodb::feeds::EdgeFleetOutboxLoadResult out;
        out.ok = true;
        zeptodb::feeds::EdgeFleetFeedEvent first;
        first.event_id = "http-e1";
        first.stream_seq = 1;
        first.kind = zeptodb::feeds::EdgeFleetEventKind::Decision;
        first.ready_ts_ns = 1810000000000000001LL;
        zeptodb::feeds::EdgeFleetFeedEvent second = first;
        second.event_id = "http-e2";
        second.stream_seq = 2;
        second.ready_ts_ns = 1810000000000000002LL;
        out.events = {first, second};
        return out;
    };
    hooks.sink = [&](const zeptodb::feeds::EdgeFleetFeedEvent& item) {
        std::lock_guard<std::mutex> lock(delivered_mu);
        delivered.push_back(item.event_id);
        return zeptodb::feeds::EdgeFleetDeliveryResult::Acked;
    };
    std::string hook_error;
    ASSERT_TRUE(server_->set_edge_fleet_connector_runtime_hooks(
        std::move(hooks), &hook_error)) << hook_error;

    const auto post = http_request(
        test_port_,
        "POST",
        "/admin/edge-fleet-connector",
        R"({"name":"test-edge-worker","enabled":true,"worker_enabled":true,"worker_poll_interval_ms":1,"batch_limit":1,"max_inflight":1})");
    EXPECT_EQ(post.status, 200);
    EXPECT_NE(post.body.find("\"worker_enabled\":true"), std::string::npos);

    ASSERT_TRUE(wait_until([&] {
        const std::string status = http_get(test_port_, "/admin/edge-fleet-connector");
        return status.find("\"acked_count\":2") != std::string::npos;
    }));

    const std::string status = http_get(test_port_, "/admin/edge-fleet-connector");
    EXPECT_NE(status.find("\"worker_hooks_configured\":true"), std::string::npos);
    EXPECT_NE(status.find("\"worker_passes_total\":"), std::string::npos);

    const std::string metrics = http_get(test_port_, "/metrics");
    EXPECT_NE(metrics.find("zepto_edge_fleet_connector_worker_passes_total{connector=\"test-edge-worker\"}"),
              std::string::npos);

    const auto del = http_request(test_port_, "DELETE", "/admin/edge-fleet-connector");
    EXPECT_EQ(del.status, 200);
    EXPECT_NE(del.body.find("\"worker_running\":false"), std::string::npos);

    std::lock_guard<std::mutex> lock(delivered_mu);
    EXPECT_GE(delivered.size(), 2u);
}

TEST_F(MetricsProviderTest, EdgeFleetConnectorRejectsInvalidLimits) {
    const auto post = http_request(
        test_port_,
        "POST",
        "/admin/edge-fleet-connector",
        R"({"name":"bad-edge-fleet","batch_limit":0})");
    EXPECT_EQ(post.status, 400);
    EXPECT_NE(post.body.find("positive"), std::string::npos);
}

TEST_F(MetricsProviderTest, EdgeFleetConnectorRejectsWorkerModeWithoutHooks) {
    const auto post = http_request(
        test_port_,
        "POST",
        "/admin/edge-fleet-connector",
        R"({"name":"no-hooks","enabled":true,"worker_enabled":true,"worker_poll_interval_ms":1})");
    EXPECT_EQ(post.status, 400);
    EXPECT_NE(post.body.find("requires outbox loader"), std::string::npos);
}

TEST_F(MetricsProviderTest, ActionOutcomeSupervisorAdminLifecycleAndMetrics) {
    const std::string initial = http_get(test_port_, "/admin/action-outcome-supervisor");
    EXPECT_NE(initial.find("\"configured\":false"), std::string::npos);
    EXPECT_NE(initial.find("\"enabled\":false"), std::string::npos);
    EXPECT_NE(initial.find("\"worker_running\":false"), std::string::npos);

    const auto post = http_request(
        test_port_,
        "POST",
        "/admin/action-outcome-supervisor",
        R"({"name":"test-action-outcome","enabled":true,"mode":"shadow","batch_limit":9,"worker_enabled":false})");
    EXPECT_EQ(post.status, 200);
    EXPECT_NE(post.body.find("\"configured\":true"), std::string::npos);
    EXPECT_NE(post.body.find("\"enabled\":true"), std::string::npos);
    EXPECT_NE(post.body.find("\"mode\":\"shadow\""), std::string::npos);
    EXPECT_NE(post.body.find("\"batch_limit\":9"), std::string::npos);

    const std::string metrics = http_get(test_port_, "/metrics");
    EXPECT_NE(metrics.find("zepto_action_outcome_supervisor_enabled{supervisor=\"test-action-outcome\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("zepto_action_outcome_proposals_processed_total{supervisor=\"test-action-outcome\"} 0"),
              std::string::npos);

    const auto del = http_request(test_port_, "DELETE", "/admin/action-outcome-supervisor");
    EXPECT_EQ(del.status, 200);
    EXPECT_NE(del.body.find("\"configured\":false"), std::string::npos);
    EXPECT_NE(del.body.find("\"enabled\":false"), std::string::npos);
}

TEST_F(MetricsProviderTest, ActionOutcomeSupervisorAdminStartsWorkerWhenHooksInstalled) {
    std::mutex decided_mu;
    std::unordered_set<std::string> decided;

    zeptodb::feeds::ActionOutcomeSupervisorRuntimeHooks hooks;
    hooks.load_proposals = [] {
        zeptodb::feeds::ActionOutcomeProposalLoadResult out;
        out.ok = true;
        zeptodb::feeds::ActionOutcomeProposal first;
        first.proposal_id = "p1";
        first.source_type = "ros2_bag";
        first.proposed_action = "continue_route";
        first.source_ts_ns = 1;
        zeptodb::feeds::ActionOutcomeProposal second = first;
        second.proposal_id = "p2";
        second.source_ts_ns = 2;
        out.proposals = {first, second};
        return out;
    };
    hooks.already_decided = [&](const std::string& id) {
        std::lock_guard<std::mutex> lock(decided_mu);
        return decided.find(id) != decided.end();
    };
    hooks.decide = [](const zeptodb::feeds::ActionOutcomeProposal& item) {
        zeptodb::feeds::ActionOutcomeDecisionResult out;
        out.ok = true;
        out.decision.proposal_id = item.proposal_id;
        out.decision.decision = "allow";
        out.decision.final_action = item.proposed_action;
        out.decision.reason = "positive_action_outcome_pressure";
        out.decision.evidence_count = 3;
        return out;
    };
    hooks.sink_decision = [&](const zeptodb::feeds::ActionOutcomeDecision& decision,
                              std::string*) {
        std::lock_guard<std::mutex> lock(decided_mu);
        decided.insert(decision.proposal_id);
        return true;
    };

    std::string hook_error;
    ASSERT_TRUE(server_->set_action_outcome_supervisor_runtime_hooks(
        std::move(hooks), &hook_error)) << hook_error;

    const auto post = http_request(
        test_port_,
        "POST",
        "/admin/action-outcome-supervisor",
        R"({"name":"test-action-worker","enabled":true,"worker_enabled":true,"worker_poll_interval_ms":1,"batch_limit":2})");
    EXPECT_EQ(post.status, 200);
    EXPECT_NE(post.body.find("\"worker_hooks_configured\":true"), std::string::npos);

    ASSERT_TRUE(wait_until([&] {
        const std::string status = http_get(test_port_, "/admin/action-outcome-supervisor");
        return status.find("\"proposals_processed_total\":2") != std::string::npos;
    }));

    const std::string status = http_get(test_port_, "/admin/action-outcome-supervisor");
    EXPECT_NE(status.find("\"worker_passes_total\":"), std::string::npos);
    EXPECT_NE(status.find("\"evidence_rows_written_total\":6"), std::string::npos);

    const std::string metrics = http_get(test_port_, "/metrics");
    EXPECT_NE(metrics.find("zepto_action_outcome_supervisor_worker_passes_total{supervisor=\"test-action-worker\"}"),
              std::string::npos);
    EXPECT_NE(metrics.find("zepto_action_outcome_evidence_rows_written_total{supervisor=\"test-action-worker\"} 6"),
              std::string::npos);

    const auto del = http_request(test_port_, "DELETE", "/admin/action-outcome-supervisor");
    EXPECT_EQ(del.status, 200);
    EXPECT_NE(del.body.find("\"worker_running\":false"), std::string::npos);
}

TEST_F(MetricsProviderTest, ActionOutcomeSupervisorRejectsInvalidModeAndMissingHooks) {
    const auto bad_mode = http_request(
        test_port_,
        "POST",
        "/admin/action-outcome-supervisor",
        R"({"name":"bad-mode","mode":"advisory"})");
    EXPECT_EQ(bad_mode.status, 400);
    EXPECT_NE(bad_mode.body.find("shadow"), std::string::npos);

    const auto no_hooks = http_request(
        test_port_,
        "POST",
        "/admin/action-outcome-supervisor",
        R"({"name":"no-hooks","enabled":true,"worker_enabled":true,"worker_poll_interval_ms":1})");
    EXPECT_EQ(no_hooks.status, 400);
    EXPECT_NE(no_hooks.body.find("requires proposal loader"), std::string::npos);
}

TEST(EdgeFleetConnectorAdminAuthTest, RequiresAdminPermission) {
    const auto key_path =
        zepto_test_util::unique_test_path("edge_fleet_connector_keys");
    {
        std::ofstream file(key_path);
    }

    zeptodb::auth::AuthManager::Config auth_cfg;
    auth_cfg.enabled = true;
    auth_cfg.api_keys_file = key_path.string();
    auth_cfg.jwt_enabled = false;
    auth_cfg.rate_limit_enabled = false;
    auth_cfg.audit_enabled = false;
    auth_cfg.audit_buffer_enabled = false;
    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);
    const std::string admin_key =
        auth->create_api_key("edge-fleet-admin", zeptodb::auth::Role::ADMIN);
    const std::string writer_key =
        auth->create_api_key("edge-fleet-writer", zeptodb::auth::Role::WRITER);

    const uint16_t port = zepto_test_util::pick_free_port();
    auto pipeline = make_pipeline();
    auto executor = std::make_unique<QueryExecutor>(*pipeline);
    zeptodb::server::HttpServer server(*executor, port,
                                       zeptodb::auth::TlsConfig{}, auth);
    server.start_async();
    std::this_thread::sleep_for(60ms);

    const auto missing_auth =
        http_request(port, "GET", "/admin/edge-fleet-connector");
    EXPECT_EQ(missing_auth.status, 401);

    const auto writer =
        http_request(port, "GET", "/admin/edge-fleet-connector", {}, writer_key);
    EXPECT_EQ(writer.status, 403);

    const auto admin =
        http_request(port, "GET", "/admin/edge-fleet-connector", {}, admin_key);
    EXPECT_EQ(admin.status, 200);
    EXPECT_NE(admin.body.find("\"configured\":false"), std::string::npos);

    const auto action_missing_auth =
        http_request(port, "GET", "/admin/action-outcome-supervisor");
    EXPECT_EQ(action_missing_auth.status, 401);

    const auto action_writer =
        http_request(port, "GET", "/admin/action-outcome-supervisor", {}, writer_key);
    EXPECT_EQ(action_writer.status, 403);

    const auto action_admin =
        http_request(port, "GET", "/admin/action-outcome-supervisor", {}, admin_key);
    EXPECT_EQ(action_admin.status, 200);
    EXPECT_NE(action_admin.body.find("\"configured\":false"), std::string::npos);

    server.stop();
    std::filesystem::remove(key_path);
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

// ============================================================================
// Table-level ACL tests
// ============================================================================

#include "zeptodb/auth/auth_manager.h"

TEST(TableACL, CanAccessTable_EmptyAllowAll) {
    zeptodb::auth::AuthContext ctx;
    ctx.role = zeptodb::auth::Role::READER;
    // empty allowed_tables = unrestricted
    EXPECT_TRUE(ctx.can_access_table("trades"));
    EXPECT_TRUE(ctx.can_access_table("quotes"));
}

TEST(TableACL, CanAccessTable_Restricted) {
    zeptodb::auth::AuthContext ctx;
    ctx.role = zeptodb::auth::Role::ANALYST;
    ctx.allowed_tables = {"trades", "orders"};

    EXPECT_TRUE(ctx.can_access_table("trades"));
    EXPECT_TRUE(ctx.can_access_table("orders"));
    EXPECT_FALSE(ctx.can_access_table("risk_positions"));
    EXPECT_FALSE(ctx.can_access_table("quotes"));
}

TEST(TableACL, ApiKeyStore_CreateWithTables) {
    // Use a temp file
    std::string path = zepto_test_util::unique_test_path("keys_table_acl").string();
    std::remove(path.c_str());

    zeptodb::auth::ApiKeyStore store(path);
    std::string key = store.create_key("desk-1", zeptodb::auth::Role::READER,
                                        {}, {"trades", "quotes"});
    ASSERT_FALSE(key.empty());

    auto entry = store.validate(key);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->allowed_tables.size(), 2u);
    EXPECT_EQ(entry->allowed_tables[0], "trades");
    EXPECT_EQ(entry->allowed_tables[1], "quotes");

    // Reload from disk and verify persistence
    zeptodb::auth::ApiKeyStore store2(path);
    auto entries = store2.list();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].allowed_tables.size(), 2u);
    EXPECT_EQ(entries[0].allowed_tables[0], "trades");
    EXPECT_EQ(entries[0].allowed_tables[1], "quotes");

    std::remove(path.c_str());
}

TEST(TableACL, ApiKeyStore_BackwardCompatible_NoTables) {
    std::string path = zepto_test_util::unique_test_path("keys_compat").string();
    std::remove(path.c_str());

    // Create key without tables
    zeptodb::auth::ApiKeyStore store(path);
    std::string key = store.create_key("legacy", zeptodb::auth::Role::WRITER);

    auto entry = store.validate(key);
    ASSERT_TRUE(entry.has_value());
    EXPECT_TRUE(entry->allowed_tables.empty());

    std::remove(path.c_str());
}
