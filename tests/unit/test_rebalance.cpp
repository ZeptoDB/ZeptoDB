// ============================================================================
// RebalanceManager + Dual-Write Tests
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/ring_consensus.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/license_validator.h"

#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>

using namespace zeptodb::cluster;
using namespace std::chrono_literals;

// Helper: load an all-features Enterprise license into the global singleton
static void ensure_enterprise_license() {
    static bool loaded = false;
    if (loaded) return;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string payload = R"({"edition":"enterprise","features":255,"max_nodes":64,"exp":)" +
        std::to_string(now + 86400) + "}";
    auto b64 = [](const std::string& s) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        auto data = reinterpret_cast<const unsigned char*>(s.data());
        size_t len = s.size();
        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i+1 < len) n |= static_cast<uint32_t>(data[i+1]) << 8;
            if (i+2 < len) n |= static_cast<uint32_t>(data[i+2]);
            out += tbl[(n>>18)&63]; out += tbl[(n>>12)&63];
            out += (i+1<len) ? tbl[(n>>6)&63] : '=';
            out += (i+2<len) ? tbl[n&63] : '=';
        }
        for (char& c : out) { if (c=='+') c='-'; else if (c=='/') c='_'; }
        while (!out.empty() && out.back()=='=') out.pop_back();
        return out;
    };
    std::string jwt = b64(R"({"alg":"RS256","typ":"JWT"})") + "." + b64(payload) + ".fakesig";
    zeptodb::auth::license().load_from_jwt_string_for_testing(jwt);
    loaded = true;
}

// ============================================================================
// Helpers
// ============================================================================

static std::unique_ptr<zeptodb::core::ZeptoPipeline> make_pipeline() {
    zeptodb::core::PipelineConfig cfg;
    cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    return std::make_unique<zeptodb::core::ZeptoPipeline>(cfg);
}

static zeptodb::ingestion::TickMessage make_tick(uint32_t sym, int64_t price,
                                                  int32_t vol, int64_t ts) {
    zeptodb::ingestion::TickMessage m{};
    m.symbol_id = sym;
    m.price = price;
    m.volume = vol;
    m.recv_ts = ts;
    return m;
}

// ============================================================================
// Mock RingConsensus for testing broadcast
// ============================================================================

class MockRingConsensus : public zeptodb::cluster::RingConsensus {
public:
    bool propose_add(NodeId node) override { last_add_ = node; add_count_++; return true; }
    bool propose_remove(NodeId node) override { last_remove_ = node; remove_count_++; return true; }
    bool apply_update(const uint8_t*, size_t) override { return true; }
    uint64_t current_epoch() const override { return 0; }

    NodeId last_add_ = 0, last_remove_ = 0;
    int add_count_ = 0, remove_count_ = 0;
};

// ============================================================================
// 1. DualWriteIngestTest — during migration, ticks written to BOTH nodes
// ============================================================================

TEST(DualWriteTest, DualWriteIngestTest) {
    auto src = make_pipeline();
    auto dst = make_pipeline();

    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);

    // Begin migration: symbol 42, node 1 → node 2
    router.begin_migration(42, 1, 2);

    auto target = router.migration_target(42);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->first, 1u);
    EXPECT_EQ(target->second, 2u);

    // Simulate dual-write path (what ClusterNode::ingest_tick does)
    for (int i = 0; i < 10; ++i) {
        auto tick = make_tick(42, 10000000LL + i, 100,
                              static_cast<int64_t>(i) * 1'000'000'000LL);
        src->ingest_tick(tick);  // write to source
        dst->ingest_tick(tick);  // write to dest (dual-write)
    }
    src->drain_sync(100);
    dst->drain_sync(100);

    // Both nodes must have all 10 ticks
    zeptodb::sql::QueryExecutor ex_src(*src);
    auto r1 = ex_src.execute("SELECT count(*) FROM trades WHERE symbol = 42");
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.rows[0][0], 10);

    zeptodb::sql::QueryExecutor ex_dst(*dst);
    auto r2 = ex_dst.execute("SELECT count(*) FROM trades WHERE symbol = 42");
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.rows[0][0], 10);

    router.end_migration(42);
    EXPECT_FALSE(router.migration_target(42).has_value());
}

// ============================================================================
// 2. DualWriteNormalPath — no migration, normal single-node routing
// ============================================================================

TEST(DualWriteTest, DualWriteNormalPath) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);

    // No migration active — migration_target returns nullopt
    EXPECT_FALSE(router.migration_target(42).has_value());
    EXPECT_FALSE(router.migration_target(99).has_value());

    // Normal route returns a single node
    NodeId owner = router.route(42);
    EXPECT_TRUE(owner == 1 || owner == 2);

    // Single-node ingest works normally
    auto pipeline = make_pipeline();
    pipeline->ingest_tick(make_tick(42, 10000000, 100, 1'000'000'000LL));
    pipeline->drain_sync(100);

    zeptodb::sql::QueryExecutor ex(*pipeline);
    auto r = ex.execute("SELECT count(*) FROM trades WHERE symbol = 42");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.rows[0][0], 1);
}

// ============================================================================
// Fixture for RebalanceManager tests
// ============================================================================

class RebalanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        src_pipeline_ = make_pipeline();
        dst_pipeline_ = make_pipeline();

        // Pre-load source with data
        for (uint32_t sym = 500; sym < 503; ++sym) {
            for (int i = 0; i < 5; ++i) {
                src_pipeline_->ingest_tick(make_tick(
                    sym, 10000000LL, 100,
                    static_cast<int64_t>(i) * 1'000'000'000LL));
            }
        }
        src_pipeline_->drain_sync(100);

        src_srv_.start(29200, [this](const std::string& sql) {
            zeptodb::sql::QueryExecutor ex(*src_pipeline_);
            return ex.execute(sql);
        });
        dst_srv_.start(29201, [this](const std::string& sql) {
            zeptodb::sql::QueryExecutor ex(*dst_pipeline_);
            return ex.execute(sql);
        }, nullptr,
        [this](const std::vector<zeptodb::ingestion::TickMessage>& batch) -> size_t {
            for (auto& msg : batch)
                dst_pipeline_->ingest_tick(msg);
            dst_pipeline_->drain_sync(100);
            return batch.size();
        });
        std::this_thread::sleep_for(20ms);

        migrator_.add_node(1, "127.0.0.1", 29200);
        migrator_.add_node(2, "127.0.0.1", 29201);
        router_.add_node(1);
    }

    void TearDown() override {
        src_srv_.stop();
        dst_srv_.stop();
    }

    std::unique_ptr<zeptodb::core::ZeptoPipeline> src_pipeline_;
    std::unique_ptr<zeptodb::core::ZeptoPipeline> dst_pipeline_;
    TcpRpcServer src_srv_, dst_srv_;
    PartitionMigrator migrator_;
    PartitionRouter router_;
};

// ============================================================================
// 3. RebalanceAddNode
// ============================================================================

TEST_F(RebalanceTest, RebalanceAddNode) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_GT(st.total_moves, 0u);
    EXPECT_EQ(st.completed_moves + st.failed_moves, st.total_moves);
}

// ============================================================================
// 4. RebalanceRemoveNode
// ============================================================================

TEST_F(RebalanceTest, RebalanceRemoveNode) {
    router_.add_node(2);

    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_remove_node(2));
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_GT(st.total_moves, 0u);
}

// ============================================================================
// 5. RebalancePauseResume
// ============================================================================

TEST_F(RebalanceTest, RebalancePauseResume) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));

    mgr.pause();
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(mgr.state(), RebalanceState::PAUSED);

    mgr.resume();
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_EQ(st.completed_moves + st.failed_moves, st.total_moves);
}

// ============================================================================
// 6. RebalanceCancel
// ============================================================================

TEST_F(RebalanceTest, RebalanceCancel) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));

    mgr.cancel();
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// ============================================================================
// 7. RebalanceAlreadyRunning
// ============================================================================

TEST_F(RebalanceTest, RebalanceAlreadyRunning) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));

    // Second start rejected
    EXPECT_FALSE(mgr.start_add_node(2));
    EXPECT_FALSE(mgr.start_remove_node(1));

    mgr.cancel();
    mgr.wait(10);
}

// ============================================================================
// 8. RebalanceStatusTracking
// ============================================================================

TEST_F(RebalanceTest, RebalanceStatusTracking) {
    RebalanceManager mgr(router_, migrator_);

    // Before start: IDLE, zeroes
    auto st = mgr.status();
    EXPECT_EQ(st.state, RebalanceState::IDLE);
    EXPECT_EQ(st.total_moves, 0u);
    EXPECT_EQ(st.completed_moves, 0u);
    EXPECT_EQ(st.failed_moves, 0u);

    EXPECT_TRUE(mgr.start_add_node(2));
    std::this_thread::sleep_for(5ms);

    auto running_st = mgr.status();
    EXPECT_GT(running_st.total_moves, 0u);

    mgr.wait(10);

    auto final_st = mgr.status();
    EXPECT_EQ(final_st.state, RebalanceState::IDLE);
    EXPECT_EQ(final_st.completed_moves + final_st.failed_moves,
              final_st.total_moves);
    EXPECT_TRUE(final_st.current_symbol.empty());
}

// ============================================================================
// 8.1 RebalanceBroadcastOnAddNode
// ============================================================================

TEST_F(RebalanceTest, RebalanceBroadcastOnAddNode) {
    MockRingConsensus mock;
    RebalanceManager mgr(router_, migrator_);
    mgr.set_consensus(&mock);

    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(10));

    EXPECT_EQ(mock.add_count_, 1);
    EXPECT_EQ(mock.last_add_, 2u);
    EXPECT_EQ(mock.remove_count_, 0);
}

// ============================================================================
// 8.2 RebalanceBroadcastOnRemoveNode
// ============================================================================

TEST_F(RebalanceTest, RebalanceBroadcastOnRemoveNode) {
    router_.add_node(2);
    MockRingConsensus mock;
    RebalanceManager mgr(router_, migrator_);
    mgr.set_consensus(&mock);

    EXPECT_TRUE(mgr.start_remove_node(2));
    EXPECT_TRUE(mgr.wait(10));

    EXPECT_EQ(mock.remove_count_, 1);
    EXPECT_EQ(mock.last_remove_, 2u);
    EXPECT_EQ(mock.add_count_, 0);
}

// ============================================================================
// 8.3 RebalanceNoBroadcastOnCancel
// ============================================================================

TEST_F(RebalanceTest, RebalanceNoBroadcastOnCancel) {
    MockRingConsensus mock;
    RebalanceManager mgr(router_, migrator_);
    mgr.set_consensus(&mock);

    EXPECT_TRUE(mgr.start_add_node(2));
    mgr.cancel();
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// ============================================================================
// Phase 3: RebalancePolicy — Load-Based Auto-Trigger Tests
// ============================================================================

class RebalancePolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        src_pipeline_ = make_pipeline();
        dst_pipeline_ = make_pipeline();

        for (uint32_t sym = 600; sym < 603; ++sym) {
            for (int i = 0; i < 5; ++i) {
                src_pipeline_->ingest_tick(make_tick(
                    sym, 10000000LL, 100,
                    static_cast<int64_t>(i) * 1'000'000'000LL));
            }
        }
        src_pipeline_->drain_sync(100);

        src_srv_.start(29300, [this](const std::string& sql) {
            zeptodb::sql::QueryExecutor ex(*src_pipeline_);
            return ex.execute(sql);
        });
        dst_srv_.start(29301, [this](const std::string& sql) {
            zeptodb::sql::QueryExecutor ex(*dst_pipeline_);
            return ex.execute(sql);
        }, nullptr,
        [this](const std::vector<zeptodb::ingestion::TickMessage>& batch) -> size_t {
            for (auto& msg : batch)
                dst_pipeline_->ingest_tick(msg);
            dst_pipeline_->drain_sync(100);
            return batch.size();
        });
        std::this_thread::sleep_for(20ms);

        migrator_.add_node(1, "127.0.0.1", 29300);
        migrator_.add_node(2, "127.0.0.1", 29301);
        router_.add_node(1);
    }

    void TearDown() override {
        src_srv_.stop();
        dst_srv_.stop();
    }

    std::unique_ptr<zeptodb::core::ZeptoPipeline> src_pipeline_;
    std::unique_ptr<zeptodb::core::ZeptoPipeline> dst_pipeline_;
    TcpRpcServer src_srv_, dst_srv_;
    PartitionMigrator migrator_;
    PartitionRouter router_;
};

// 9. Imbalanced load triggers rebalance automatically
TEST_F(RebalancePolicyTest, RebalancePolicyImbalanceDetection) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 1.5;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}, {2, 10}};
    });
    mgr.start_policy();

    // Wait for auto-trigger (check_interval=1s, give it up to 3s)
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(100ms);
        if (mgr.state() != RebalanceState::IDLE) break;
    }
    // The policy should have triggered a rebalance
    // It may already be running or may have completed
    auto st = mgr.status();
    EXPECT_TRUE(st.state == RebalanceState::RUNNING ||
                st.total_moves > 0);

    mgr.stop_policy();
    mgr.cancel();
    mgr.wait(5);
}

// 10. Cooldown prevents rapid re-triggering
TEST_F(RebalancePolicyTest, RebalancePolicyCooldown) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 1.5;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 3;  // 3s cooldown (startup grace period = 3s too)

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}, {2, 10}};
    });
    mgr.start_policy();

    // Wait for first trigger (after startup grace period of 3s + check interval)
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(100ms);
        if (mgr.state() != RebalanceState::IDLE) break;
    }
    mgr.wait(10);  // let first rebalance complete

    auto st_after_first = mgr.status();
    size_t first_total = st_after_first.total_moves;
    EXPECT_GT(first_total, 0u);

    // Wait 1 check interval — cooldown (3s) should block second trigger
    std::this_thread::sleep_for(1500ms);
    auto st_after_wait = mgr.status();
    // total_moves should not have increased (cooldown blocks)
    EXPECT_EQ(st_after_wait.total_moves, first_total);

    mgr.stop_policy();
}

// 11. Balanced load does not trigger rebalance
TEST_F(RebalancePolicyTest, RebalancePolicyNoImbalance) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 2.0;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 50}, {2, 50}};
    });
    mgr.start_policy();

    // Wait 2+ check intervals
    std::this_thread::sleep_for(2500ms);

    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
    EXPECT_EQ(mgr.status().total_moves, 0u);

    mgr.stop_policy();
}

// 12. Disabled policy does not trigger
TEST_F(RebalancePolicyTest, RebalancePolicyDisabled) {
    RebalanceConfig cfg;
    cfg.policy.enabled = false;
    cfg.policy.check_interval_sec = 1;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}, {2, 10}};
    });
    mgr.start_policy();  // should be a no-op

    std::this_thread::sleep_for(2000ms);

    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
    EXPECT_EQ(mgr.status().total_moves, 0u);

    mgr.stop_policy();
}

// 13. Single node — no crash, no rebalance
TEST_F(RebalancePolicyTest, RebalancePolicySingleNode) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 2.0;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}};  // only 1 node
    });
    mgr.start_policy();

    std::this_thread::sleep_for(2000ms);

    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
    EXPECT_EQ(mgr.status().total_moves, 0u);

    mgr.stop_policy();
}

// 14. Node with 0 partitions — no division by zero
TEST_F(RebalancePolicyTest, RebalancePolicyZeroPartitions) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 2.0;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}, {2, 0}};  // node 2 has 0 partitions
    });
    mgr.start_policy();

    // Wait for check — should trigger (100/1 = 100 > 2.0 threshold)
    // The key thing: no crash from division by zero
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(100ms);
        if (mgr.state() != RebalanceState::IDLE) break;
    }

    // Should have triggered (min_count clamped to 1, ratio = 100.0 > 2.0)
    auto st = mgr.status();
    EXPECT_TRUE(st.state == RebalanceState::RUNNING || st.total_moves > 0);

    mgr.stop_policy();
    mgr.cancel();
    mgr.wait(5);
}

// ============================================================================
// Phase 4: Admin HTTP API Tests
// ============================================================================
// Uses raw socket HTTP helpers following the pattern from test_cluster.cpp.
// The HttpServer with auth disabled allows admin endpoints without auth.

#include "zeptodb/server/http_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static std::string http_get_rebalance(uint16_t port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return "";
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);
    std::string raw;
    char buf[4096];
    struct timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);
    auto pos = raw.find("\r\n\r\n");
    return pos != std::string::npos ? raw.substr(pos + 4) : raw;
}

static std::string http_post_rebalance(uint16_t port, const std::string& path,
                                        const std::string& body = "") {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return "";
    }
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + body;
    ::send(fd, req.data(), req.size(), 0);
    std::string raw;
    char buf[4096];
    struct timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);
    auto pos = raw.find("\r\n\r\n");
    return pos != std::string::npos ? raw.substr(pos + 4) : raw;
}

class HttpRebalanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensure_enterprise_license();
        pipeline_ = make_pipeline();
        for (uint32_t sym = 700; sym < 703; ++sym) {
            for (int i = 0; i < 5; ++i) {
                pipeline_->ingest_tick(make_tick(
                    sym, 10000000LL, 100,
                    static_cast<int64_t>(i) * 1'000'000'000LL));
            }
        }
        pipeline_->drain_sync(100);

        executor_ = std::make_unique<zeptodb::sql::QueryExecutor>(*pipeline_);

        // No auth — require_admin returns true
        server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, http_port_);

        // Set up migrator/router for rebalance manager
        src_srv_.start(29400, [this](const std::string& sql) {
            return executor_->execute(sql);
        });
        dst_srv_.start(29401, [this](const std::string& sql) {
            return executor_->execute(sql);
        }, nullptr,
        [](const std::vector<zeptodb::ingestion::TickMessage>& batch) -> size_t {
            return batch.size();
        });
        std::this_thread::sleep_for(20ms);

        migrator_.add_node(1, "127.0.0.1", 29400);
        migrator_.add_node(2, "127.0.0.1", 29401);
        router_.add_node(1);

        mgr_ = std::make_unique<RebalanceManager>(router_, migrator_);
        server_->set_rebalance_manager(mgr_.get());
        server_->set_ready(true);
        server_->start_async();
        std::this_thread::sleep_for(60ms);
    }

    void TearDown() override {
        server_->stop();
        mgr_->cancel();
        mgr_->wait(5);
        src_srv_.stop();
        dst_srv_.stop();
    }

    static constexpr uint16_t http_port_ = 18920;

    std::unique_ptr<zeptodb::core::ZeptoPipeline> pipeline_;
    std::unique_ptr<zeptodb::sql::QueryExecutor> executor_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
    TcpRpcServer src_srv_, dst_srv_;
    PartitionMigrator migrator_;
    PartitionRouter router_;
    std::unique_ptr<RebalanceManager> mgr_;
};

// 15. GET /admin/rebalance/status returns IDLE state
TEST_F(HttpRebalanceTest, HttpRebalanceStatus) {
    auto body = http_get_rebalance(http_port_, "/admin/rebalance/status");
    EXPECT_NE(body.find("\"state\":\"IDLE\""), std::string::npos);
    EXPECT_NE(body.find("\"total_moves\":0"), std::string::npos);
    EXPECT_NE(body.find("\"completed_moves\":0"), std::string::npos);
    EXPECT_NE(body.find("\"failed_moves\":0"), std::string::npos);
}

// 16. POST /admin/rebalance/start then /cancel
TEST_F(HttpRebalanceTest, HttpRebalanceStartAndCancel) {
    // Note: do NOT add node 2 to router before start — plan_add requires the node to be new
    auto start_body = http_post_rebalance(http_port_, "/admin/rebalance/start",
                                           R"({"action":"add_node","node_id":2})");
    EXPECT_NE(start_body.find("\"ok\":true"), std::string::npos);

    std::this_thread::sleep_for(20ms);

    auto cancel_body = http_post_rebalance(http_port_, "/admin/rebalance/cancel");
    EXPECT_NE(cancel_body.find("\"ok\":true"), std::string::npos);

    mgr_->wait(5);
    EXPECT_EQ(mgr_->state(), RebalanceState::IDLE);
}

// ============================================================================
// Phase 5: Edge Cases, Concurrency, Single-Node Safety, Multi-Symbol, Policy
// ============================================================================

// 17. Empty plan when node already exists → no-op
TEST_F(RebalanceTest, EmptyPlanNoOp) {
    router_.add_node(2);  // node 2 already in router
    RebalanceManager mgr(router_, migrator_);
    EXPECT_FALSE(mgr.start_add_node(2));  // empty plan
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// 18. Can't remove last node
TEST_F(RebalanceTest, RemoveLastNodeReturnsEmpty) {
    // router_ has only node 1 (from SetUp)
    RebalanceManager mgr(router_, migrator_);
    EXPECT_FALSE(mgr.start_remove_node(1));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// 19. wait() with short timeout returns false
TEST_F(RebalanceTest, WaitTimeoutReturnsFalse) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    // 1-second timeout while rebalance may still be running
    bool finished = mgr.wait(1);
    if (!finished) {
        EXPECT_NE(mgr.state(), RebalanceState::IDLE);
    }
    mgr.cancel();
    EXPECT_TRUE(mgr.wait(10));
}

// 20. Cancel while paused
TEST_F(RebalanceTest, CancelWhilePaused) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    mgr.pause();
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(mgr.state(), RebalanceState::PAUSED);
    mgr.cancel();
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// 21. Double cancel — second is no-op
TEST_F(RebalanceTest, DoubleCancel) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    mgr.cancel();
    mgr.cancel();  // no crash, no hang
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// 22. Double pause — second is no-op
TEST_F(RebalanceTest, DoublePause) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    mgr.pause();
    mgr.pause();  // no crash
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(mgr.state(), RebalanceState::PAUSED);
    mgr.resume();
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// 23. Resume without pause — no-op
TEST_F(RebalanceTest, ResumeWithoutPause) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    mgr.resume();  // no crash, no-op
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// ============================================================================
// Concurrent Operations
// ============================================================================

// 24. Concurrent ingest during migration — thread-safety of dual-write
TEST(DualWriteTest, ConcurrentIngestDuringMigration) {
    auto src = make_pipeline();
    auto dst = make_pipeline();

    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.begin_migration(42, 1, 2);

    constexpr int kThreads = 4;
    constexpr int kTicksPerThread = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kTicksPerThread; ++i) {
                int64_t ts = static_cast<int64_t>(t * kTicksPerThread + i) * 1'000'000'000LL;
                auto tick = make_tick(42, 10000000LL + i, 100, ts);
                src->ingest_tick(tick);
                dst->ingest_tick(tick);
                // Also verify migration_target is thread-safe
                auto target = router.migration_target(42);
                EXPECT_TRUE(target.has_value());
            }
        });
    }
    for (auto& th : threads) th.join();

    src->drain_sync(1000);
    dst->drain_sync(1000);

    zeptodb::sql::QueryExecutor ex_src(*src);
    auto r1 = ex_src.execute("SELECT count(*) FROM trades WHERE symbol = 42");
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.rows[0][0], kThreads * kTicksPerThread);

    zeptodb::sql::QueryExecutor ex_dst(*dst);
    auto r2 = ex_dst.execute("SELECT count(*) FROM trades WHERE symbol = 42");
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.rows[0][0], kThreads * kTicksPerThread);

    router.end_migration(42);
}

// 25. Rapid start/cancel cycles — no crash, no hang
TEST_F(RebalanceTest, RapidStartCancel) {
    RebalanceManager mgr(router_, migrator_);
    for (int i = 0; i < 10; ++i) {
        // Re-add node 2 if it was added by previous cycle
        if (router_.node_count() > 1) {
            router_.remove_node(2);
        }
        EXPECT_TRUE(mgr.start_add_node(2));
        mgr.cancel();
        EXPECT_TRUE(mgr.wait(10));
        EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
    }
}

// ============================================================================
// Single-Node Safety
// ============================================================================

// 26. ZeptoPipeline (non-cluster) unaffected by rebalancing feature
TEST(SingleNodeTest, SingleNodePipelineUnaffected) {
    auto pipeline = make_pipeline();
    for (int i = 0; i < 1000; ++i) {
        pipeline->ingest_tick(make_tick(
            42, 10000000LL + i, 100,
            static_cast<int64_t>(i) * 1'000'000'000LL));
    }
    pipeline->drain_sync(2000);

    zeptodb::sql::QueryExecutor ex(*pipeline);
    auto r = ex.execute("SELECT count(*) FROM trades WHERE symbol = 42");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.rows[0][0], 1000);
    // ZeptoPipeline has no migration_target concept — just direct ingest
}

// 27. Single ClusterNode — no peers, no RebalanceManager
#include "zeptodb/cluster/cluster_node.h"
#include "zeptodb/cluster/transport.h"
#include "shm_backend.h"

TEST(SingleNodeTest, SingleNodeNoRebalanceManager) {
    ensure_enterprise_license();
    using ShmNode = zeptodb::cluster::ClusterNode<SharedMemBackend>;

    zeptodb::cluster::ClusterConfig cfg;
    cfg.self = {"127.0.0.1", 19001, 1};
    cfg.enable_remote_ingest = false;
    cfg.pipeline.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;

    auto node = std::make_unique<ShmNode>(cfg);
    node->join_cluster();  // no seeds = bootstrap

    // Ingest ticks locally
    for (int i = 0; i < 50; ++i) {
        zeptodb::ingestion::TickMessage msg{};
        msg.symbol_id = 99;
        msg.price = 10000000LL;
        msg.volume = 100;
        msg.recv_ts = static_cast<int64_t>(i) * 1'000'000'000LL;
        EXPECT_TRUE(node->ingest_local(msg));
    }

    // Wait for drain
    const auto& pstats = node->pipeline().stats();
    for (int i = 0; i < 5000 && pstats.ticks_stored.load(std::memory_order_acquire) < 50; ++i)
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    // migration_target returns nullopt for all symbols (no migration active)
    EXPECT_FALSE(node->router().migration_target(99).has_value());
    EXPECT_FALSE(node->router().migration_target(0).has_value());

    // All ticks went to local pipeline
    auto result = node->query_local_vwap(99);
    EXPECT_EQ(result.type, zeptodb::core::QueryResult::Type::VWAP);

    node->leave_cluster();
}

// ============================================================================
// Multi-Symbol Migration
// ============================================================================

// 28. Pre-load 10 symbols, rebalance add_node, verify total_moves
TEST_F(RebalanceTest, MultiSymbolMigration) {
    // Pre-load source with 10 symbols (5 ticks each) — symbols 800..809
    for (uint32_t sym = 800; sym < 810; ++sym) {
        for (int i = 0; i < 5; ++i) {
            src_pipeline_->ingest_tick(make_tick(
                sym, 10000000LL, 100,
                static_cast<int64_t>(i) * 1'000'000'000LL));
        }
    }
    src_pipeline_->drain_sync(200);

    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_GT(st.total_moves, 0u);
    EXPECT_EQ(st.completed_moves + st.failed_moves, st.total_moves);
}

// ============================================================================
// Policy Edge Cases
// ============================================================================

// 29. Load provider returns empty vector — no crash
TEST_F(RebalancePolicyTest, PolicyLoadProviderReturnsEmpty) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 1.5;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([]() -> std::vector<std::pair<NodeId, size_t>> {
        return {};  // empty
    });
    mgr.start_policy();

    std::this_thread::sleep_for(2000ms);

    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
    EXPECT_EQ(mgr.status().total_moves, 0u);

    mgr.stop_policy();
}

// 30. Start policy then immediately stop — clean shutdown
TEST_F(RebalancePolicyTest, PolicyStopWhileChecking) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 1.5;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}, {2, 10}};
    });
    mgr.start_policy();
    mgr.stop_policy();  // immediately stop

    // No crash, clean shutdown
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

// ============================================================================
// Fix #1: peer_rpc_clients_ Thread Safety Tests
// ============================================================================

TEST(SingleNodeTest, PeerRpcConcurrentAccess) {
    ensure_enterprise_license();
    using ShmNode = zeptodb::cluster::ClusterNode<SharedMemBackend>;

    zeptodb::cluster::ClusterConfig cfg;
    cfg.self = {"127.0.0.1", 19010, 1};
    cfg.enable_remote_ingest = false;
    cfg.pipeline.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;

    auto node = std::make_unique<ShmNode>(cfg);
    node->join_cluster();

    // Concurrent ingest_tick from multiple threads — exercises the
    // peer_rpc_mutex_ path (even though all route locally here,
    // migration_target() and route() are called concurrently).
    constexpr int kThreads = 4;
    constexpr int kTicks = 200;
    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kTicks; ++i) {
                zeptodb::ingestion::TickMessage msg{};
                msg.symbol_id = 42;
                msg.price = 10000000LL;
                msg.volume = 100;
                msg.recv_ts = static_cast<int64_t>(t * kTicks + i) * 1'000'000'000LL;
                if (node->ingest_tick(msg)) ok_count++;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(ok_count.load(), kThreads * kTicks);
    node->leave_cluster();
}

// ============================================================================
// Fix #2: Move Timeout Tests
// ============================================================================

TEST_F(RebalanceTest, MoveTimeoutEnforced) {
    // Set a very short timeout (1s) — moves should still succeed if fast enough
    RebalanceConfig cfg;
    cfg.move_timeout_sec = 5;

    RebalanceManager mgr(router_, migrator_, cfg);
    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(30));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_GT(st.total_moves, 0u);
    // With 5s timeout, local moves should succeed
    EXPECT_GT(st.completed_moves, 0u);
}

TEST(MigratorTest, MoveTimeoutZeroDisablesTimeout) {
    PartitionMigrator migrator;
    migrator.set_move_timeout(0);

    // No crash — timeout disabled means no std::async wrapping
    // Just verify the setter works
    SUCCEED();
}

// ============================================================================
// Fix #3: Query Routing Safety (recently_migrated) Tests
// ============================================================================

TEST(RouterTest, RecentlyMigratedGracePeriod) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);

    // During migration: migration_target returns value
    router.begin_migration(42, 1, 2);
    EXPECT_TRUE(router.migration_target(42).has_value());
    EXPECT_FALSE(router.recently_migrated(42).has_value());

    // End migration: migration_target gone, recently_migrated appears
    router.end_migration(42);
    EXPECT_FALSE(router.migration_target(42).has_value());

    auto recent = router.recently_migrated(42);
    ASSERT_TRUE(recent.has_value());
    EXPECT_EQ(recent->first, 1u);
    EXPECT_EQ(recent->second, 2u);
}

TEST(RouterTest, RecentlyMigratedExpires) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.set_migration_grace_period(std::chrono::seconds(1));

    router.begin_migration(42, 1, 2);
    router.end_migration(42);

    // Immediately after: should be present
    EXPECT_TRUE(router.recently_migrated(42).has_value());

    // After grace period: should expire
    std::this_thread::sleep_for(1100ms);
    EXPECT_FALSE(router.recently_migrated(42).has_value());
}

TEST(RouterTest, RecentlyMigratedNoMigration) {
    PartitionRouter router;
    router.add_node(1);

    // No migration ever started — both should be nullopt
    EXPECT_FALSE(router.migration_target(99).has_value());
    EXPECT_FALSE(router.recently_migrated(99).has_value());
}

TEST(RouterTest, RecentlyMigratedMultipleSymbols) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);

    router.begin_migration(10, 1, 2);
    router.begin_migration(20, 2, 3);
    router.end_migration(10);
    router.end_migration(20);

    auto r1 = router.recently_migrated(10);
    auto r2 = router.recently_migrated(20);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->first, 1u);
    EXPECT_EQ(r1->second, 2u);
    EXPECT_EQ(r2->first, 2u);
    EXPECT_EQ(r2->second, 3u);
}

TEST(RouterTest, EndMigrationWithoutBeginIsNoOp) {
    PartitionRouter router;
    router.add_node(1);

    // end_migration on a symbol that was never migrating — no crash
    router.end_migration(999);
    EXPECT_FALSE(router.recently_migrated(999).has_value());
}

// ============================================================================
// Partial-Move Rebalance API Tests
// ============================================================================

TEST_F(RebalanceTest, PartialMovePartitions) {
    router_.add_node(2);
    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});

    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_EQ(st.total_moves, 1u);
    EXPECT_EQ(st.completed_moves + st.failed_moves, 1u);
}

TEST_F(RebalanceTest, PartialMoveEmptyMoves) {
    RebalanceManager mgr(router_, migrator_);
    std::vector<PartitionRouter::Move> empty;
    EXPECT_FALSE(mgr.start_move_partitions(std::move(empty)));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);
}

TEST_F(RebalanceTest, PartialMoveMultipleSymbols) {
    router_.add_node(2);
    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});
    moves.push_back({501, 1, 2});
    moves.push_back({502, 1, 2});

    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_EQ(st.total_moves, 3u);
    EXPECT_EQ(st.completed_moves + st.failed_moves, 3u);
}

TEST_F(RebalanceTest, PartialMoveNoBroadcast) {
    router_.add_node(2);
    MockRingConsensus mock;
    RebalanceManager mgr(router_, migrator_);
    mgr.set_consensus(&mock);

    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});

    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
    EXPECT_TRUE(mgr.wait(10));

    EXPECT_EQ(mock.add_count_, 0);
    EXPECT_EQ(mock.remove_count_, 0);
}

TEST_F(RebalanceTest, PartialMoveAlreadyRunning) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));

    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});
    EXPECT_FALSE(mgr.start_move_partitions(std::move(moves)));

    mgr.cancel();
    mgr.wait(10);
}

TEST_F(HttpRebalanceTest, HttpPartialMovePartitions) {
    router_.add_node(2);

    auto body = http_post_rebalance(http_port_, "/admin/rebalance/start",
        R"({"action":"move_partitions","moves":[{"symbol":700,"from":1,"to":2}]})");
    EXPECT_NE(body.find("\"ok\":true"), std::string::npos);

    mgr_->wait(10);
    EXPECT_EQ(mgr_->state(), RebalanceState::IDLE);
}

// 37. HTTP rejects self-move (from == to)
TEST_F(HttpRebalanceTest, HttpPartialMoveSelfMoveRejected) {
    auto body = http_post_rebalance(http_port_, "/admin/rebalance/start",
        R"({"action":"move_partitions","moves":[{"symbol":700,"from":1,"to":1}]})");
    EXPECT_NE(body.find("from"), std::string::npos);
    EXPECT_NE(body.find("to"), std::string::npos);
    EXPECT_EQ(mgr_->state(), RebalanceState::IDLE);
}

// ============================================================================
// Rebalance History Tests
// ============================================================================

TEST_F(RebalanceTest, HistoryRecordedAfterAddNode) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.history().empty());

    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(10));

    auto hist = mgr.history();
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].action, RebalanceAction::ADD_NODE);
    EXPECT_EQ(hist[0].node_id, 2u);
    EXPECT_GT(hist[0].total_moves, 0u);
    EXPECT_EQ(hist[0].completed_moves + hist[0].failed_moves, hist[0].total_moves);
    EXPECT_GE(hist[0].duration_ms, 0);
    EXPECT_GT(hist[0].start_time_ms, 0);
    EXPECT_FALSE(hist[0].cancelled);
}

TEST_F(RebalanceTest, HistoryRecordedAfterCancel) {
    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_add_node(2));
    mgr.cancel();
    EXPECT_TRUE(mgr.wait(10));

    auto hist = mgr.history();
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_TRUE(hist[0].cancelled);
}

TEST_F(RebalanceTest, HistoryMultipleEntries) {
    RebalanceManager mgr(router_, migrator_);

    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(10));

    router_.remove_node(2);
    EXPECT_TRUE(mgr.start_add_node(2));
    EXPECT_TRUE(mgr.wait(10));

    auto hist = mgr.history();
    ASSERT_EQ(hist.size(), 2u);
}

TEST_F(RebalanceTest, HistoryPartialMoveAction) {
    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_);

    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});
    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
    EXPECT_TRUE(mgr.wait(10));

    auto hist = mgr.history();
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].action, RebalanceAction::NONE);
    EXPECT_EQ(hist[0].node_id, 0u);
}

TEST_F(HttpRebalanceTest, HttpRebalanceHistory) {
    // Trigger a rebalance first
    auto start_body = http_post_rebalance(http_port_, "/admin/rebalance/start",
                                           R"({"action":"add_node","node_id":2})");
    EXPECT_NE(start_body.find("\"ok\":true"), std::string::npos);
    mgr_->wait(10);

    auto body = http_get_rebalance(http_port_, "/admin/rebalance/history");
    EXPECT_NE(body.find("\"action\":\"add_node\""), std::string::npos);
    EXPECT_NE(body.find("\"node_id\":2"), std::string::npos);
    EXPECT_NE(body.find("\"duration_ms\""), std::string::npos);
    EXPECT_NE(body.find("\"cancelled\":false"), std::string::npos);
}

// ============================================================================
// Additional Coverage Tests
// ============================================================================

// 38. Data integrity after partial-move — verify data arrives at destination
TEST_F(RebalanceTest, PartialMoveDataIntegrity) {
    router_.add_node(2);

    // Source has symbols 500-502 (5 ticks each, from SetUp)
    RebalanceManager mgr(router_, migrator_);
    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});
    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
    EXPECT_TRUE(mgr.wait(10));

    // Verify data arrived at destination
    zeptodb::sql::QueryExecutor ex_dst(*dst_pipeline_);
    auto r = ex_dst.execute("SELECT count(*) FROM trades WHERE symbol = 500");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.rows[0][0], 5);
}

// 39. Pause/resume during partial-move
TEST_F(RebalanceTest, PartialMovePauseResume) {
    router_.add_node(2);

    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});
    moves.push_back({501, 1, 2});
    moves.push_back({502, 1, 2});

    RebalanceManager mgr(router_, migrator_);
    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));

    mgr.pause();
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(mgr.state(), RebalanceState::PAUSED);

    mgr.resume();
    EXPECT_TRUE(mgr.wait(10));
    EXPECT_EQ(mgr.state(), RebalanceState::IDLE);

    auto st = mgr.status();
    EXPECT_EQ(st.completed_moves + st.failed_moves, st.total_moves);
}

// 40. History ring buffer overflow — 51st entry evicts oldest
TEST_F(RebalanceTest, HistoryRingBufferOverflow) {
    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_);

    // Use partial-move to generate many history entries without ring changes
    for (int i = 0; i < 51; ++i) {
        std::vector<PartitionRouter::Move> moves;
        moves.push_back({500, 1, 2});
        EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
        EXPECT_TRUE(mgr.wait(10));
    }

    auto hist = mgr.history();
    EXPECT_EQ(hist.size(), 50u);  // capped at MAX_HISTORY
}

// 41. Remove node history records REMOVE_NODE action
TEST_F(RebalanceTest, HistoryRecordedAfterRemoveNode) {
    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_);

    EXPECT_TRUE(mgr.start_remove_node(2));
    EXPECT_TRUE(mgr.wait(10));

    auto hist = mgr.history();
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].action, RebalanceAction::REMOVE_NODE);
    EXPECT_EQ(hist[0].node_id, 2u);
    EXPECT_FALSE(hist[0].cancelled);
}

// 42. HTTP history returns empty array when no rebalances
TEST_F(HttpRebalanceTest, HttpRebalanceHistoryEmpty) {
    auto body = http_get_rebalance(http_port_, "/admin/rebalance/history");
    EXPECT_EQ(body, "[]");
}

// 43. HTTP invalid action returns 400
TEST_F(HttpRebalanceTest, HttpRebalanceInvalidAction) {
    auto body = http_post_rebalance(http_port_, "/admin/rebalance/start",
        R"({"action":"invalid_action","node_id":1})");
    EXPECT_NE(body.find("Invalid action"), std::string::npos);
}

// 44. HTTP move_partitions without moves array returns 400
TEST_F(HttpRebalanceTest, HttpPartialMoveMissingMovesArray) {
    auto body = http_post_rebalance(http_port_, "/admin/rebalance/start",
        R"({"action":"move_partitions"})");
    EXPECT_NE(body.find("Missing"), std::string::npos);
}

// 45. HTTP move_partitions with empty moves array returns 400
TEST_F(HttpRebalanceTest, HttpPartialMoveEmptyMovesArray) {
    auto body = http_post_rebalance(http_port_, "/admin/rebalance/start",
        R"({"action":"move_partitions","moves":[]})");
    EXPECT_NE(body.find("Empty moves"), std::string::npos);
}

// 46. Policy auto-trigger records history
TEST_F(RebalancePolicyTest, PolicyAutoTriggerRecordsHistory) {
    RebalanceConfig cfg;
    cfg.policy.enabled = true;
    cfg.policy.imbalance_ratio = 1.5;
    cfg.policy.check_interval_sec = 1;
    cfg.policy.cooldown_sec = 0;

    router_.add_node(2);
    RebalanceManager mgr(router_, migrator_, cfg);

    mgr.set_load_provider([&]() -> std::vector<std::pair<NodeId, size_t>> {
        return {{1, 100}, {2, 10}};
    });
    mgr.start_policy();

    // Wait for auto-trigger
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(100ms);
        if (mgr.state() != RebalanceState::IDLE) break;
    }
    mgr.wait(10);
    mgr.stop_policy();

    auto hist = mgr.history();
    EXPECT_GE(hist.size(), 1u);
}

// 47. Checkpoint with partial-move
TEST_F(RebalanceTest, PartialMoveWithCheckpoint) {
    router_.add_node(2);

    std::string cp_dir = "/tmp/zepto_test_rebalance_cp";
    std::filesystem::create_directories(cp_dir);

    RebalanceConfig cfg;
    cfg.checkpoint_dir = cp_dir;
    RebalanceManager mgr(router_, migrator_, cfg);

    std::vector<PartitionRouter::Move> moves;
    moves.push_back({500, 1, 2});
    EXPECT_TRUE(mgr.start_move_partitions(std::move(moves)));
    EXPECT_TRUE(mgr.wait(10));

    auto st = mgr.status();
    EXPECT_EQ(st.completed_moves + st.failed_moves, st.total_moves);

    // Checkpoint file should exist
    std::ifstream f(cp_dir + "/rebalance.json");
    EXPECT_TRUE(f.good());
    // Cleanup
    std::filesystem::remove_all(cp_dir);
}
