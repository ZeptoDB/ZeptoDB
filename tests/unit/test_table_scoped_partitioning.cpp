// ============================================================================
// Test: Table-Scoped Partitioning (P7 — devlog 053)
// ----------------------------------------------------------------------------
// Verifies that partitions are isolated per table_id so that
// `SELECT * FROM empty_table` never returns rows from other tables.
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <gtest/gtest.h>

using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::ingestion::TickMessage;
using zeptodb::sql::QueryExecutor;
using zeptodb::storage::PartitionKey;
using zeptodb::storage::PartitionKeyHash;

namespace {

std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

constexpr const char* TBL_DDL =
    "(symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)";

}  // namespace

// Compile-time: TickMessage must remain 64 bytes (cache line) after adding table_id.
TEST(TableScopedPartitioning, TickMessageSize64) {
    static_assert(sizeof(TickMessage) == 64, "TickMessage must remain 64 bytes");
    EXPECT_EQ(sizeof(TickMessage), 64u);
}

TEST(TableScopedPartitioning, PartitionKeyHashDistinct) {
    PartitionKey k1{1, 42, 3600'000'000'000LL};
    PartitionKey k2{2, 42, 3600'000'000'000LL};  // same (symbol, hour), different table
    EXPECT_FALSE(k1 == k2);
    PartitionKeyHash h;
    EXPECT_NE(h(k1), h(k2));
}

TEST(TableScopedPartitioning, CreateTableAssignsUniqueTableId) {
    auto pipeline = make_pipeline();
    QueryExecutor ex(*pipeline);
    ex.execute(std::string("CREATE TABLE t1 ") + TBL_DDL);
    ex.execute(std::string("CREATE TABLE t2 ") + TBL_DDL);
    ex.execute(std::string("CREATE TABLE t3 ") + TBL_DDL);
    auto& reg = pipeline->schema_registry();
    const uint16_t id1 = reg.get_table_id("t1");
    const uint16_t id2 = reg.get_table_id("t2");
    const uint16_t id3 = reg.get_table_id("t3");
    EXPECT_NE(id1, 0);
    EXPECT_NE(id2, 0);
    EXPECT_NE(id3, 0);
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    EXPECT_NE(id1, id3);
}

TEST(TableScopedPartitioning, InsertIntoTableSetsMsgTableId) {
    auto pipeline = make_pipeline();
    QueryExecutor ex(*pipeline);
    ex.execute(std::string("CREATE TABLE trades ") + TBL_DDL);
    const uint16_t tid = pipeline->schema_registry().get_table_id("trades");
    ASSERT_NE(tid, 0);

    auto r = ex.execute("INSERT INTO trades VALUES (1, 15000, 100, 1000)");
    ASSERT_TRUE(r.ok()) << r.error;

    const auto parts = pipeline->partition_manager().get_partitions_for_table(tid);
    EXPECT_GE(parts.size(), 1u);
}

// The actual bug fix: SELECT * FROM empty_table must NOT return other tables' data.
TEST(TableScopedPartitioning, EmptyTableReturnsZeroRows) {
    auto pipeline = make_pipeline();
    QueryExecutor ex(*pipeline);
    ex.execute(std::string("CREATE TABLE a ") + TBL_DDL);
    ex.execute(std::string("CREATE TABLE b ") + TBL_DDL);
    ex.execute("INSERT INTO a VALUES (1, 10, 100, 1000)");
    ex.execute("INSERT INTO a VALUES (2, 20, 200, 2000)");

    auto r = ex.execute("SELECT count(*) FROM b");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 0);

    auto r2 = ex.execute("SELECT count(*) FROM a");
    ASSERT_TRUE(r2.ok()) << r2.error;
    EXPECT_EQ(r2.rows[0][0], 2);
}

TEST(TableScopedPartitioning, TwoTablesIsolated) {
    auto pipeline = make_pipeline();
    QueryExecutor ex(*pipeline);
    ex.execute(std::string("CREATE TABLE t1 ") + TBL_DDL);
    ex.execute(std::string("CREATE TABLE t2 ") + TBL_DDL);

    ex.execute("INSERT INTO t1 VALUES (1, 100, 10, 1000)");
    ex.execute("INSERT INTO t1 VALUES (1, 101, 11, 1001)");
    ex.execute("INSERT INTO t2 VALUES (1, 500, 50, 2000)");

    auto r1 = ex.execute("SELECT sum(price) FROM t1");
    ASSERT_TRUE(r1.ok()) << r1.error;
    EXPECT_EQ(r1.rows[0][0], 201);  // 100 + 101, NOT including 500 from t2

    auto r2 = ex.execute("SELECT sum(price) FROM t2");
    ASSERT_TRUE(r2.ok()) << r2.error;
    EXPECT_EQ(r2.rows[0][0], 500);
}

// Backward-compat: direct ingest_tick with table_id=0 still routes & queries.
TEST(TableScopedPartitioning, LegacyPathTableIdZero) {
    auto pipeline = make_pipeline();
    TickMessage msg{};
    msg.symbol_id = 7;
    msg.price     = 42;
    msg.volume    = 1;
    msg.recv_ts   = 1000;
    msg.table_id  = 0;  // legacy
    ASSERT_TRUE(pipeline->ingest_tick(msg));
    pipeline->drain_sync(10);

    // Legacy partitions live under table_id=0
    const auto parts = pipeline->partition_manager().get_partitions_for_table(0);
    EXPECT_GE(parts.size(), 1u);

    // Programmatic query API uses the legacy overload (table_id=0)
    auto r = pipeline->query_count(7);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.ivalue, 1);
}

TEST(TableScopedPartitioning, DropTableReleasesPartitions) {
    auto pipeline = make_pipeline();
    QueryExecutor ex(*pipeline);
    ex.execute(std::string("CREATE TABLE a ") + TBL_DDL);
    ex.execute(std::string("CREATE TABLE b ") + TBL_DDL);
    const uint16_t a_tid = pipeline->schema_registry().get_table_id("a");
    ASSERT_NE(a_tid, 0);

    ASSERT_TRUE(ex.execute("INSERT INTO a VALUES (1, 10, 100, 1000)").ok());
    ASSERT_TRUE(ex.execute("INSERT INTO b VALUES (1, 20, 200, 2000)").ok());

    const size_t a_parts_before = pipeline->partition_manager()
        .get_partitions_for_table(a_tid).size();
    ASSERT_GE(a_parts_before, 1u);
    const size_t total_before = pipeline->partition_manager().partition_count();

    auto dr = ex.execute("DROP TABLE a");
    ASSERT_TRUE(dr.ok()) << dr.error;

    // Partition count decreased by a's partitions
    EXPECT_EQ(pipeline->partition_manager().partition_count(),
              total_before - a_parts_before);
    // a's partitions are gone
    EXPECT_EQ(pipeline->partition_manager().get_partitions_for_table(a_tid).size(),
              0u);
    // b still intact
    auto rb = ex.execute("SELECT count(*) FROM b");
    ASSERT_TRUE(rb.ok()) << rb.error;
    EXPECT_EQ(rb.rows[0][0], 1);
}

// ============================================================================
// Stage A: HDB-level isolation + SchemaRegistry JSON durability
// ============================================================================
#include "zeptodb/storage/hdb_writer.h"
#include "zeptodb/storage/hdb_reader.h"
#include "zeptodb/storage/schema_registry.h"
#include <filesystem>
#include <cstring>
#include <fstream>
#include <chrono>
#include <unistd.h>

namespace fs_tsp = std::filesystem;
using zeptodb::storage::ArenaAllocator;
using zeptodb::storage::ArenaConfig;
using zeptodb::storage::ColumnType;
using zeptodb::storage::HDBReader;
using zeptodb::storage::HDBWriter;
using zeptodb::storage::Partition;
using zeptodb::storage::SchemaRegistry;

// Two tables, same (symbol, hour) → flushed to different dirs; mmap read back
// returns each table's own rows only.
TEST(TableScopedPartitioning, HDBFlushIsolatedPerTable) {
    const auto tmp = fs_tsp::temp_directory_path() /
        ("zepto_tsp_isolated_" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()));
    fs_tsp::create_directories(tmp);

    using namespace zeptodb::storage;
    HDBWriter writer(tmp.string(), /*compression=*/false);

    const zeptodb::SymbolId sym = 42;
    const int64_t hour = 3600LL * 1'000'000'000LL;

    auto make_part = [&](uint16_t tid, int64_t base_px, size_t n) {
        auto arena = std::make_unique<ArenaAllocator>(ArenaConfig{
            .total_size = 1ULL * 1024 * 1024, .use_hugepages = false});
        PartitionKey k{tid, sym, hour};
        auto p = std::make_unique<Partition>(k, std::move(arena));
        p->add_column("price", ColumnType::INT64);
        for (size_t i = 0; i < n; ++i)
            p->get_column("price")->append<int64_t>(base_px + static_cast<int64_t>(i));
        p->seal();
        return p;
    };

    auto p1 = make_part(/*tid=*/1, /*base=*/100, /*n=*/10);
    auto p2 = make_part(/*tid=*/2, /*base=*/900, /*n=*/10);
    ASSERT_GT(writer.flush_partition(*p1), 0u);
    ASSERT_GT(writer.flush_partition(*p2), 0u);

    // Different on-disk dirs
    EXPECT_TRUE(fs_tsp::exists(tmp / "t1" / std::to_string(sym) / std::to_string(hour) / "price.bin"));
    EXPECT_TRUE(fs_tsp::exists(tmp / "t2" / std::to_string(sym) / std::to_string(hour) / "price.bin"));

    HDBReader reader(tmp.string());
    auto c1 = reader.read_column(/*tid=*/1, sym, hour, "price");
    auto c2 = reader.read_column(/*tid=*/2, sym, hour, "price");
    ASSERT_TRUE(c1.valid());
    ASSERT_TRUE(c2.valid());
    auto s1 = c1.as_span<int64_t>();
    auto s2 = c2.as_span<int64_t>();
    EXPECT_EQ(s1[0], 100);
    EXPECT_EQ(s2[0], 900);
    EXPECT_EQ(s1[9], 109);
    EXPECT_EQ(s2[9], 909);

    std::error_code ec;
    fs_tsp::remove_all(tmp, ec);
}

TEST(TableScopedPartitioning, SchemaRegistryPersistsAcrossRestart) {
    const auto tmp = fs_tsp::temp_directory_path() /
        ("zepto_tsp_registry_" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()));
    fs_tsp::create_directories(tmp);
    const std::string path = (tmp / "_schema.json").string();

    using namespace zeptodb::storage;
    {
        SchemaRegistry r;
        ASSERT_TRUE(r.create("t_a", {{"x", ColumnType::INT64}}));
        ASSERT_TRUE(r.create("t_b", {{"y", ColumnType::INT64}, {"z", ColumnType::INT32}}));
        ASSERT_TRUE(r.set_ttl("t_b", 3600LL * 1'000'000'000LL));
        r.mark_has_data("t_a");
        ASSERT_TRUE(r.save_to(path));
    }
    {
        SchemaRegistry r2;
        ASSERT_TRUE(r2.load_from(path));
        EXPECT_EQ(r2.table_count(), 2u);
        const auto a = r2.get("t_a");
        const auto b = r2.get("t_b");
        ASSERT_TRUE(a.has_value());
        ASSERT_TRUE(b.has_value());
        EXPECT_NE(a->table_id, 0);
        EXPECT_NE(b->table_id, 0);
        EXPECT_NE(a->table_id, b->table_id);
        EXPECT_TRUE(a->has_data);
        EXPECT_FALSE(b->has_data);
        EXPECT_EQ(b->ttl_ns, 3600LL * 1'000'000'000LL);
        EXPECT_EQ(b->columns.size(), 2u);
        EXPECT_EQ(b->columns[0].name, "y");
        // next_table_id_ must be > max(loaded table_id); creating a new one
        // must not collide with persisted ids.
        ASSERT_TRUE(r2.create("t_c", {{"w", ColumnType::INT64}}));
        const uint16_t c_id = r2.get_table_id("t_c");
        EXPECT_GT(c_id, a->table_id);
        EXPECT_GT(c_id, b->table_id);
    }

    std::error_code ec;
    fs_tsp::remove_all(tmp, ec);
}

TEST(HDBFileHeader, V1BackwardCompatibleRead) {
    const auto tmp = fs_tsp::temp_directory_path() /
        ("zepto_tsp_v1_" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()));
    fs_tsp::create_directories(tmp);

    // Craft a 32-byte v1 header + a small INT64 payload manually.
    const zeptodb::SymbolId sym = 7;
    const int64_t hour = 3600LL * 1'000'000'000LL;
    const std::string dir = (tmp / std::to_string(sym) / std::to_string(hour)).string();
    fs_tsp::create_directories(dir);
    const std::string file_path = dir + "/price.bin";

    const int64_t rows[] = {111, 222, 333};
    constexpr size_t N = sizeof(rows) / sizeof(rows[0]);

    std::ofstream out(file_path, std::ios::binary);
    ASSERT_TRUE(out.good());
    // v1 layout: magic[5] + version + col_type + compression + row_count(u64)
    //            + data_size(u64) + uncompressed_size(u64) = 32 bytes
    out.write("APEXH", 5);
    uint8_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), 1);
    uint8_t col_type = static_cast<uint8_t>(ColumnType::INT64);
    out.write(reinterpret_cast<const char*>(&col_type), 1);
    uint8_t compression = 0;
    out.write(reinterpret_cast<const char*>(&compression), 1);
    uint64_t row_count = N, data_size = sizeof(rows), uncomp = sizeof(rows);
    out.write(reinterpret_cast<const char*>(&row_count), 8);
    out.write(reinterpret_cast<const char*>(&data_size), 8);
    out.write(reinterpret_cast<const char*>(&uncomp), 8);
    out.write(reinterpret_cast<const char*>(rows), sizeof(rows));
    out.close();

    ASSERT_EQ(fs_tsp::file_size(file_path), 32u + sizeof(rows));

    HDBReader reader(tmp.string());
    auto col = reader.read_column(sym, hour, "price");  // legacy overload: table_id=0
    ASSERT_TRUE(col.valid());
    EXPECT_EQ(col.num_rows, N);
    EXPECT_EQ(col.type, ColumnType::INT64);
    auto sp = col.as_span<int64_t>();
    EXPECT_EQ(sp[0], 111);
    EXPECT_EQ(sp[1], 222);
    EXPECT_EQ(sp[2], 333);

    std::error_code ec;
    fs_tsp::remove_all(tmp, ec);
}

// ============================================================================
// Stage C: Strict SQL fallback + ClusterNode table-aware routing
// ============================================================================
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/query_coordinator.h"

// Strict fallback: once any CREATE TABLE has run, SELECT from an unknown
// table must return 0 rows — NOT the legacy "all partitions" fallback that
// would leak rows from other tables.
TEST(TableScopedPartitioning, StrictFallbackUnknownTableReturnsEmpty) {
    auto pipeline = make_pipeline();
    QueryExecutor ex(*pipeline);
    ex.execute(std::string("CREATE TABLE a ") + TBL_DDL);
    ex.execute("INSERT INTO a VALUES (1, 100, 10, 1000)");
    ex.execute("INSERT INTO a VALUES (2, 200, 20, 2000)");

    // Sanity: `a` has data.
    auto ra = ex.execute("SELECT count(*) FROM a");
    ASSERT_TRUE(ra.ok()) << ra.error;
    EXPECT_EQ(ra.rows[0][0], 2);

    // Unknown table name must yield an empty result set, not a's rows.
    // The parser/planner may surface this as either ok-with-empty or an error;
    // both are acceptable — the key invariant is "no leaked rows".
    auto r = ex.execute("SELECT * FROM bogus");
    if (r.ok()) {
        EXPECT_EQ(r.rows.size(), 0u);
    } else {
        EXPECT_FALSE(r.error.empty());
    }

    auto r2 = ex.execute("SELECT count(*) FROM bogus");
    if (r2.ok()) {
        ASSERT_EQ(r2.rows.size(), 1u);
        EXPECT_EQ(r2.rows[0][0], 0);
    } else {
        EXPECT_FALSE(r2.error.empty());
    }
}

// Legacy-mode sanity: if no CREATE TABLE has ever run, an unknown table name
// still falls back to all-partitions (pre-082 behavior for programmatic ingest).
TEST(TableScopedPartitioning, LegacyFallbackWithNoCreateTable) {
    auto pipeline = make_pipeline();
    TickMessage msg{};
    msg.symbol_id = 1; msg.price = 100; msg.volume = 10;
    msg.recv_ts = 1000; msg.table_id = 0;
    ASSERT_TRUE(pipeline->ingest_tick(msg));
    pipeline->drain_sync(10);

    QueryExecutor ex(*pipeline);
    auto r = ex.execute("SELECT count(*) FROM trades");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows[0][0], 1);  // legacy fallback returns the row
}

// C1: PartitionRouter(table_id, symbol) overload hashes (table, sym) together
// so two different tables with the same symbol_id can land on different nodes.
TEST(ClusterRouting, TableAwareRouteOverload) {
    using zeptodb::cluster::PartitionRouter;
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);
    router.add_node(4);

    // At least one symbol must hash to a different owner when table_id differs.
    bool found_difference = false;
    for (zeptodb::SymbolId s = 1; s <= 256; ++s) {
        if (router.route(/*tid=*/1, s) != router.route(/*tid=*/2, s)) {
            found_difference = true;
            break;
        }
    }
    EXPECT_TRUE(found_difference)
        << "Expected at least one symbol to hash differently across tables";

    // Backward compat: route(sym) == route(0, sym).
    for (zeptodb::SymbolId s = 1; s <= 16; ++s) {
        EXPECT_EQ(router.route(s), router.route(/*tid=*/0, s));
    }
}

// C1: Scatter-gather SELECT across two pipelines that loaded the same
// _schema.json.  Each node resolves FROM <table> locally via its own
// SchemaRegistry — this is the "durable _schema.json lets nodes stay in sync
// automatically" invariant that makes table-aware distributed SELECT work
// without any RPC changes.
TEST(ClusterRouting, ScatterGatherWithDurableSchemaRegistry) {
    using namespace zeptodb::cluster;
    namespace fs_c = std::filesystem;

    const auto tmp = fs_c::temp_directory_path() /
        ("zepto_tsp_scatter_" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()));
    fs_c::create_directories(tmp);
    const std::string schema_path = (tmp / "_schema.json").string();

    {
        zeptodb::storage::SchemaRegistry r;
        ASSERT_TRUE(r.create("orders", {
            {"symbol", zeptodb::storage::ColumnType::INT64},
            {"price",  zeptodb::storage::ColumnType::INT64},
            {"volume", zeptodb::storage::ColumnType::INT64}
        }));
        ASSERT_TRUE(r.save_to(schema_path));
    }

    auto p1 = make_pipeline();
    auto p2 = make_pipeline();

    // Both nodes load the same schema — same tid for "orders" everywhere.
    ASSERT_TRUE(p1->schema_registry().load_from(schema_path));
    ASSERT_TRUE(p2->schema_registry().load_from(schema_path));
    const uint16_t tid1 = p1->schema_registry().get_table_id("orders");
    const uint16_t tid2 = p2->schema_registry().get_table_id("orders");
    ASSERT_NE(tid1, 0);
    EXPECT_EQ(tid1, tid2);

    auto insert = [&](zeptodb::core::ZeptoPipeline& p, zeptodb::SymbolId sym,
                      int64_t px, int count) {
        for (int i = 0; i < count; ++i) {
            TickMessage m{};
            m.symbol_id = sym;
            m.price     = px;
            m.volume    = 1;
            m.recv_ts   = static_cast<int64_t>(i + 1) * 1'000'000'000LL;
            m.table_id  = tid1;
            ASSERT_TRUE(p.ingest_tick(m));
        }
        p.drain_sync(50);
    };
    insert(*p1, /*sym=*/10, 100, 3);
    insert(*p2, /*sym=*/20, 200, 5);

    QueryCoordinator coord;
    coord.add_local_node({"127.0.0.1", 0, 1}, *p1);
    coord.add_local_node({"127.0.0.1", 0, 2}, *p2);

    // SELECT FROM orders — each node resolves tid locally from its own registry.
    auto r = coord.execute_sql("SELECT count(*) FROM orders");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0][0], 8);  // 3 + 5 — both nodes routed FROM orders

    // Unknown table on both nodes → 0 rows (strict fallback via scatter).
    auto r_unknown = coord.execute_sql("SELECT count(*) FROM bogus_scatter");
    if (r_unknown.ok()) {
        ASSERT_EQ(r_unknown.rows.size(), 1u);
        EXPECT_EQ(r_unknown.rows[0][0], 0);
    } else {
        EXPECT_FALSE(r_unknown.error.empty());
    }

    std::error_code ec;
    fs_c::remove_all(tmp, ec);
}


// ============================================================================
// devlog 086 (D1): SchemaRegistry::save_to concurrent safety
// ----------------------------------------------------------------------------
// Before: `save_to` took a shared_lock and wrote to a fixed `.tmp` suffix.
// Two concurrent DDL callers could race on the tmp file / rename.
// After: unique_lock serialises saves, and the tmp suffix is per-(pid,tid)
// so cross-process writers never collide.
// ============================================================================
#include <atomic>
#include <thread>

TEST(TableScopedPartitioning, SchemaRegistrySaveConcurrentSafe) {
    namespace fs_d = std::filesystem;
    auto tmp = fs_d::temp_directory_path() /
        ("zepto_sr_concurrent_" +
         std::to_string(static_cast<unsigned long>(::getpid())) + "_" +
         std::to_string(static_cast<unsigned long long>(
             std::chrono::steady_clock::now().time_since_epoch().count())));
    fs_d::create_directories(tmp);
    const std::string path = (tmp / "_schema.json").string();

    zeptodb::storage::SchemaRegistry reg;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10;

    std::atomic<int> save_fails{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                std::string name = "t_" + std::to_string(t) + "_" + std::to_string(i);
                reg.create(name, {});
                if (!reg.save_to(path)) save_fails.fetch_add(1);
            }
        });
    }
    for (auto& w : workers) w.join();

    // No save should have failed outright (tmp-collision used to corrupt).
    EXPECT_EQ(save_fails.load(), 0);

    // Final load must parse and contain all kThreads * kPerThread tables.
    zeptodb::storage::SchemaRegistry reloaded;
    ASSERT_TRUE(reloaded.load_from(path));
    EXPECT_EQ(reloaded.table_count(),
              static_cast<size_t>(kThreads * kPerThread));

    std::error_code ec;
    fs_d::remove_all(tmp, ec);
}
