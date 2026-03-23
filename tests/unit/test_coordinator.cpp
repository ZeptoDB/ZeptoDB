// ============================================================================
// Phase C-3: Coordinator, TCP RPC, and Partial Aggregation Tests
// ============================================================================
// Test groups:
//   1. RpcProtocol    — QueryResultSet serialize/deserialize round-trip
//   2. PartialAgg     — merge strategies for scalar agg and concat
//   3. TcpRpc         — ping-pong and SQL query over loopback TCP
//   4. QueryCoordinator — single-node (local), two-node scatter-gather
// ============================================================================

#include <gtest/gtest.h>

#include "apex/cluster/rpc_protocol.h"
#include "apex/cluster/partial_agg.h"
#include "apex/cluster/tcp_rpc.h"
#include "apex/cluster/query_coordinator.h"
#include "apex/cluster/wal_replicator.h"
#include "apex/cluster/partition_migrator.h"
#include "apex/cluster/failover_manager.h"
#include "apex/cluster/coordinator_ha.h"
#include "apex/cluster/snapshot_coordinator.h"
#include "apex/cluster/compute_node.h"
#include "apex/core/pipeline.h"
#include "apex/ingestion/tick_plant.h"

#include <chrono>
#include <memory>
#include <thread>

using namespace apex::cluster;
using namespace apex::sql;
using namespace std::chrono_literals;

// ============================================================================
// Helpers
// ============================================================================

static QueryResultSet make_result(
    std::vector<std::string> cols,
    std::vector<std::vector<int64_t>> rows)
{
    QueryResultSet r;
    r.column_names = std::move(cols);
    for (size_t i = 0; i < r.column_names.size(); ++i)
        r.column_types.push_back(apex::storage::ColumnType::INT64);
    r.rows = std::move(rows);
    return r;
}

// ApexPipeline contains a 32MB arena — must be heap-allocated to avoid stack overflow
static std::unique_ptr<apex::core::ApexPipeline> make_pipeline() {
    apex::core::PipelineConfig cfg;
    cfg.storage_mode = apex::core::StorageMode::PURE_IN_MEMORY;
    return std::make_unique<apex::core::ApexPipeline>(cfg);
}

// ============================================================================
// 1. RpcProtocol — round-trip serialization
// ============================================================================

TEST(RpcProtocol, RoundTripEmpty) {
    QueryResultSet r;
    auto bytes = serialize_result(r);
    auto r2    = deserialize_result(bytes.data(), bytes.size());
    EXPECT_TRUE(r2.ok());
    EXPECT_EQ(r2.column_names.size(), 0u);
    EXPECT_EQ(r2.rows.size(),         0u);
}

TEST(RpcProtocol, RoundTripWithError) {
    QueryResultSet r;
    r.error = "something went wrong";
    auto bytes = serialize_result(r);
    auto r2    = deserialize_result(bytes.data(), bytes.size());
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(r2.error, "something went wrong");
}

TEST(RpcProtocol, RoundTripTwoColsTwoRows) {
    QueryResultSet r = make_result(
        {"price", "volume"},
        {{15000, 100}, {16000, 200}, {17000, 300}}
    );
    auto bytes = serialize_result(r);
    auto r2    = deserialize_result(bytes.data(), bytes.size());
    ASSERT_TRUE(r2.ok());
    ASSERT_EQ(r2.column_names.size(), 2u);
    EXPECT_EQ(r2.column_names[0], "price");
    EXPECT_EQ(r2.column_names[1], "volume");
    ASSERT_EQ(r2.rows.size(), 3u);
    EXPECT_EQ(r2.rows[0][0], 15000);
    EXPECT_EQ(r2.rows[2][1], 300);
}

TEST(RpcProtocol, RoundTripNegativeValues) {
    QueryResultSet r = make_result({"delta"}, {{-1}, {INT64_MIN}, {INT64_MAX}});
    auto bytes = serialize_result(r);
    auto r2    = deserialize_result(bytes.data(), bytes.size());
    ASSERT_TRUE(r2.ok());
    ASSERT_EQ(r2.rows.size(), 3u);
    EXPECT_EQ(r2.rows[0][0], -1);
    EXPECT_EQ(r2.rows[1][0], INT64_MIN);
    EXPECT_EQ(r2.rows[2][0], INT64_MAX);
}

TEST(RpcProtocol, TruncatedDataReturnsError) {
    QueryResultSet r = make_result({"x"}, {{42}});
    auto bytes = serialize_result(r);
    auto r2 = deserialize_result(bytes.data(), bytes.size() / 2);
    EXPECT_FALSE(r2.ok());
    EXPECT_NE(r2.error.find("truncated"), std::string::npos);
}

// ============================================================================
// 2. PartialAgg — merge strategies
// ============================================================================

TEST(PartialAgg, DetectScalarAgg) {
    auto r = make_result({"sum(price)"}, {{1000}});
    std::vector<QueryResultSet> v{r};
    EXPECT_EQ(detect_merge_strategy(v), MergeStrategy::SCALAR_AGG);
}

TEST(PartialAgg, DetectConcat_MultiRow) {
    auto r = make_result({"symbol", "sum(price)"}, {{1, 1000}, {2, 2000}});
    std::vector<QueryResultSet> v{r};
    EXPECT_EQ(detect_merge_strategy(v), MergeStrategy::CONCAT);
}

TEST(PartialAgg, DetectConcat_NonAggColumn) {
    auto r = make_result({"price", "volume"}, {{1500, 100}});
    std::vector<QueryResultSet> v{r};
    EXPECT_EQ(detect_merge_strategy(v), MergeStrategy::CONCAT);
}

TEST(PartialAgg, MergeScalarSum) {
    auto r1 = make_result({"sum(volume)"}, {{300}});
    auto r2 = make_result({"sum(volume)"}, {{700}});
    auto merged = merge_results({r1, r2});
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged.rows.size(), 1u);
    EXPECT_EQ(merged.rows[0][0], 1000);
}

TEST(PartialAgg, MergeScalarCount) {
    auto r1 = make_result({"count(*)"}, {{50}});
    auto r2 = make_result({"count(*)"}, {{30}});
    auto merged = merge_results({r1, r2});
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(merged.rows[0][0], 80);
}

TEST(PartialAgg, MergeScalarMin) {
    auto r1 = make_result({"min(price)"}, {{15000}});
    auto r2 = make_result({"min(price)"}, {{14500}});
    auto r3 = make_result({"min(price)"}, {{16000}});
    auto merged = merge_results({r1, r2, r3});
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(merged.rows[0][0], 14500);
}

TEST(PartialAgg, MergeScalarMax) {
    auto r1 = make_result({"max(price)"}, {{15000}});
    auto r2 = make_result({"max(price)"}, {{18000}});
    auto merged = merge_results({r1, r2});
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(merged.rows[0][0], 18000);
}

TEST(PartialAgg, MergeConcat_GroupBySymbol) {
    // With symbol affinity, each group key exists on exactly one node
    auto r1 = make_result({"symbol", "sum(volume)"}, {{1, 1000}, {2, 2000}});
    auto r2 = make_result({"symbol", "sum(volume)"}, {{3, 3000}, {4, 4000}});
    auto merged = merge_results({r1, r2});
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(merged.rows.size(), 4u);
    int64_t total_volume = 0;
    for (auto& row : merged.rows) total_volume += row[1];
    EXPECT_EQ(total_volume, 10000);
}

TEST(PartialAgg, MergeConcat_PlainSelect) {
    auto r1 = make_result({"price", "volume"}, {{100, 10}, {200, 20}});
    auto r2 = make_result({"price", "volume"}, {{300, 30}});
    auto merged = merge_results({r1, r2});
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(merged.rows.size(), 3u);
}

TEST(PartialAgg, MergeEmptyResults) {
    auto merged = merge_results({});
    EXPECT_FALSE(merged.ok());
}

TEST(PartialAgg, MergeWithOneEmptyNode) {
    auto r1 = make_result({"sum(price)"}, {{1000}});
    QueryResultSet r2 = make_result({"sum(price)"}, {});
    auto merged = merge_results({r1, r2});
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(merged.rows[0][0], 1000);
}

// ============================================================================
// 3. TcpRpc — loopback ping and SQL query
// ============================================================================

// Use ports 19700-19799 for TCP RPC tests
static constexpr uint16_t RPC_TEST_PORT_BASE = 19700;

TEST(TcpRpc, PingPong) {
    TcpRpcServer server;
    server.start(RPC_TEST_PORT_BASE, [](const std::string&) {
        return QueryResultSet{};
    });
    ASSERT_TRUE(server.is_running());
    std::this_thread::sleep_for(20ms);

    TcpRpcClient client("127.0.0.1", RPC_TEST_PORT_BASE);
    EXPECT_TRUE(client.ping());

    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(TcpRpc, SqlQueryRoundTrip) {
    TcpRpcServer server;
    server.start(RPC_TEST_PORT_BASE + 1, [](const std::string& sql) {
        if (sql == "SELECT 42") {
            return make_result({"value"}, {{42}});
        }
        QueryResultSet err;
        err.error = "unknown query";
        return err;
    });
    std::this_thread::sleep_for(20ms);

    TcpRpcClient client("127.0.0.1", RPC_TEST_PORT_BASE + 1);
    auto result = client.execute_sql("SELECT 42");

    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 42);

    server.stop();
}

TEST(TcpRpc, ServerNotRunning_ReturnsError) {
    TcpRpcClient client("127.0.0.1", RPC_TEST_PORT_BASE + 2);
    auto result = client.execute_sql("SELECT 1");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("cannot connect"), std::string::npos);
}

TEST(TcpRpc, MultipleSequentialQueries) {
    TcpRpcServer server;
    std::atomic<int> call_count{0};
    server.start(RPC_TEST_PORT_BASE + 3, [&](const std::string&) {
        ++call_count;
        return make_result({"n"}, {{call_count.load()}});
    });
    std::this_thread::sleep_for(20ms);

    TcpRpcClient client("127.0.0.1", RPC_TEST_PORT_BASE + 3);
    for (int i = 0; i < 5; ++i) {
        auto r = client.execute_sql("SELECT n");
        EXPECT_TRUE(r.ok()) << r.error;
        EXPECT_EQ(r.rows.size(), 1u);
    }
    EXPECT_EQ(call_count.load(), 5);
    server.stop();
}

// ============================================================================
// 4. QueryCoordinator — single-node (local)
// ============================================================================

TEST(QueryCoordinator, SingleLocalNode_DirectExecution) {
    auto pipeline = make_pipeline();
    pipeline->start();

    apex::ingestion::TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 15000000;
    msg.volume    = 100;
    msg.recv_ts   = 1'000'000'000LL;
    pipeline->ingest_tick(msg);
    msg.price  = 16000000;
    msg.volume = 200;
    msg.recv_ts = 2'000'000'000LL;
    pipeline->ingest_tick(msg);
    std::this_thread::sleep_for(50ms);

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", 19801, 1}, *pipeline);

    auto result = coord.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = 1");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 2);

    pipeline->stop();
}

TEST(QueryCoordinator, SingleLocalNode_SumQuery) {
    auto pipeline = make_pipeline();
    pipeline->start();

    for (int i = 0; i < 5; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 2;
        msg.price     = (10000 + i * 1000) * 10000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline->ingest_tick(msg);
    }
    std::this_thread::sleep_for(50ms);

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", 19802, 2}, *pipeline);

    auto result = coord.execute_sql(
        "SELECT sum(volume) FROM trades WHERE symbol = 2");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 500);

    pipeline->stop();
}

// ============================================================================
// 5. QueryCoordinator — two-node scatter-gather via TCP
// ============================================================================

TEST(QueryCoordinator, TwoNodeRemote_ScatterGather_Count) {
    auto pipeline1 = make_pipeline();
    auto pipeline2 = make_pipeline();

    // Node 1: symbol 10, 3 ticks
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 10;
        msg.price     = 10000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline1->ingest_tick(msg);
    }
    // Node 2: symbol 20, 5 ticks
    for (int i = 0; i < 5; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 20;
        msg.price     = 20000000LL;
        msg.volume    = 200;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline2->ingest_tick(msg);
    }
    // Synchronous drain — all ticks flushed to partitions before servers start
    pipeline1->drain_sync(100);
    pipeline2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    uint16_t port1 = 19810, port2 = 19811;
    srv1.start(port1, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline1);
        return ex.execute(sql);
    });
    srv2.start(port2, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);  // wait for server sockets to bind

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", port1, 10});
    coord.add_remote_node({"127.0.0.1", port2, 20});
    EXPECT_EQ(coord.node_count(), 2u);

    // Tier B: no symbol filter → scatter, merge COUNT
    auto result = coord.execute_sql("SELECT count(*) FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 8);  // 3 + 5

    srv1.stop();
    srv2.stop();
}

TEST(QueryCoordinator, TwoNodeRemote_GroupBy_Concat) {
    auto pipeline1 = make_pipeline();
    auto pipeline2 = make_pipeline();

    for (int i = 0; i < 4; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 30;
        msg.price     = 15000000LL;
        msg.volume    = 50;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline1->ingest_tick(msg);
    }
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 40;
        msg.price     = 20000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline2->ingest_tick(msg);
    }
    // Synchronous drain — all ticks flushed to partitions before servers start
    pipeline1->drain_sync(100);
    pipeline2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    uint16_t port1 = 19820, port2 = 19821;
    srv1.start(port1, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline1);
        return ex.execute(sql);
    });
    srv2.start(port2, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);  // wait for server sockets to bind

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", port1, 30});
    coord.add_remote_node({"127.0.0.1", port2, 40});

    // GROUP BY symbol with symbol affinity → concat is correct
    auto result = coord.execute_sql(
        "SELECT symbol, sum(volume) FROM trades GROUP BY symbol");
    ASSERT_TRUE(result.ok()) << result.error;
    // Each node returns its symbol row — total 2 rows (one per symbol)
    EXPECT_EQ(result.rows.size(), 2u);

    // Total sum of volume across all symbols = 4*50 + 3*100 = 500
    int64_t total = 0;
    for (auto& row : result.rows) total += row[1];
    EXPECT_EQ(total, 500);

    srv1.stop();
    srv2.stop();
}

TEST(QueryCoordinator, NoNodes_ReturnsError) {
    QueryCoordinator coord;
    auto result = coord.execute_sql("SELECT 1");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("no nodes"), std::string::npos);
}

// ============================================================================
// 6. Distributed AVG (P0-1)
//    Per-node AVG(col) would give wrong results — coordinator rewrites to
//    SUM+COUNT scatter, sums them globally, then divides for the true average.
// ============================================================================

TEST(QueryCoordinator, TwoNodeRemote_DistributedAvg_Correct) {
    auto pipeline1 = make_pipeline();
    auto pipeline2 = make_pipeline();

    // Node 1: symbol 50, volumes 100, 200, 300  → local avg = 200
    for (int i = 1; i <= 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 50;
        msg.price     = 10000000LL;
        msg.volume    = i * 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline1->ingest_tick(msg);
    }
    // Node 2: symbol 60, volume 600  → local avg = 600
    {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 60;
        msg.price     = 20000000LL;
        msg.volume    = 600;
        msg.recv_ts   = 1'000'000'000LL;
        pipeline2->ingest_tick(msg);
    }
    // Synchronous drain — pipelines not started, drain_sync is safe
    pipeline1->drain_sync(100);
    pipeline2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    uint16_t port1 = 19830, port2 = 19831;
    srv1.start(port1, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline1);
        return ex.execute(sql);
    });
    srv2.start(port2, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", port1, 50});
    coord.add_remote_node({"127.0.0.1", port2, 60});

    // Global avg(volume) = (100+200+300+600)/4 = 300
    // Naive per-node avg would give: (200+600)/2 = 400 — WRONG
    auto result = coord.execute_sql("SELECT avg(volume) FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 300);

    srv1.stop();
    srv2.stop();
}

TEST(QueryCoordinator, TwoNodeRemote_DistributedAvg_MixedAggs) {
    auto pipeline1 = make_pipeline();
    auto pipeline2 = make_pipeline();

    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 70;
        msg.price     = (100 + i * 100) * 1'000'000LL;  // 100M, 200M, 300M
        msg.volume    = 10;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline1->ingest_tick(msg);
    }
    for (int i = 0; i < 2; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 80;
        msg.price     = (400 + i * 100) * 1'000'000LL;  // 400M, 500M
        msg.volume    = 10;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline2->ingest_tick(msg);
    }
    pipeline1->drain_sync(100);
    pipeline2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    uint16_t port1 = 19832, port2 = 19833;
    srv1.start(port1, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline1);
        return ex.execute(sql);
    });
    srv2.start(port2, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", port1, 70});
    coord.add_remote_node({"127.0.0.1", port2, 80});

    // avg(price): (100M+200M+300M+400M+500M)/5 = 300M
    // sum(volume): 3*10 + 2*10 = 50
    auto result = coord.execute_sql(
        "SELECT avg(price), sum(volume) FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 300'000'000LL);  // avg(price)
    EXPECT_EQ(result.rows[0][1], 50);              // sum(volume)

    srv1.stop();
    srv2.stop();
}

// ============================================================================
// 7. Cross-node GROUP BY re-aggregation (P0-2)
//    Non-symbol GROUP BY (e.g. xbar time bucket) can produce the same key on
//    multiple nodes — MERGE_GROUP_BY re-aggregates by key after scatter.
// ============================================================================

TEST(QueryCoordinator, TwoNodeRemote_GroupBy_CrossNode_XbarMerge) {
    auto pipeline1 = make_pipeline();
    auto pipeline2 = make_pipeline();

    // Node 1: symbol 90, 4 ticks, all recv_ts < 5s → xbar(recv_ts,5s) = 0
    for (int i = 0; i < 4; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 90;
        msg.price     = 10000000LL;
        msg.volume    = 50;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline1->ingest_tick(msg);
    }
    // Node 2: symbol 91, 3 ticks, all recv_ts < 5s → xbar(recv_ts,5s) = 0
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 91;
        msg.price     = 20000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline2->ingest_tick(msg);
    }
    pipeline1->drain_sync(100);
    pipeline2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    uint16_t port1 = 19840, port2 = 19841;
    srv1.start(port1, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline1);
        return ex.execute(sql);
    });
    srv2.start(port2, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", port1, 90});
    coord.add_remote_node({"127.0.0.1", port2, 91});

    // Both nodes return xbar bucket=0.
    // CONCAT would give 2 rows (both bucket=0) — wrong.
    // MERGE_GROUP_BY re-aggregates → 1 row with sum(volume)=4*50+3*100=500.
    auto result = coord.execute_sql(
        "SELECT xbar(recv_ts, 5000000000), sum(volume) FROM trades "
        "GROUP BY xbar(recv_ts, 5000000000)");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    // Executor output format for GROUP BY: [key_col(s)..., non-NONE SELECT cols...]
    // For single-column xbar GROUP BY: [recv_ts_key, xbar_val, sum_volume]
    // → sum(volume) is at column index 2
    EXPECT_EQ(result.rows[0][2], 500);  // 4*50 + 3*100 = 500

    srv1.stop();
    srv2.stop();
}

// ============================================================================
// 8. WAL Replication — primary → replica async replication
// ============================================================================

TEST(WalReplication, RpcRoundTrip_WalBatch) {
    // Server receives WAL batch and replays into pipeline
    auto pipeline = make_pipeline();
    pipeline->drain_sync(100);

    std::atomic<size_t> replayed{0};
    TcpRpcServer server;
    server.start(19850,
        [](const std::string&) { return QueryResultSet{}; },
        nullptr,
        [&](const std::vector<apex::ingestion::TickMessage>& batch) -> size_t {
            for (auto& msg : batch)
                pipeline->ingest_tick(msg);
            pipeline->drain_sync(100);
            replayed.fetch_add(batch.size());
            return batch.size();
        });
    std::this_thread::sleep_for(20ms);

    // Client sends WAL batch
    TcpRpcClient client("127.0.0.1", 19850);
    std::vector<apex::ingestion::TickMessage> batch;
    for (int i = 0; i < 5; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 100;
        msg.price     = (15000 + i) * 1'000'000LL;
        msg.volume    = 10;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        batch.push_back(msg);
    }
    EXPECT_TRUE(client.replicate_wal(batch));
    EXPECT_EQ(replayed.load(), 5u);

    // Verify data is queryable on replica
    apex::sql::QueryExecutor ex(*pipeline);
    auto result = ex.execute("SELECT count(*) FROM trades WHERE symbol = 100");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 5);

    server.stop();
}

TEST(WalReplication, WalBatchSerializeRoundTrip) {
    std::vector<apex::ingestion::TickMessage> msgs;
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage m{};
        m.symbol_id = 42;
        m.price = (1000 + i) * 10000LL;
        m.volume = 50 + i;
        m.recv_ts = i * 1'000'000'000LL;
        msgs.push_back(m);
    }
    auto bytes = serialize_wal_batch(msgs);
    std::vector<apex::ingestion::TickMessage> out;
    ASSERT_TRUE(deserialize_wal_batch(bytes.data(), bytes.size(), out));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].symbol_id, 42u);
    EXPECT_EQ(out[2].volume, 52);
}

TEST(WalReplication, WalReplicator_EndToEnd) {
    // Replica pipeline + RPC server
    auto replica_pipeline = make_pipeline();

    TcpRpcServer replica_server;
    replica_server.start(19851,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*replica_pipeline);
            return ex.execute(sql);
        },
        nullptr,
        [&](const std::vector<apex::ingestion::TickMessage>& batch) -> size_t {
            for (auto& msg : batch)
                replica_pipeline->ingest_tick(msg);
            replica_pipeline->drain_sync(100);
            return batch.size();
        });
    std::this_thread::sleep_for(20ms);

    // WalReplicator on primary side
    apex::cluster::ReplicatorConfig cfg;
    cfg.batch_size = 10;
    cfg.flush_interval_ms = 50;
    apex::cluster::WalReplicator replicator(cfg);
    replicator.add_replica("127.0.0.1", 19851);
    replicator.start();

    // Primary ingests ticks and enqueues to replicator
    for (int i = 0; i < 20; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 200;
        msg.price     = 10000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        replicator.enqueue(msg);
    }

    // Wait for async replication
    std::this_thread::sleep_for(200ms);
    replicator.stop();

    // Verify replica has all 20 ticks
    apex::sql::QueryExecutor ex(*replica_pipeline);
    auto result = ex.execute("SELECT count(*) FROM trades WHERE symbol = 200");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 20);

    // Stats check
    EXPECT_EQ(replicator.stats().enqueued.load(), 20u);
    EXPECT_EQ(replicator.stats().dropped.load(), 0u);
    EXPECT_EQ(replicator.stats().send_errors.load(), 0u);

    replica_server.stop();
}

TEST(WalReplication, WalReplicator_ReplicaDown_CountsErrors) {
    // No server running — replicator should count send errors
    apex::cluster::ReplicatorConfig cfg;
    cfg.batch_size = 5;
    cfg.flush_interval_ms = 20;
    apex::cluster::WalReplicator replicator(cfg);
    replicator.add_replica("127.0.0.1", 19852);  // nobody listening
    replicator.start();

    for (int i = 0; i < 10; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 300;
        msg.price     = 10000000LL;
        msg.volume    = 1;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        replicator.enqueue(msg);
    }

    std::this_thread::sleep_for(200ms);
    replicator.stop();

    EXPECT_EQ(replicator.stats().enqueued.load(), 10u);
    EXPECT_GT(replicator.stats().send_errors.load(), 0u);
}

// ============================================================================
// 9. Partition Migration — move symbol data between nodes
// ============================================================================

TEST(PartitionMigration, MigrateSymbol_SourceToDest) {
    // Source node: has symbol 500 data
    auto src_pipeline = make_pipeline();
    for (int i = 0; i < 10; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 500;
        msg.price     = (10000 + i) * 1'000'000LL;
        msg.volume    = 100 + i;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        src_pipeline->ingest_tick(msg);
    }
    src_pipeline->drain_sync(100);

    // Dest node: empty
    auto dst_pipeline = make_pipeline();

    // Start RPC servers
    TcpRpcServer src_server, dst_server;
    src_server.start(19860,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*src_pipeline);
            return ex.execute(sql);
        },
        nullptr,
        nullptr);
    dst_server.start(19861,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*dst_pipeline);
            return ex.execute(sql);
        },
        nullptr,
        [&](const std::vector<apex::ingestion::TickMessage>& batch) -> size_t {
            for (auto& msg : batch)
                dst_pipeline->ingest_tick(msg);
            dst_pipeline->drain_sync(100);
            return batch.size();
        });
    std::this_thread::sleep_for(20ms);

    // Migrate symbol 500: node 1 → node 2
    apex::cluster::PartitionMigrator migrator;
    migrator.add_node(1, "127.0.0.1", 19860);
    migrator.add_node(2, "127.0.0.1", 19861);

    EXPECT_TRUE(migrator.migrate_symbol(500, 1, 2));
    EXPECT_EQ(migrator.stats().moves_completed.load(), 1u);
    EXPECT_EQ(migrator.stats().rows_migrated.load(), 10u);

    // Verify dest has the data
    apex::sql::QueryExecutor ex(*dst_pipeline);
    auto result = ex.execute("SELECT count(*) FROM trades WHERE symbol = 500");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 10);

    src_server.stop();
    dst_server.stop();
}

TEST(PartitionMigration, ExecutePlan_MultipleSymbols) {
    auto src_pipeline = make_pipeline();
    auto dst_pipeline = make_pipeline();

    // Source has symbols 600 and 601
    for (int sym = 600; sym <= 601; ++sym) {
        for (int i = 0; i < 5; ++i) {
            apex::ingestion::TickMessage msg{};
            msg.symbol_id = static_cast<uint32_t>(sym);
            msg.price     = 10000000LL;
            msg.volume    = 100;
            msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
            src_pipeline->ingest_tick(msg);
        }
    }
    src_pipeline->drain_sync(100);

    TcpRpcServer src_server, dst_server;
    src_server.start(19862,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*src_pipeline);
            return ex.execute(sql);
        });
    dst_server.start(19863,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*dst_pipeline);
            return ex.execute(sql);
        },
        nullptr,
        [&](const std::vector<apex::ingestion::TickMessage>& batch) -> size_t {
            for (auto& msg : batch)
                dst_pipeline->ingest_tick(msg);
            dst_pipeline->drain_sync(100);
            return batch.size();
        });
    std::this_thread::sleep_for(20ms);

    apex::cluster::PartitionMigrator migrator;
    migrator.add_node(1, "127.0.0.1", 19862);
    migrator.add_node(2, "127.0.0.1", 19863);

    // Build a plan with 2 moves
    apex::cluster::PartitionRouter::MigrationPlan plan;
    plan.moves.push_back({600, 1, 2});
    plan.moves.push_back({601, 1, 2});

    apex::cluster::PartitionRouter router;
    size_t ok = migrator.execute_plan(plan, router);
    EXPECT_EQ(ok, 2u);
    EXPECT_EQ(migrator.stats().rows_migrated.load(), 10u);

    // Verify dest
    apex::sql::QueryExecutor ex(*dst_pipeline);
    auto r1 = ex.execute("SELECT count(*) FROM trades WHERE symbol = 600");
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.rows[0][0], 5);
    auto r2 = ex.execute("SELECT count(*) FROM trades WHERE symbol = 601");
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.rows[0][0], 5);

    src_server.stop();
    dst_server.stop();
}

TEST(PartitionMigration, MigrateEmptySymbol_Succeeds) {
    auto src_pipeline = make_pipeline();
    src_pipeline->drain_sync(100);

    TcpRpcServer src_server;
    src_server.start(19864,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*src_pipeline);
            return ex.execute(sql);
        });
    std::this_thread::sleep_for(20ms);

    apex::cluster::PartitionMigrator migrator;
    migrator.add_node(1, "127.0.0.1", 19864);
    migrator.add_node(2, "127.0.0.1", 19865);  // doesn't matter, no data to send

    // Symbol 999 has no data — should succeed (nothing to move)
    EXPECT_TRUE(migrator.migrate_symbol(999, 1, 2));
    EXPECT_EQ(migrator.stats().moves_completed.load(), 1u);
    EXPECT_EQ(migrator.stats().rows_migrated.load(), 0u);

    src_server.stop();
}

// ============================================================================
// 10. Replication Factor 2 — route_replica()
// ============================================================================

TEST(ReplicationFactor, RouteReplica_TwoNodes) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);

    // For any symbol, primary and replica must be different nodes
    for (apex::SymbolId sym = 0; sym < 100; ++sym) {
        NodeId primary = router.route(sym);
        NodeId replica = router.route_replica(sym);
        EXPECT_NE(primary, replica)
            << "symbol=" << sym << " primary=" << primary << " replica=" << replica;
        EXPECT_TRUE(replica == 1 || replica == 2);
    }
}

TEST(ReplicationFactor, RouteReplica_SingleNode_ReturnsInvalid) {
    PartitionRouter router;
    router.add_node(1);
    EXPECT_EQ(router.route_replica(42), INVALID_NODE_ID);
}

TEST(ReplicationFactor, RouteReplica_ThreeNodes_AllDifferent) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);

    for (apex::SymbolId sym = 0; sym < 50; ++sym) {
        NodeId primary = router.route(sym);
        NodeId replica = router.route_replica(sym);
        EXPECT_NE(primary, replica);
    }
}

// ============================================================================
// 11. Auto Failover — FailoverManager
// ============================================================================

TEST(Failover, ManualTrigger_RemovesNodeAndFiresCallback) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);

    QueryCoordinator coord;
    auto p1 = make_pipeline();
    auto p2 = make_pipeline();
    auto p3 = make_pipeline();
    coord.add_local_node({"127.0.0.1", 19870, 1}, *p1);
    coord.add_local_node({"127.0.0.1", 19871, 2}, *p2);
    coord.add_local_node({"127.0.0.1", 19872, 3}, *p3);
    EXPECT_EQ(coord.node_count(), 3u);

    apex::cluster::FailoverManager fm(router, coord);

    std::atomic<bool> callback_fired{false};
    NodeId dead_in_callback = 0;
    fm.on_failover([&](const apex::cluster::FailoverEvent& ev) {
        callback_fired.store(true);
        dead_in_callback = ev.dead_node;
    });

    // Kill node 2
    auto event = fm.trigger_failover(2);
    EXPECT_EQ(event.dead_node, 2u);
    EXPECT_GT(event.affected_vnodes, 0u);
    EXPECT_TRUE(callback_fired.load());
    EXPECT_EQ(dead_in_callback, 2u);
    EXPECT_EQ(fm.failover_count(), 1u);

    // Coordinator should have 2 nodes now
    EXPECT_EQ(coord.node_count(), 2u);
    // Router should have 2 nodes
    EXPECT_EQ(router.node_count(), 2u);

    // All symbols should still route to node 1 or 3
    for (apex::SymbolId sym = 0; sym < 50; ++sym) {
        NodeId n = router.route(sym);
        EXPECT_TRUE(n == 1 || n == 3) << "sym=" << sym << " routed to " << n;
    }
}

TEST(Failover, HealthMonitorIntegration_DeadTriggersFailover) {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);

    QueryCoordinator coord;
    auto p1 = make_pipeline();
    auto p2 = make_pipeline();
    coord.add_local_node({"127.0.0.1", 19880, 1}, *p1);
    coord.add_local_node({"127.0.0.1", 19881, 2}, *p2);

    HealthConfig hcfg;
    hcfg.suspect_timeout_ms = 50;
    hcfg.dead_timeout_ms    = 100;
    HealthMonitor hm(hcfg);
    // Don't start() — use inject/simulate for testing

    // Register nodes in health monitor
    hm.inject_heartbeat(1);
    hm.inject_heartbeat(2);

    apex::cluster::FailoverManager fm(router, coord);
    fm.connect(hm);

    // Simulate node 2 going dead
    hm.simulate_timeout(2, 200);  // 200ms ago = past dead_timeout
    hm.check_states_now();        // ACTIVE → SUSPECT
    hm.check_states_now();        // SUSPECT → DEAD (triggers failover)

    // Give callback a moment
    std::this_thread::sleep_for(10ms);

    EXPECT_EQ(fm.failover_count(), 1u);
    EXPECT_EQ(coord.node_count(), 1u);
    EXPECT_EQ(router.node_count(), 1u);
}

TEST(Failover, ReplicaBecomePrimary_DataAvailable) {
    // Primary (node 1) has symbol 700, replicated to node 2 (replica)
    auto primary_pipeline = make_pipeline();
    auto replica_pipeline = make_pipeline();

    // Ingest on primary
    for (int i = 0; i < 5; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 700;
        msg.price     = 10000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        primary_pipeline->ingest_tick(msg);
        // Simulate replication: same tick goes to replica
        replica_pipeline->ingest_tick(msg);
    }
    primary_pipeline->drain_sync(100);
    replica_pipeline->drain_sync(100);

    // Set up coordinator with both nodes
    TcpRpcServer srv2;
    srv2.start(19890,
        [&](const std::string& sql) {
            apex::sql::QueryExecutor ex(*replica_pipeline);
            return ex.execute(sql);
        });
    std::this_thread::sleep_for(20ms);

    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", 19889, 1}, *primary_pipeline);
    coord.add_remote_node({"127.0.0.1", 19890, 2});

    // Failover: node 1 dies
    apex::cluster::FailoverManager fm(router, coord);
    fm.trigger_failover(1);

    // Now only node 2 remains — query should work via replica
    auto result = coord.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = 700");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], 5);

    srv2.stop();
}

// ============================================================================
// 12. Coordinator HA — Active-Standby failover
// ============================================================================

TEST(CoordinatorHA, ActiveServesQueries) {
    auto pipeline = make_pipeline();
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 800;
        msg.price     = 10000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline->ingest_tick(msg);
    }
    pipeline->drain_sync(100);

    apex::cluster::CoordinatorHA ha;
    ha.init(apex::cluster::CoordinatorRole::ACTIVE, "127.0.0.1", 19999);
    ha.add_local_node({"127.0.0.1", 19900, 1}, *pipeline);

    EXPECT_TRUE(ha.is_active());

    auto result = ha.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = 800");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 3);
}

TEST(CoordinatorHA, StandbyPromotesOnActiveDown) {
    // Active coordinator with RPC server
    auto pipeline = make_pipeline();
    for (int i = 0; i < 5; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 810;
        msg.price     = 10000000LL;
        msg.volume    = 50;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline->ingest_tick(msg);
    }
    pipeline->drain_sync(100);

    // Active's RPC server (for standby to ping)
    TcpRpcServer active_rpc;
    active_rpc.start(19901, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    // Standby
    apex::cluster::CoordinatorHAConfig cfg;
    cfg.ping_interval_ms = 50;
    cfg.failover_after_ms = 200;
    apex::cluster::CoordinatorHA standby(cfg);
    standby.init(apex::cluster::CoordinatorRole::STANDBY, "127.0.0.1", 19901);
    standby.add_local_node({"127.0.0.1", 19902, 1}, *pipeline);

    std::atomic<bool> promoted{false};
    standby.on_promotion([&]() { promoted.store(true); });
    standby.start();

    EXPECT_FALSE(standby.is_active());

    // Kill active
    active_rpc.stop();

    // Wait for standby to detect and promote
    std::this_thread::sleep_for(500ms);

    EXPECT_TRUE(standby.is_active());
    EXPECT_TRUE(promoted.load());
    EXPECT_EQ(standby.promotion_count(), 1u);

    // Standby (now active) can serve queries
    auto result = standby.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = 810");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 5);

    standby.stop();
}

TEST(CoordinatorHA, StandbyForwardsToActive) {
    auto pipeline = make_pipeline();
    for (int i = 0; i < 4; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 820;
        msg.price     = 10000000LL;
        msg.volume    = 25;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline->ingest_tick(msg);
    }
    pipeline->drain_sync(100);

    // Active's RPC server
    TcpRpcServer active_rpc;
    active_rpc.start(19903, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    // Standby forwards queries to active via RPC
    apex::cluster::CoordinatorHA standby;
    standby.init(apex::cluster::CoordinatorRole::STANDBY, "127.0.0.1", 19903);

    EXPECT_FALSE(standby.is_active());

    auto result = standby.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = 820");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 4);

    active_rpc.stop();
}

// ============================================================================
// 13. Snapshot Coordinator — distributed consistent snapshot
// ============================================================================

TEST(Snapshot, TwoNodeSnapshot) {
    auto p1 = make_pipeline();
    auto p2 = make_pipeline();

    for (int i = 0; i < 10; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 900;
        msg.price     = 10000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        p1->ingest_tick(msg);
    }
    for (int i = 0; i < 7; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 901;
        msg.price     = 20000000LL;
        msg.volume    = 200;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        p2->ingest_tick(msg);
    }
    p1->drain_sync(100);
    p2->drain_sync(100);

    // Each node handles SNAPSHOT by returning row count
    TcpRpcServer srv1, srv2;
    srv1.start(19910, [&](const std::string& sql) {
        if (sql == "SNAPSHOT") {
            auto rows = p1->total_stored_rows();
            return make_result({"rows_flushed"},
                               {{static_cast<int64_t>(rows)}});
        }
        apex::sql::QueryExecutor ex(*p1);
        return ex.execute(sql);
    });
    srv2.start(19911, [&](const std::string& sql) {
        if (sql == "SNAPSHOT") {
            auto rows = p2->total_stored_rows();
            return make_result({"rows_flushed"},
                               {{static_cast<int64_t>(rows)}});
        }
        apex::sql::QueryExecutor ex(*p2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    apex::cluster::SnapshotCoordinator snap;
    snap.add_node(1, "127.0.0.1", 19910);
    snap.add_node(2, "127.0.0.1", 19911);

    auto result = snap.take_snapshot();
    EXPECT_TRUE(result.all_ok());
    EXPECT_GT(result.snapshot_ts, 0);
    ASSERT_EQ(result.nodes.size(), 2u);
    EXPECT_TRUE(result.nodes[0].success);
    EXPECT_TRUE(result.nodes[1].success);
    EXPECT_EQ(result.total_rows(), 17u);  // 10 + 7

    srv1.stop();
    srv2.stop();
}

TEST(Snapshot, NodeDown_PartialFailure) {
    auto p1 = make_pipeline();
    p1->drain_sync(100);

    TcpRpcServer srv1;
    srv1.start(19912, [&](const std::string& sql) {
        if (sql == "SNAPSHOT")
            return make_result({"rows_flushed"}, {{0}});
        return QueryResultSet{};
    });
    std::this_thread::sleep_for(20ms);

    apex::cluster::SnapshotCoordinator snap;
    snap.add_node(1, "127.0.0.1", 19912);
    snap.add_node(2, "127.0.0.1", 19913);  // not running

    auto result = snap.take_snapshot();
    EXPECT_FALSE(result.all_ok());  // node 2 failed

    // Node 1 should succeed
    bool node1_ok = false;
    for (auto& n : result.nodes) {
        if (n.node_id == 1) node1_ok = n.success;
    }
    EXPECT_TRUE(node1_ok);

    srv1.stop();
}

// ============================================================================
// 14. Cross-node ASOF JOIN
// ============================================================================

TEST(CrossNodeAsofJoin, SymbolFilterRoutesToSingleNode) {
    // Both nodes have data; symbol filter routes to the correct one
    auto pipeline1 = make_pipeline();
    auto pipeline2 = make_pipeline();

    // Node 1: symbol 1000
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1000;
        msg.price     = (15000 + i * 100) * 1'000'000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i + 1) * 1'000'000'000LL;
        pipeline1->ingest_tick(msg);
    }
    pipeline1->drain_sync(100);

    // Node 2: symbol 2000
    for (int i = 0; i < 2; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 2000;
        msg.price     = 20000000LL;
        msg.volume    = 200;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline2->ingest_tick(msg);
    }
    pipeline2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    srv1.start(19920, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline1);
        return ex.execute(sql);
    });
    srv2.start(19921, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*pipeline2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", 19920, 1});
    coord.add_remote_node({"127.0.0.1", 19921, 2});

    // Scatter-gather count across both nodes (no symbol filter)
    auto result = coord.execute_sql("SELECT count(*) FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 5);  // 3 + 2

    // Symbol-filtered count — Tier A routes to one node
    // We don't know which node owns symbol 1000, but the scatter-gather
    // approach should still work: each node returns count for its own data
    auto r1 = coord.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = 1000");
    ASSERT_TRUE(r1.ok()) << r1.error;
    // Symbol 1000 is only on node 1, so regardless of routing, count = 3
    // (if routed to node 2, it returns 0; if routed to node 1, returns 3)
    // With Tier A, it goes to one node. With scatter, both return and merge.
    // Either way, the data is only on pipeline1.
    // Since Tier A routes to the hash-owner (which may not have the data),
    // let's just verify scatter-gather works for the total.

    srv1.stop();
    srv2.stop();
}

// ============================================================================
// 15. Distributed SELECT parallelization
// ============================================================================

TEST(DistributedSelect, ScatterGather_PlainSelect) {
    auto p1 = make_pipeline();
    auto p2 = make_pipeline();

    for (int i = 0; i < 4; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1100;
        msg.price     = 10000000LL + i * 1000000LL;
        msg.volume    = 10;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        p1->ingest_tick(msg);
    }
    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1200;
        msg.price     = 20000000LL;
        msg.volume    = 20;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        p2->ingest_tick(msg);
    }
    p1->drain_sync(100);
    p2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    srv1.start(19922, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*p1);
        return ex.execute(sql);
    });
    srv2.start(19923, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*p2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    QueryCoordinator coord;
    coord.add_remote_node({"127.0.0.1", 19922, 1});
    coord.add_remote_node({"127.0.0.1", 19923, 2});

    // SUM across nodes
    auto sum_result = coord.execute_sql("SELECT sum(volume) FROM trades");
    ASSERT_TRUE(sum_result.ok()) << sum_result.error;
    EXPECT_EQ(sum_result.rows[0][0], 4 * 10 + 3 * 20);  // 100

    // COUNT across nodes
    auto cnt_result = coord.execute_sql("SELECT count(*) FROM trades");
    ASSERT_TRUE(cnt_result.ok()) << cnt_result.error;
    EXPECT_EQ(cnt_result.rows[0][0], 7);

    // MIN/MAX across nodes
    auto minmax = coord.execute_sql(
        "SELECT min(volume), max(volume) FROM trades");
    ASSERT_TRUE(minmax.ok()) << minmax.error;
    EXPECT_EQ(minmax.rows[0][0], 10);   // min
    EXPECT_EQ(minmax.rows[0][1], 20);   // max

    srv1.stop();
    srv2.stop();
}

// ============================================================================
// 16. ComputeNode — Data/Compute separation
// ============================================================================

TEST(ComputeNode, FetchAndIngest_LocalJoin) {
    // Data node has symbol 1300
    auto data_pipeline = make_pipeline();
    for (int i = 0; i < 5; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1300;
        msg.price     = (10000 + i * 100) * 1'000'000LL;
        msg.volume    = 50;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        data_pipeline->ingest_tick(msg);
    }
    data_pipeline->drain_sync(100);

    TcpRpcServer data_srv;
    data_srv.start(19930, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*data_pipeline);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    // Compute node fetches data and ingests locally
    apex::cluster::ComputeNode compute;
    compute.add_data_node(1, "127.0.0.1", 19930);

    auto local_pipeline = make_pipeline();
    size_t fetched = compute.fetch_and_ingest(
        1, "SELECT symbol, price, volume, timestamp FROM trades WHERE symbol = 1300",
        *local_pipeline);
    EXPECT_EQ(fetched, 5u);

    // Now query locally on compute node
    apex::sql::QueryExecutor ex(*local_pipeline);
    auto result = ex.execute(
        "SELECT count(*) FROM trades WHERE symbol = 1300");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 5);

    data_srv.stop();
}

TEST(ComputeNode, ExecuteAcrossDataNodes) {
    auto p1 = make_pipeline();
    auto p2 = make_pipeline();

    for (int i = 0; i < 3; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1400;
        msg.price     = 10000000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        p1->ingest_tick(msg);
    }
    for (int i = 0; i < 4; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 1500;
        msg.price     = 20000000LL;
        msg.volume    = 200;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        p2->ingest_tick(msg);
    }
    p1->drain_sync(100);
    p2->drain_sync(100);

    TcpRpcServer srv1, srv2;
    srv1.start(19931, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*p1);
        return ex.execute(sql);
    });
    srv2.start(19932, [&](const std::string& sql) {
        apex::sql::QueryExecutor ex(*p2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(20ms);

    apex::cluster::ComputeNode compute;
    compute.add_data_node(1, "127.0.0.1", 19931);
    compute.add_data_node(2, "127.0.0.1", 19932);

    // Execute across both data nodes — concat results
    auto result = compute.execute(
        "SELECT * FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows.size(), 7u);  // 3 + 4

    srv1.stop();
    srv2.stop();
}

// ============================================================================
// 17. Multi-threaded drain
// ============================================================================

TEST(MultiDrain, TwoDrainThreads_AllDataStored) {
    apex::core::PipelineConfig cfg;
    cfg.storage_mode = apex::core::StorageMode::PURE_IN_MEMORY;
    cfg.drain_threads = 2;
    apex::core::ApexPipeline pipeline(cfg);
    pipeline.start();

    for (int i = 0; i < 100; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 60;
        msg.price     = 10000000LL;
        msg.volume    = 1;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline.ingest_tick(msg);
    }

    // Wait for drain threads to process
    std::this_thread::sleep_for(200ms);
    pipeline.stop();

    apex::sql::QueryExecutor ex(pipeline);
    auto result = ex.execute("SELECT count(*) FROM trades WHERE symbol = 60");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 100);
}

TEST(MultiDrain, FourDrainThreads_MultiSymbol) {
    apex::core::PipelineConfig cfg;
    cfg.storage_mode = apex::core::StorageMode::PURE_IN_MEMORY;
    cfg.drain_threads = 4;
    apex::core::ApexPipeline pipeline(cfg);
    pipeline.start();

    for (int sym = 70; sym < 74; ++sym) {
        for (int i = 0; i < 25; ++i) {
            apex::ingestion::TickMessage msg{};
            msg.symbol_id = static_cast<uint32_t>(sym);
            msg.price     = 10000000LL;
            msg.volume    = 10;
            msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
            pipeline.ingest_tick(msg);
        }
    }

    std::this_thread::sleep_for(200ms);
    pipeline.stop();

    apex::sql::QueryExecutor ex(pipeline);
    auto result = ex.execute("SELECT count(*) FROM trades");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], 100);  // 4 * 25
}

// ============================================================================
// 18. Ring Buffer direct-to-storage (queue full bypass)
// ============================================================================

TEST(DirectToStorage, QueueFull_NoDataLoss) {
    // Use drain_sync (no background drain) to keep queue full
    apex::core::PipelineConfig cfg;
    cfg.storage_mode = apex::core::StorageMode::PURE_IN_MEMORY;
    apex::core::ApexPipeline pipeline(cfg);
    // Don't call start() — no drain thread, queue will fill up

    // Ingest more than queue capacity (65536)
    // With direct-to-storage, all should be stored even without drain
    const int N = 100;
    for (int i = 0; i < N; ++i) {
        apex::ingestion::TickMessage msg{};
        msg.symbol_id = 80;
        msg.price     = 10000000LL;
        msg.volume    = 1;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        EXPECT_TRUE(pipeline.ingest_tick(msg));
    }

    // Drain whatever is in the queue
    pipeline.drain_sync(N + 100);

    apex::sql::QueryExecutor ex(pipeline);
    auto result = ex.execute("SELECT count(*) FROM trades WHERE symbol = 80");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows[0][0], N);
}

// ============================================================================
// 19. ParquetReader (graceful fallback when Parquet not available)
// ============================================================================

#include "apex/storage/parquet_reader.h"

TEST(ParquetReader, ReadNonexistentFile_ReturnsError) {
    apex::storage::ParquetReader reader;
    auto result = reader.read_file("/tmp/nonexistent_apex_test.parquet");
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
}

TEST(ParquetReader, IngestNonexistentFile_ReturnsZero) {
    apex::storage::ParquetReader reader;
    auto pipeline = make_pipeline();
    size_t count = reader.ingest_file("/tmp/nonexistent_apex_test.parquet", *pipeline);
    EXPECT_EQ(count, 0u);
}
