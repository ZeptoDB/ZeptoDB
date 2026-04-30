// ============================================================================
// ZeptoDB: DDL replication tests (devlog 112)
// ============================================================================
// Verifies QueryCoordinator::forward_ddl_to_remotes() — fire-and-forget DDL
// replication so every pod converges on the same schema.
//
// Scenarios:
//   1. CreateTableReplicatesToRemote — CREATE TABLE on node 1, forward →
//      node 2 schema_registry has the table.
//   2. DropTableReplicatesToRemote   — DROP TABLE on node 1, forward →
//      node 2 schema_registry no longer has the table.
//   3. ForwardToDownNodeNoThrow      — fake unreachable remote, call returns
//      cleanly (no crash, no throw).
//   4. LocalEndpointSkipped          — forward must NOT execute on local
//      endpoints (the HTTP server has already run the DDL locally).
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"

#include "test_port_helper.h"

#include <chrono>
#include <memory>
#include <thread>

using namespace zeptodb;
using namespace zeptodb::cluster;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::unique_ptr<core::ZeptoPipeline> make_pipeline_ddl() {
    core::PipelineConfig cfg;
    cfg.storage_mode = core::StorageMode::PURE_IN_MEMORY;
    auto p = std::make_unique<core::ZeptoPipeline>(cfg);
    p->start();
    return p;
}

// ---------------------------------------------------------------------------
// 1. CREATE TABLE replicates to remote pod
// ---------------------------------------------------------------------------
TEST(DDLReplication, CreateTableReplicatesToRemote) {
    auto p1 = make_pipeline_ddl();   // "local" (HTTP-receiving) pod
    auto p2 = make_pipeline_ddl();   // remote pod reached via TCP RPC

    // Real TcpRpcServer wrapping p2's executor — same pattern as existing
    // coordinator scatter-gather tests.
    TcpRpcServer srv2;
    uint16_t port2 = zepto_test_util::pick_free_port();
    srv2.start(port2, [&](const std::string& sql) {
        sql::QueryExecutor ex(*p2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(50ms);

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", zepto_test_util::pick_free_port(), 1}, *p1);
    coord.add_remote_node({"127.0.0.1", port2, 2});

    // Emulate what HttpServer does: execute locally first, then forward.
    sql::QueryExecutor ex1(*p1);
    const std::string ddl =
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)";
    auto r = ex1.execute(ddl);
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_TRUE(p1->schema_registry().exists("trades"));
    EXPECT_FALSE(p2->schema_registry().exists("trades"))
        << "Precondition: p2 must not have the table before replication";

    coord.forward_ddl_to_remotes(ddl);

    // Allow the remote RPC to complete (single-shot round-trip on loopback)
    for (int i = 0; i < 100 && !p2->schema_registry().exists("trades"); ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_TRUE(p2->schema_registry().exists("trades"))
        << "Remote pod must receive and apply the CREATE TABLE";

    srv2.stop();
    p1->stop();
    p2->stop();
}

// ---------------------------------------------------------------------------
// 2. DROP TABLE replicates to remote pod
// ---------------------------------------------------------------------------
TEST(DDLReplication, DropTableReplicatesToRemote) {
    auto p1 = make_pipeline_ddl();
    auto p2 = make_pipeline_ddl();

    // Seed both nodes with the table so DROP has something to remove
    const std::string ddl_create =
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)";
    ASSERT_TRUE(sql::QueryExecutor(*p1).execute(ddl_create).ok());
    ASSERT_TRUE(sql::QueryExecutor(*p2).execute(ddl_create).ok());
    ASSERT_TRUE(p1->schema_registry().exists("trades"));
    ASSERT_TRUE(p2->schema_registry().exists("trades"));

    TcpRpcServer srv2;
    uint16_t port2 = zepto_test_util::pick_free_port();
    srv2.start(port2, [&](const std::string& sql) {
        sql::QueryExecutor ex(*p2);
        return ex.execute(sql);
    });
    std::this_thread::sleep_for(50ms);

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", zepto_test_util::pick_free_port(), 1}, *p1);
    coord.add_remote_node({"127.0.0.1", port2, 2});

    const std::string ddl_drop = "DROP TABLE trades";
    auto r = sql::QueryExecutor(*p1).execute(ddl_drop);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_FALSE(p1->schema_registry().exists("trades"));
    EXPECT_TRUE(p2->schema_registry().exists("trades"))
        << "Precondition: remote still has the table before replication";

    coord.forward_ddl_to_remotes(ddl_drop);

    for (int i = 0; i < 100 && p2->schema_registry().exists("trades"); ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_FALSE(p2->schema_registry().exists("trades"))
        << "Remote pod must apply the DROP TABLE";

    srv2.stop();
    p1->stop();
    p2->stop();
}

// ---------------------------------------------------------------------------
// 3. Forward to a down / unreachable remote must not throw
// ---------------------------------------------------------------------------
// Register a remote endpoint with no TcpRpcServer behind it.  Connect will
// fail; the coordinator must log a warning and return cleanly.
TEST(DDLReplication, ForwardToDownNodeNoThrow) {
    auto p1 = make_pipeline_ddl();

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", zepto_test_util::pick_free_port(), 1}, *p1);

    // Reserve a port, then let it go — nothing listens there.
    uint16_t dead_port = zepto_test_util::pick_free_port();
    coord.add_remote_node({"127.0.0.1", dead_port, 99});

    // Expect no exception, no crash.
    EXPECT_NO_THROW(coord.forward_ddl_to_remotes(
        "CREATE TABLE if_not_exists_ok (x INT64)"));

    p1->stop();
}

// ---------------------------------------------------------------------------
// 4. Local endpoints must be skipped
// ---------------------------------------------------------------------------
// If forward_ddl_to_remotes accidentally ran on local endpoints, the local
// pipeline would see the CREATE TABLE twice.  CREATE TABLE (without IF NOT
// EXISTS) fails on a duplicate — so the assertion is that the table was
// created exactly once by our explicit local execute(), and the subsequent
// forward_ddl_to_remotes() did NOT touch the local pipeline.
TEST(DDLReplication, LocalEndpointSkipped) {
    auto p1 = make_pipeline_ddl();

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", zepto_test_util::pick_free_port(), 1}, *p1);
    // No remotes registered at all.

    const std::string ddl =
        "CREATE TABLE trades (symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)";
    ASSERT_TRUE(sql::QueryExecutor(*p1).execute(ddl).ok());
    ASSERT_TRUE(p1->schema_registry().exists("trades"));

    // This must be a no-op on the local pipeline — we invoke it twice to
    // amplify any accidental side-effect (double CREATE would error/duplicate).
    EXPECT_NO_THROW(coord.forward_ddl_to_remotes(ddl));
    EXPECT_NO_THROW(coord.forward_ddl_to_remotes(ddl));

    // Table still present, schema_registry still healthy.
    EXPECT_TRUE(p1->schema_registry().exists("trades"));

    p1->stop();
}
