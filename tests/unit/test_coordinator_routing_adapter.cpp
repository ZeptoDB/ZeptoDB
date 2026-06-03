// ============================================================================
// ZeptoDB: CoordinatorRoutingAdapter tests (BACKLOG P8-I3-wire, devlog 111)
// ============================================================================
// Exercises the non-template adapter that bridges QueryExecutor::
// set_cluster_node() to a PartitionRouter + peer RPC client pool. These
// tests cover the adapter in isolation; end-to-end coverage of the wire-up
// inside zepto_http_server comes from EKS multinode benchmarks (stage 3).
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/cluster/coordinator_routing_adapter.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rpc_client_base.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/common/types.h"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Minimal stub implementing RpcClientBase. Counts invocations; returns true.
class CountingRpcClient : public zeptodb::cluster::RpcClientBase {
public:
    std::atomic<int> calls{0};
    bool ingest_tick(const zeptodb::ingestion::TickMessage& /*msg*/) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};

// Build a real PURE_IN_MEMORY pipeline for tests. Kept small to avoid
// over-allocating arenas for a single-tick test.
std::unique_ptr<zeptodb::core::ZeptoPipeline> make_pipeline() {
    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    return std::make_unique<zeptodb::core::ZeptoPipeline>(pc);
}

zeptodb::ingestion::TickMessage make_tick(zeptodb::SymbolId sym,
                                          uint16_t table_id = 1) {
    zeptodb::ingestion::TickMessage m{};
    m.symbol_id = sym;
    m.table_id  = table_id;
    m.price     = 15000;
    m.volume    = 100;
    m.recv_ts   = 1'000'000'000LL;
    return m;
}

} // namespace

// ----------------------------------------------------------------------------
// Case 1: owner is self → lands in local pipeline, no remote calls
// ----------------------------------------------------------------------------
TEST(CoordinatorRoutingAdapter, RoutesToLocalWhenOwnerIsSelf) {
    zeptodb::cluster::PartitionRouter router;
    std::shared_mutex router_mu;
    router.add_node(1);  // only one node, so every route() returns 1

    auto pipeline = make_pipeline();
    ASSERT_TRUE(pipeline->schema_registry().create(
        "local_ticks",
        {{"ts", zeptodb::storage::ColumnType::TIMESTAMP_NS},
         {"symbol", zeptodb::storage::ColumnType::SYMBOL},
         {"price", zeptodb::storage::ColumnType::INT64},
         {"volume", zeptodb::storage::ColumnType::INT64}}));
    const uint16_t table_id =
        pipeline->schema_registry().get_table_id("local_ticks");
    zeptodb::cluster::CoordinatorRoutingAdapter::RpcClientMap remotes;

    zeptodb::cluster::CoordinatorRoutingAdapter adapter(
        &router, &router_mu, pipeline.get(), /*self_id=*/1, &remotes);

    const uint64_t before = pipeline->stats().ticks_ingested.load();
    EXPECT_TRUE(adapter.ingest_tick(make_tick(42, table_id)));
    EXPECT_EQ(pipeline->stats().ticks_ingested.load(), before + 1);
    EXPECT_TRUE(pipeline->schema_registry().has_data("local_ticks"));
}

// ----------------------------------------------------------------------------
// Case 2: owner is remote → lands in matching RPC client, pipeline untouched
// ----------------------------------------------------------------------------
TEST(CoordinatorRoutingAdapter, RoutesToRemoteWhenOwnerIsDifferent) {
    zeptodb::cluster::PartitionRouter router;
    std::shared_mutex router_mu;
    router.add_node(1);
    router.add_node(2);

    // Find a symbol that routes to node 2 under (table_id=1, sym)
    zeptodb::SymbolId target = 0;
    for (zeptodb::SymbolId s = 1; s < 10000; ++s) {
        if (router.route(1, s) == 2) { target = s; break; }
    }
    ASSERT_NE(target, 0u) << "Failed to find a symbol routing to node 2";

    auto pipeline = make_pipeline();
    auto stub = std::make_shared<CountingRpcClient>();
    zeptodb::cluster::CoordinatorRoutingAdapter::RpcClientMap remotes;
    remotes.emplace(2, stub);

    zeptodb::cluster::CoordinatorRoutingAdapter adapter(
        &router, &router_mu, pipeline.get(), /*self_id=*/1, &remotes);

    const uint64_t before = pipeline->stats().ticks_ingested.load();
    EXPECT_TRUE(adapter.ingest_tick(make_tick(target)));
    EXPECT_EQ(stub->calls.load(), 1);
    EXPECT_EQ(pipeline->stats().ticks_ingested.load(), before)
        << "Remote-owned tick must NOT land in local pipeline";
}

// ----------------------------------------------------------------------------
// Case 3: owner is unknown (stale ring) → returns false, no side effects
// ----------------------------------------------------------------------------
TEST(CoordinatorRoutingAdapter, DropsOnUnknownOwner) {
    zeptodb::cluster::PartitionRouter router;
    std::shared_mutex router_mu;
    router.add_node(1);
    router.add_node(99);  // node 99 is in ring but NOT in remotes map

    zeptodb::SymbolId target = 0;
    for (zeptodb::SymbolId s = 1; s < 10000; ++s) {
        if (router.route(1, s) == 99) { target = s; break; }
    }
    ASSERT_NE(target, 0u) << "Failed to find a symbol routing to node 99";

    auto pipeline = make_pipeline();
    auto stub_mismatch = std::make_shared<CountingRpcClient>();  // keyed at 2
    zeptodb::cluster::CoordinatorRoutingAdapter::RpcClientMap remotes;
    remotes.emplace(2, stub_mismatch);

    zeptodb::cluster::CoordinatorRoutingAdapter adapter(
        &router, &router_mu, pipeline.get(), /*self_id=*/1, &remotes);

    const uint64_t before = pipeline->stats().ticks_ingested.load();
    EXPECT_FALSE(adapter.ingest_tick(make_tick(target)));
    EXPECT_EQ(stub_mismatch->calls.load(), 0);
    EXPECT_EQ(pipeline->stats().ticks_ingested.load(), before);
}

// ----------------------------------------------------------------------------
// Case 4: 8 threads × 10K ingests on a 3-node ring — counters add up and
// no crash / no data race (router reads are shared_lock-guarded).
// ----------------------------------------------------------------------------
TEST(CoordinatorRoutingAdapter, ConcurrentRouteLocksAreCorrect) {
    zeptodb::cluster::PartitionRouter router;
    std::shared_mutex router_mu;
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);

    auto pipeline = make_pipeline();
    auto stub2 = std::make_shared<CountingRpcClient>();
    auto stub3 = std::make_shared<CountingRpcClient>();
    zeptodb::cluster::CoordinatorRoutingAdapter::RpcClientMap remotes;
    remotes.emplace(2, stub2);
    remotes.emplace(3, stub3);

    zeptodb::cluster::CoordinatorRoutingAdapter adapter(
        &router, &router_mu, pipeline.get(), /*self_id=*/1, &remotes);

    constexpr int N_THREADS  = 8;
    constexpr int PER_THREAD = 10000;

    std::vector<std::thread> threads;
    std::atomic<int> successes{0};
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                // Cycle through 5000 symbols so all 3 nodes get hit.
                zeptodb::SymbolId s =
                    static_cast<zeptodb::SymbolId>((t * PER_THREAD + i) % 5000 + 1);
                if (adapter.ingest_tick(make_tick(s))) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    const int total_remote =
        stub2->calls.load() + stub3->calls.load();
    const int total_local =
        static_cast<int>(pipeline->stats().ticks_ingested.load());

    EXPECT_EQ(successes.load(), N_THREADS * PER_THREAD);
    EXPECT_EQ(total_local + total_remote, N_THREADS * PER_THREAD);

    // Sanity: each of the three buckets received at least some traffic
    // (otherwise the test isn't exercising the branch it claims to).
    EXPECT_GT(total_local, 0);
    EXPECT_GT(stub2->calls.load(), 0);
    EXPECT_GT(stub3->calls.load(), 0);
}

// ----------------------------------------------------------------------------
// Case 5: ingest-node invariant — self NOT in ring → zero local ticks
// Mirrors the zepto_ingest_node wire-up: add_local_node then remove_node(self)
// so route() never returns self_id. Every tick must go to a remote stub.
// ----------------------------------------------------------------------------
TEST(CoordinatorRoutingAdapter, IngestNodeInvariant_ZeroLocalTicks) {
    // Simulate the ingest-node wire-up: coordinator with self removed from ring
    zeptodb::cluster::PartitionRouter router;
    std::shared_mutex router_mu;
    // Add self + 2 storage nodes, then remove self
    router.add_node(65534);  // self (ingest node)
    router.add_node(1);      // storage pod 1
    router.add_node(2);      // storage pod 2
    router.remove_node(65534);  // ← the critical step

    auto pipeline = make_pipeline();
    auto stub1 = std::make_shared<CountingRpcClient>();
    auto stub2 = std::make_shared<CountingRpcClient>();
    zeptodb::cluster::CoordinatorRoutingAdapter::RpcClientMap remotes;
    remotes.emplace(1, stub1);
    remotes.emplace(2, stub2);

    zeptodb::cluster::CoordinatorRoutingAdapter adapter(
        &router, &router_mu, pipeline.get(), /*self_id=*/65534, &remotes);

    // Drive 10K ticks across many symbols
    int successes = 0;
    for (int i = 0; i < 10000; ++i) {
        auto sym = static_cast<zeptodb::SymbolId>(i % 5000 + 1);
        if (adapter.ingest_tick(make_tick(sym))) ++successes;
    }

    EXPECT_EQ(successes, 10000);
    // The invariant: ZERO ticks in local pipeline
    EXPECT_EQ(pipeline->stats().ticks_ingested.load(), 0u)
        << "Ingest node must forward ALL ticks to storage pods; "
           "self must not be in the hash ring";
    // All ticks went to the two stubs
    EXPECT_EQ(stub1->calls.load() + stub2->calls.load(), 10000);
}
