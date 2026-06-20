// ============================================================================
// ZeptoDB: Distributed INSERT routing tests (devlog 103)
// ============================================================================
// Verifies that QueryExecutor::exec_insert() routes ticks through the cluster
// PartitionRouter when set_cluster_node() has been wired, and preserves the
// original direct-to-pipeline behaviour when it has not.
//
// Scenarios:
//   (a) INSERT routes to the remote owner (two-node TCP RPC round trip)
//   (b) INSERT lands locally when this node owns the symbol
//   (c) Backward compatibility: no cluster_node_ → direct to local pipeline
//   (d) Explicit null: set_cluster_node(nullptr) reverts to fallback
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/cluster/cluster_node.h"
#include "zeptodb/cluster/cluster_node_base.h"
#include "zeptodb/cluster/coordinator_routing_adapter.h"
#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"

#include "shm_backend.h"
#include "test_port_helper.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace zeptodb;
using namespace zeptodb::cluster;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers (borrowed pattern from test_cluster.cpp)
// ---------------------------------------------------------------------------
static void ensure_enterprise_license_di() {
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

// Find a symbol_id whose PartitionRouter routes to `target_node` on `node`
// under the given `table_id`.  Must use the table-aware overload because
// ClusterNode::ingest_tick() routes via (msg.table_id, msg.symbol_id) and the
// named table's stable id is not the same as the legacy table_id=0 route.
static SymbolId find_symbol_routed_to(ClusterNode<SharedMemBackend>& node,
                                      uint16_t table_id,
                                      NodeId target_node) {
    for (SymbolId s = 1; s < 10000; ++s) {
        if (node.route(table_id, s) == target_node) return s;
    }
    return 0;
}

namespace {
class StartedPipeline {
public:
    explicit StartedPipeline(zeptodb::core::PipelineConfig config)
        : pipeline_(std::move(config)) {
        pipeline_.start();
    }

    ~StartedPipeline() {
        pipeline_.stop();
    }

    StartedPipeline(const StartedPipeline&) = delete;
    StartedPipeline& operator=(const StartedPipeline&) = delete;

    zeptodb::core::ZeptoPipeline& get() { return pipeline_; }

private:
    zeptodb::core::ZeptoPipeline pipeline_;
};
}  // namespace

// Wait for pipeline to drain so ticks_stored reflects the INSERT.  (Unused by
// ticks_ingested-based assertions below but kept for clarity should a test
// switch to stored-visibility checks.)
// static void wait_for_stored(...);

// ---------------------------------------------------------------------------
// (c) Backward compatibility — no cluster_node_ set
// ---------------------------------------------------------------------------
TEST(DistributedInsert, NoClusterNode_FallsBackToPipeline) {
    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    auto pipeline = std::make_unique<zeptodb::core::ZeptoPipeline>(pc);
    pipeline->start();

    zeptodb::sql::QueryExecutor ex(*pipeline);
    auto cr = ex.execute(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)");
    ASSERT_TRUE(cr.ok()) << cr.error;

    uint64_t before = pipeline->stats().ticks_ingested.load();
    auto ir = ex.execute("INSERT INTO trades VALUES (1, 100, 10, 1000)");
    ASSERT_TRUE(ir.ok()) << ir.error;

    uint64_t after = pipeline->stats().ticks_ingested.load();
    EXPECT_EQ(after - before, 1u) << "INSERT must reach local pipeline when no cluster_node_ is set";

    pipeline->stop();
}

// ---------------------------------------------------------------------------
// (d) Explicit nullptr reverts to fallback
// ---------------------------------------------------------------------------
// Stub ClusterNodeBase that counts ingest_tick() calls.  Used to confirm that
// set_cluster_node(&stub) routes through the base, and set_cluster_node(nullptr)
// reverts to the local pipeline.
namespace {
class CountingClusterNode : public ClusterNodeBase {
public:
    std::atomic<uint64_t> calls{0};
    bool ingest_tick(zeptodb::ingestion::TickMessage) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool ingest_typed_row(const zeptodb::core::TypedRowMessage&) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};
}  // namespace

TEST(DistributedInsert, NullAfterSet_RevertsToPipeline) {
    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    auto pipeline = std::make_unique<zeptodb::core::ZeptoPipeline>(pc);
    pipeline->start();

    zeptodb::sql::QueryExecutor ex(*pipeline);
    ASSERT_TRUE(ex.execute(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)").ok());

    CountingClusterNode stub;
    ex.set_cluster_node(&stub);

    // With stub wired: pipeline must NOT receive the tick; stub must.
    uint64_t pipe_before = pipeline->stats().ticks_ingested.load();
    ASSERT_TRUE(ex.execute("INSERT INTO trades VALUES (1, 100, 10, 1000)").ok());
    EXPECT_EQ(stub.calls.load(), 1u);
    EXPECT_EQ(pipeline->stats().ticks_ingested.load(), pipe_before)
        << "INSERT must NOT hit local pipeline when cluster_node_ is set";

    // After explicit null: pipeline receives the tick again; stub stays at 1.
    ex.set_cluster_node(nullptr);
    ASSERT_TRUE(ex.execute("INSERT INTO trades VALUES (2, 200, 20, 2000)").ok());
    EXPECT_EQ(stub.calls.load(), 1u) << "stub must not be called after nullptr";
    EXPECT_GE(pipeline->stats().ticks_ingested.load(), pipe_before + 1)
        << "INSERT must reach pipeline after set_cluster_node(nullptr)";

    pipeline->stop();
}

// ---------------------------------------------------------------------------
// (b) INSERT lands locally when this node owns the symbol
// ---------------------------------------------------------------------------
// Uses a real two-node ClusterNode<SharedMemBackend> pair so route() is driven
// by PartitionRouter.  We disable remote ingest so any mis-route would surface
// as a false return rather than a silent remote dispatch.
TEST(DistributedInsert, OwnerIsSelf_IngestsLocally) {
    ensure_enterprise_license_di();

    ClusterConfig cfg1, cfg2;
    cfg1.self = {"127.0.0.1", zepto_test_util::pick_free_port(), 1};
    cfg2.self = {"127.0.0.1", zepto_test_util::pick_free_port(), 2};
    cfg1.pipeline.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    cfg2.pipeline.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    cfg1.enable_remote_ingest = false;
    cfg2.enable_remote_ingest = false;

    auto n1 = std::make_unique<ClusterNode<SharedMemBackend>>(cfg1);
    auto n2 = std::make_unique<ClusterNode<SharedMemBackend>>(cfg2);
    n1->join_cluster();
    n2->join_cluster({cfg1.self});
    n1->router().add_node(2);

    zeptodb::sql::QueryExecutor ex(n1->pipeline());
    ex.set_cluster_node(n1.get());
    ASSERT_TRUE(ex.execute(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)").ok());

    // Resolve real table_id AFTER CREATE TABLE, then pick a symbol this node
    // owns under that exact (table_id, symbol) key — same key ingest_tick uses.
    uint16_t tid = n1->pipeline().schema_registry().get_table_id("trades");
    ASSERT_NE(tid, 0u);
    SymbolId sym = find_symbol_routed_to(*n1, tid, 1);
    ASSERT_NE(sym, 0u);

    uint64_t n1_before = n1->pipeline().stats().ticks_ingested.load();
    uint64_t n2_before = n2->pipeline().stats().ticks_ingested.load();

    auto r = ex.execute("INSERT INTO trades VALUES (" + std::to_string(sym) +
                        ", 100, 10, 1000)");
    ASSERT_TRUE(r.ok()) << r.error;

    uint64_t n1_after = n1->pipeline().stats().ticks_ingested.load();
    uint64_t n2_after = n2->pipeline().stats().ticks_ingested.load();
    EXPECT_EQ(n1_after - n1_before, 1u) << "tick must land on owner (node 1)";
    EXPECT_EQ(n2_after - n2_before, 0u) << "tick must NOT land on non-owner (node 2)";

    n1->leave_cluster();
    n2->leave_cluster();
}

// ---------------------------------------------------------------------------
// (a) INSERT routes to remote owner (two-node TCP RPC round trip)
// ---------------------------------------------------------------------------
// Requires enable_remote_ingest=true so ClusterNode::ingest_tick dispatches
// over TcpRpcClient to node 2 when node 2 owns the symbol.
//
// NOTE: ClusterNode derives peer RPC port from (seed.port + 100), so we must
// let rpc_port default — NOT set it to a kernel-picked port — otherwise the
// client connects to the wrong port.  We request `self.port` as a free port
// and trust port+100 is also free (loopback, per-process PID-based offset).
TEST(DistributedInsert, RoutesToRemoteOwner) {
    ensure_enterprise_license_di();

    // Pick free ports for self.port; rpc_port auto-derives as self.port + 100.
    uint16_t p1 = zepto_test_util::pick_free_port();
    uint16_t p2 = zepto_test_util::pick_free_port();
    // Sanity: ensure neither p+100 collides with the other node's base port.
    // If they do, bump one by 1.  (Extremely unlikely on loopback.)
    if (p1 + 100 == p2 || p2 + 100 == p1 || p1 == p2) {
        p2 = static_cast<uint16_t>(p2 ^ 1);
    }

    ClusterConfig cfg1, cfg2;
    cfg1.self = {"127.0.0.1", p1, 1};
    cfg2.self = {"127.0.0.1", p2, 2};
    cfg1.pipeline.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    cfg2.pipeline.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    cfg1.enable_remote_ingest = true;
    cfg2.enable_remote_ingest = true;

    auto n1 = std::make_unique<ClusterNode<SharedMemBackend>>(cfg1);
    auto n2 = std::make_unique<ClusterNode<SharedMemBackend>>(cfg2);

    // Start node 2 first so its RPC server is listening before node 1 dials in.
    n2->join_cluster();
    n1->join_cluster({cfg2.self});
    n2->router().add_node(1);

    std::this_thread::sleep_for(100ms);

    // Create an unrelated table on only one node first. `trades` must still
    // get the same stable table_id on both nodes; otherwise remote INSERTs can
    // be stored under one pod's id and queried under another pod's id.
    ASSERT_TRUE(n2->execute_sql_local(
        "CREATE TABLE warmup_only_on_n2 (symbol INT64, price INT64)").ok());

    // CREATE TABLE on BOTH nodes so the remote INSERT can resolve table_id.
    ASSERT_TRUE(n1->execute_sql_local(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)").ok());
    ASSERT_TRUE(n2->execute_sql_local(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)").ok());

    // Resolve the real stable table_id and pick a symbol that routes to node 2
    // under that key.  Using the single-arg route(sym) — i.e. table_id=0 —
    // would pick a key from a different hash slot than ingest_tick's
    // (table_id, sym) lookup, causing flaky half-the-time mis-routes.
    uint16_t tid = n1->pipeline().schema_registry().get_table_id("trades");
    ASSERT_NE(tid, 0u);
    EXPECT_EQ(tid, n2->pipeline().schema_registry().get_table_id("trades"));
    SymbolId sym = 0;
    for (SymbolId s = 1; s < 10000; ++s) {
        if (n1->route(tid, s) == 2 && n1->route(s) == 1) {
            sym = s;
            break;
        }
    }
    ASSERT_NE(sym, 0u);

    zeptodb::sql::QueryExecutor ex(n1->pipeline());
    ex.set_cluster_node(n1.get());

    uint64_t n1_before = n1->pipeline().stats().ticks_ingested.load();
    uint64_t n2_before = n2->pipeline().stats().ticks_ingested.load();

    auto r = ex.execute("INSERT INTO trades VALUES (" + std::to_string(sym) +
                        ", 100, 10, 1000)");
    ASSERT_TRUE(r.ok()) << r.error;

    // Allow the RPC round trip to complete.
    for (int i = 0; i < 200; ++i) {
        if (n2->pipeline().stats().ticks_ingested.load() > n2_before) break;
        std::this_thread::sleep_for(10ms);
    }

    uint64_t n1_after = n1->pipeline().stats().ticks_ingested.load();
    uint64_t n2_after = n2->pipeline().stats().ticks_ingested.load();
    EXPECT_EQ(n2_after - n2_before, 1u)
        << "tick must be routed to remote owner (node 2)";
    EXPECT_EQ(n1_after - n1_before, 0u)
        << "tick must NOT land on node 1 (non-owner receiving HTTP INSERT)";

    EXPECT_TRUE(n2->pipeline().schema_registry().has_data("trades"))
        << "remote tick RPC must make the table visible to SQL on the owner";

    zeptodb::sql::QueryResultSet remote_count;
    for (int i = 0; i < 200; ++i) {
        remote_count = n2->execute_sql_local(
            "SELECT count(*) FROM trades WHERE symbol = " + std::to_string(sym));
        if (remote_count.ok() &&
            !remote_count.rows.empty() &&
            !remote_count.rows[0].empty() &&
            remote_count.rows[0][0] == 1) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(remote_count.ok()) << remote_count.error;
    ASSERT_FALSE(remote_count.rows.empty());
    ASSERT_FALSE(remote_count.rows[0].empty());
    EXPECT_EQ(remote_count.rows[0][0], 1)
        << "remote owner must expose routed ticks through local SQL";

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", p1, 1}, n1->pipeline());
    coord.add_remote_node({"127.0.0.1", static_cast<uint16_t>(p2 + 100), 2});
    auto coordinated_count = coord.execute_sql(
        "SELECT count(*) FROM trades WHERE symbol = " + std::to_string(sym));
    ASSERT_TRUE(coordinated_count.ok()) << coordinated_count.error;
    ASSERT_FALSE(coordinated_count.rows.empty());
    ASSERT_FALSE(coordinated_count.rows[0].empty());
    EXPECT_EQ(coordinated_count.rows[0][0], 1)
        << "coordinator single-symbol SELECT must use the same table-aware "
           "route as distributed INSERT";

    n1->leave_cluster();
    n2->leave_cluster();
}

TEST(DistributedInsert, CoordinatorAdapterRoutesTypedRowsOverRpcAndDecodesRemoteStrings) {
    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    StartedPipeline local(pc);
    StartedPipeline remote(pc);

    const uint16_t local_port = zepto_test_util::pick_free_port();
    uint16_t remote_rpc_port = zepto_test_util::pick_free_port();
    if (remote_rpc_port == local_port) {
        remote_rpc_port = static_cast<uint16_t>(remote_rpc_port ^ 1);
    }

    TcpRpcServer remote_rpc;
    remote_rpc.set_thread_pool_size(2);
    remote_rpc.set_typed_row_ingest_callback(
        [&remote](zeptodb::core::TypedRowMessage row) {
            return remote.get().ingest_typed_row(std::move(row));
        });
    remote_rpc.start(remote_rpc_port, [&remote](const std::string& sql) {
        zeptodb::sql::QueryExecutor ex(remote.get());
        return ex.execute(sql);
    });
    ASSERT_TRUE(remote_rpc.is_running()) << "remote typed-row RPC server did not start";

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", local_port, 1}, local.get());
    coord.add_remote_node({"127.0.0.1", remote_rpc_port, 8});

    const std::string table = "action_outcome_episodes";
    auto created = coord.execute_sql(
        "CREATE TABLE " + table + " ("
        "episode_id STRING, action_class STRING, outcome_score INT64, "
        "timestamp_ns TIMESTAMP_NS)");
    ASSERT_TRUE(created.ok()) << created.error;

    const uint16_t table_id = local.get().schema_registry().get_table_id(table);
    ASSERT_NE(table_id, 0u);
    ASSERT_EQ(remote.get().schema_registry().get_table_id(table), table_id);
    {
        auto lock = coord.router_read_lock();
        ASSERT_EQ(coord.router().route(table_id, 0), 8)
            << "fixture expects the Action-Outcome table's default shard on node 8";
    }

    auto empty = coord.execute_sql("SELECT count(*) FROM " + table);
    ASSERT_TRUE(empty.ok()) << empty.error;
    ASSERT_FALSE(empty.rows.empty());
    ASSERT_FALSE(empty.rows[0].empty());
    EXPECT_EQ(empty.rows[0][0], 0);

    CoordinatorRoutingAdapter::RpcClientMap remotes;
    remotes.emplace(
        8,
        std::make_shared<TcpRpcClient>("127.0.0.1", remote_rpc_port, 2000, 2, 5000));
    CoordinatorRoutingAdapter adapter(
        &coord.router(), &coord.router_mutex(), &local.get(), 1, &remotes);

    zeptodb::sql::QueryExecutor local_ex(local.get());
    local_ex.set_cluster_node(&adapter);

    const uint64_t local_before = local.get().stats().ticks_ingested.load();
    const uint64_t remote_before = remote.get().stats().ticks_ingested.load();

    auto inserted = local_ex.execute(
        "INSERT INTO " + table +
        " (episode_id, action_class, outcome_score, timestamp_ns) VALUES "
        "('aoe_remote_001', 'rollback', 42, 1000000000)");
    ASSERT_TRUE(inserted.ok()) << inserted.error;
    ASSERT_FALSE(inserted.rows.empty());
    ASSERT_FALSE(inserted.rows[0].empty());
    EXPECT_EQ(inserted.rows[0][0], 1);

    for (int i = 0; i < 200; ++i) {
        if (remote.get().stats().ticks_ingested.load() > remote_before) break;
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(local.get().stats().ticks_ingested.load() - local_before, 0u)
        << "typed-row INSERT must not materialize on the non-owner local node";
    EXPECT_EQ(remote.get().stats().ticks_ingested.load() - remote_before, 1u)
        << "typed-row INSERT must cross RPC and materialize on the remote owner";
    EXPECT_TRUE(remote.get().schema_registry().has_data(table));

    auto selected = coord.execute_sql(
        "SELECT episode_id, action_class, outcome_score FROM " + table);
    ASSERT_TRUE(selected.ok()) << selected.error;
    ASSERT_EQ(selected.rows.size(), 1u);
    ASSERT_EQ(selected.rows[0].size(), 3u);
    EXPECT_EQ(selected.rows[0][2], 42);
    ASSERT_GE(selected.string_rows.size(), 2u);
    EXPECT_EQ(selected.string_rows[0], "aoe_remote_001");
    EXPECT_EQ(selected.string_rows[1], "rollback");

    remote_rpc.stop();
}

TEST(DistributedInsert, ClusterWindowMaterializesGenericTableValues) {
    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    StartedPipeline local(pc);
    StartedPipeline remote(pc);

    const uint16_t local_port = zepto_test_util::pick_free_port();
    uint16_t remote_rpc_port = zepto_test_util::pick_free_port();
    if (remote_rpc_port == local_port) {
        remote_rpc_port = static_cast<uint16_t>(remote_rpc_port ^ 1);
    }

    TcpRpcServer remote_rpc;
    remote_rpc.set_thread_pool_size(2);
    remote_rpc.set_typed_row_ingest_callback(
        [&remote](zeptodb::core::TypedRowMessage row) {
            return remote.get().ingest_typed_row(std::move(row));
        });
    remote_rpc.start(remote_rpc_port, [&remote](const std::string& sql) {
        zeptodb::sql::QueryExecutor ex(remote.get());
        return ex.execute(sql);
    });
    ASSERT_TRUE(remote_rpc.is_running()) << "remote typed-row RPC server did not start";

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", local_port, 1}, local.get());
    coord.add_remote_node({"127.0.0.1", remote_rpc_port, 8});

    const std::string table = "action_outcome_episodes";
    auto created = coord.execute_sql(
        "CREATE TABLE " + table + " ("
        "episode_id STRING, group_id INT64, recommendation_rank INT64, "
        "score_micros INT64, timestamp_ns TIMESTAMP_NS)");
    ASSERT_TRUE(created.ok()) << created.error;

    const uint16_t table_id = local.get().schema_registry().get_table_id(table);
    ASSERT_NE(table_id, 0u);
    ASSERT_EQ(remote.get().schema_registry().get_table_id(table), table_id);
    {
        auto lock = coord.router_read_lock();
        ASSERT_EQ(coord.router().route(table_id, 0), 8)
            << "fixture expects the Action-Outcome table's default shard on node 8";
    }

    CoordinatorRoutingAdapter::RpcClientMap remotes;
    remotes.emplace(
        8,
        std::make_shared<TcpRpcClient>("127.0.0.1", remote_rpc_port, 2000, 2, 5000));
    CoordinatorRoutingAdapter adapter(
        &coord.router(), &coord.router_mutex(), &local.get(), 1, &remotes);

    zeptodb::sql::QueryExecutor local_ex(local.get());
    local_ex.set_cluster_node(&adapter);

    const uint64_t remote_before = remote.get().stats().ticks_ingested.load();
    ASSERT_TRUE(local_ex.execute(
        "INSERT INTO " + table + " "
        "(episode_id, group_id, recommendation_rank, score_micros, timestamp_ns) "
        "VALUES ('aoe_remote_001', 10, 1, 420000, 1000000000)"
    ).ok());
    ASSERT_TRUE(local_ex.execute(
        "INSERT INTO " + table + " "
        "(episode_id, group_id, recommendation_rank, score_micros, timestamp_ns) "
        "VALUES ('aoe_remote_002', 10, 2, 840000, 1000000001)"
    ).ok());

    for (int i = 0; i < 200; ++i) {
        if (remote.get().stats().ticks_ingested.load() >= remote_before + 2) break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(remote.get().stats().ticks_ingested.load() - remote_before, 2u);

    auto ranked = coord.execute_sql(
        "SELECT group_id, recommendation_rank, score_micros, "
        "ROW_NUMBER() OVER (PARTITION BY group_id ORDER BY recommendation_rank) AS rank_check "
        "FROM " + table + " ORDER BY recommendation_rank");
    ASSERT_TRUE(ranked.ok()) << ranked.error;
    ASSERT_EQ(ranked.rows.size(), 2u);
    ASSERT_EQ(ranked.rows[0].size(), 4u);
    EXPECT_EQ(ranked.rows[0][0], 10);
    EXPECT_EQ(ranked.rows[0][1], 1);
    EXPECT_EQ(ranked.rows[0][2], 420000);
    EXPECT_EQ(ranked.rows[0][3], 1);
    EXPECT_EQ(ranked.rows[1][0], 10);
    EXPECT_EQ(ranked.rows[1][1], 2);
    EXPECT_EQ(ranked.rows[1][2], 840000);
    EXPECT_EQ(ranked.rows[1][3], 2);

    auto lagged = coord.execute_sql(
        "SELECT group_id, recommendation_rank, score_micros, "
        "LAG(score_micros, 1, 0) OVER (PARTITION BY group_id ORDER BY recommendation_rank) AS prev_score "
        "FROM " + table + " ORDER BY recommendation_rank");
    ASSERT_TRUE(lagged.ok()) << lagged.error;
    ASSERT_EQ(lagged.rows.size(), 2u);
    ASSERT_EQ(lagged.rows[0].size(), 4u);
    EXPECT_EQ(lagged.rows[0][0], 10);
    EXPECT_EQ(lagged.rows[0][1], 1);
    EXPECT_EQ(lagged.rows[0][2], 420000);
    EXPECT_EQ(lagged.rows[0][3], 0);
    EXPECT_EQ(lagged.rows[1][0], 10);
    EXPECT_EQ(lagged.rows[1][1], 2);
    EXPECT_EQ(lagged.rows[1][2], 840000);
    EXPECT_EQ(lagged.rows[1][3], 420000);

    remote_rpc.stop();
}
