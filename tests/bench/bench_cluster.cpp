// ============================================================================
// Phase C 벤치마크: Cluster 컴포넌트 성능 측정
// ============================================================================
// 1. SharedMem transport latency (baseline)
// 2. Partition routing throughput (lookups/sec)
// 3. 2-노드 ingest+query vs single-node
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

#include "zeptodb/cluster/transport.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/health_monitor.h"
#include "zeptodb/cluster/cluster_node.h"
#include "zeptodb/auth/license_validator.h"
#include "shm_backend.h"

using namespace zeptodb;
using namespace zeptodb::cluster;
using namespace zeptodb::core;
using namespace std::chrono;

// ============================================================================
// 유틸리티
// ============================================================================
struct BenchResult {
    std::string name;
    double      ops_per_sec;
    double      latency_ns;
    uint64_t    total_ops;
    double      duration_sec;
};

void print_result(const BenchResult& r) {
    std::cout << std::left  << std::setw(45) << r.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(0)
              << r.ops_per_sec << " ops/s  "
              << std::setw(10) << std::setprecision(1) << r.latency_ns << " ns/op"
              << std::endl;
}

// ============================================================================
// 벤치마크 1: SharedMem Transport 레이턴시
// ============================================================================
BenchResult bench_shm_write_latency() {
    SharedMemBackend writer, reader;
    NodeAddress addr1{"127.0.0.1", 9001, 1};
    NodeAddress addr2{"127.0.0.1", 9002, 2};

    writer.do_init(addr1);
    reader.do_init(addr2);

    constexpr size_t BUF_SIZE = 4096;
    alignas(64) uint8_t local_buf[BUF_SIZE] = {};
    alignas(64) uint8_t src_buf[64] = {};

    RemoteRegion region = writer.do_register_memory(local_buf, BUF_SIZE);

    constexpr int WARMUP   = 10000;
    constexpr int ITERS    = 500000;

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        reader.do_remote_write(src_buf, region, 0, 64);
    }

    // 측정
    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        reader.do_remote_write(src_buf, region, 0, 64);
        reader.do_fence();
    }
    auto t1 = high_resolution_clock::now();

    double dur_ns  = duration_cast<nanoseconds>(t1 - t0).count();
    double lat_ns  = dur_ns / ITERS;
    double ops_sec = ITERS / (dur_ns / 1e9);

    writer.do_deregister_memory(region);
    writer.do_shutdown();
    reader.do_shutdown();

    return {"SharedMem write+fence latency (64B)", ops_sec, lat_ns, ITERS,
            dur_ns / 1e9};
}

BenchResult bench_shm_read_latency() {
    SharedMemBackend writer, reader;
    NodeAddress addr1{"127.0.0.1", 9003, 1};
    NodeAddress addr2{"127.0.0.1", 9004, 2};

    writer.do_init(addr1);
    reader.do_init(addr2);

    constexpr size_t BUF_SIZE = 4096;
    alignas(64) uint8_t local_buf[BUF_SIZE] = {};
    alignas(64) uint8_t dst_buf[64] = {};

    RemoteRegion region = writer.do_register_memory(local_buf, BUF_SIZE);

    constexpr int ITERS = 500000;

    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        reader.do_remote_read(region, 0, dst_buf, 64);
    }
    auto t1 = high_resolution_clock::now();

    double dur_ns = duration_cast<nanoseconds>(t1 - t0).count();

    writer.do_deregister_memory(region);
    writer.do_shutdown();
    reader.do_shutdown();

    return {"SharedMem read latency (64B)", ITERS / (dur_ns / 1e9),
            dur_ns / ITERS, ITERS, dur_ns / 1e9};
}

BenchResult bench_shm_bulk_write() {
    SharedMemBackend writer, reader;
    NodeAddress addr1{"127.0.0.1", 9005, 1};
    NodeAddress addr2{"127.0.0.1", 9006, 2};

    writer.do_init(addr1);
    reader.do_init(addr2);

    constexpr size_t BUF_SIZE  = 1024 * 1024;  // 1MB
    constexpr size_t CHUNK     = 4096;          // 4KB per op
    std::vector<uint8_t> local_buf(BUF_SIZE, 0);
    std::vector<uint8_t> src_chunk(CHUNK, 0xAB);

    RemoteRegion region = writer.do_register_memory(local_buf.data(), BUF_SIZE);

    constexpr int ITERS = 100000;

    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        size_t offset = (i * CHUNK) % (BUF_SIZE - CHUNK);
        reader.do_remote_write(src_chunk.data(), region, offset, CHUNK);
    }
    reader.do_fence();
    auto t1 = high_resolution_clock::now();

    double dur_ns  = duration_cast<nanoseconds>(t1 - t0).count();
    double gb_sec  = (static_cast<double>(ITERS) * CHUNK) / (dur_ns / 1e9) / 1e9;
    double ops_sec = ITERS / (dur_ns / 1e9);

    std::cout << "  SharedMem bulk write throughput: "
              << std::fixed << std::setprecision(2) << gb_sec << " GB/s" << std::endl;

    writer.do_deregister_memory(region);
    writer.do_shutdown();
    reader.do_shutdown();

    return {"SharedMem bulk write (4KB chunks)", ops_sec,
            dur_ns / ITERS, ITERS, dur_ns / 1e9};
}

// ============================================================================
// 벤치마크 2: PartitionRouter 라우팅 처리량
// ============================================================================
BenchResult bench_routing_throughput() {
    PartitionRouter router;
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);
    router.add_node(4);

    constexpr int ITERS = 10000000;  // 10M

    // 캐시 워밍
    for (zeptodb::SymbolId s = 0; s < 1000; ++s) router.route(s);

    volatile NodeId sink = 0;  // 최적화 방지
    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        sink = router.route(static_cast<zeptodb::SymbolId>(i % 10000));
    }
    auto t1 = high_resolution_clock::now();
    (void)sink;

    double dur_ns = duration_cast<nanoseconds>(t1 - t0).count();
    return {"PartitionRouter route() throughput", ITERS / (dur_ns / 1e9),
            dur_ns / ITERS, ITERS, dur_ns / 1e9};
}

BenchResult bench_routing_uncached() {
    PartitionRouter router;
    for (NodeId n = 1; n <= 8; ++n) router.add_node(n);

    constexpr int ITERS = 1000000;

    volatile NodeId sink = 0;
    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        // 캐시 초과하는 심볼 ID 사용
        sink = router.route(static_cast<zeptodb::SymbolId>(100000 + i));
    }
    auto t1 = high_resolution_clock::now();
    (void)sink;

    double dur_ns = duration_cast<nanoseconds>(t1 - t0).count();
    return {"PartitionRouter route() uncached", ITERS / (dur_ns / 1e9),
            dur_ns / ITERS, ITERS, dur_ns / 1e9};
}

// ============================================================================
// 벤치마크 3: 2-노드 vs 단일 노드 Ingest+Query
// ============================================================================
BenchResult bench_single_node_ingest() {
    using ShmNode = ClusterNode<SharedMemBackend>;

    ClusterConfig cfg;
    cfg.self = {"127.0.0.1", 9010, 10};
    cfg.pipeline.storage_mode = StorageMode::PURE_IN_MEMORY;
    cfg.enable_remote_ingest = false;

    // ClusterNode ~8MB: 힙에 할당
    auto node = std::make_unique<ShmNode>(cfg);
    node->join_cluster();

    constexpr int WARMUP = 1000;
    constexpr int ITERS  = 100000;

    ingestion::TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 15000000;
    msg.volume    = 100;
    msg.recv_ts = 1'000'000'000LL;

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        msg.seq_num = i;
        node->ingest_local(msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        msg.seq_num   = WARMUP + i;
        msg.recv_ts   = 1'000'000'000LL + i * 1000000LL;
        node->ingest_local(msg);
    }
    auto t1 = high_resolution_clock::now();

    double dur_ns = duration_cast<nanoseconds>(t1 - t0).count();

    node->leave_cluster();

    return {"Single-node ingest throughput", ITERS / (dur_ns / 1e9),
            dur_ns / ITERS, ITERS, dur_ns / 1e9};
}

BenchResult bench_routing_decision() {
    PartitionRouter router;
    for (NodeId n = 1; n <= 4; ++n) router.add_node(n);

    constexpr int ITERS = 5000000;

    // 라우팅 결정 오버헤드 단독 측정
    std::vector<zeptodb::SymbolId> symbols(1000);
    for (size_t i = 0; i < symbols.size(); ++i) symbols[i] = static_cast<zeptodb::SymbolId>(i);

    volatile NodeId sink = 0;
    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        sink = router.route(symbols[i % symbols.size()]);
    }
    auto t1 = high_resolution_clock::now();
    (void)sink;

    double dur_ns = duration_cast<nanoseconds>(t1 - t0).count();
    return {"Routing decision overhead (cached)", ITERS / (dur_ns / 1e9),
            dur_ns / ITERS, ITERS, dur_ns / 1e9};
}

// ============================================================================
// Main
// ============================================================================
int main() {
    // Load Enterprise license from env (ZEPTODB_LICENSE_KEY) or /etc/zeptodb/license.key.
    // Required for cluster ingest benchmarks; transport/routing benches work without it.
    zeptodb::auth::license().load();

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║      ZeptoDB Phase C Cluster Benchmark               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n\n";

    std::vector<BenchResult> results;

    // ────────────────────────────────────────────────────
    std::cout << "[ 1. SharedMem Transport Latency ]\n";
    results.push_back(bench_shm_write_latency());
    print_result(results.back());

    results.push_back(bench_shm_read_latency());
    print_result(results.back());

    results.push_back(bench_shm_bulk_write());
    print_result(results.back());

    std::cout << "\n[ 2. Partition Routing Throughput ]\n";
    results.push_back(bench_routing_throughput());
    print_result(results.back());

    results.push_back(bench_routing_uncached());
    print_result(results.back());

    results.push_back(bench_routing_decision());
    print_result(results.back());

    std::cout << "\n[ 3. Cluster Ingest Performance ]\n";
    results.push_back(bench_single_node_ingest());
    print_result(results.back());

    std::cout << "\n[ Summary ]\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left  << std::setw(45) << "Benchmark"
              << std::right << std::setw(15) << "Throughput"
              << std::setw(12) << "Latency" << "\n";
    std::cout << std::string(80, '-') << "\n";
    for (auto& r : results) {
        std::cout << std::left  << std::setw(45) << r.name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(0)
                  << r.ops_per_sec << " ops/s  "
                  << std::setw(8) << std::setprecision(1)
                  << r.latency_ns << " ns\n";
    }
    std::cout << std::string(80, '-') << "\n\n";

    return 0;
}
