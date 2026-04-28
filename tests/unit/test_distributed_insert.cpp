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
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"

#include "shm_backend.h"
#include "test_port_helper.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

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
// first CREATE TABLE gets table_id=1, not 0.
static SymbolId find_symbol_routed_to(ClusterNode<SharedMemBackend>& node,
                                      uint16_t table_id,
                                      NodeId target_node) {
    for (SymbolId s = 1; s < 10000; ++s) {
        if (node.route(table_id, s) == target_node) return s;
    }
    return 0;
}

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

    // CREATE TABLE on BOTH nodes so the remote INSERT can resolve table_id.
    ASSERT_TRUE(n1->execute_sql_local(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)").ok());
    ASSERT_TRUE(n2->execute_sql_local(
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)").ok());

    // Resolve the real table_id (CREATE TABLE assigns the first table_id=1)
    // and pick a symbol that routes to node 2 under that key.  Using the
    // single-arg route(sym) — i.e. table_id=0 — would pick a key from a
    // different hash slot than ingest_tick's (table_id, sym) lookup, causing
    // flaky half-the-time mis-routes.
    uint16_t tid = n1->pipeline().schema_registry().get_table_id("trades");
    ASSERT_NE(tid, 0u);
    SymbolId sym = find_symbol_routed_to(*n1, tid, 2);
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

    n1->leave_cluster();
    n2->leave_cluster();
}
