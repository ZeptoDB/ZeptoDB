// ============================================================================
// zepto_data_node — standalone data node for multi-process testing
// ============================================================================
// Usage: ./zepto_data_node <port> [num_ticks]
//   Starts a TcpRpcServer on <port>, ingests num_ticks (default 1000),
//   then serves SQL queries until killed.
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <thread>

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port> [num_ticks] [symbol_id]\n";
        return 1;
    }

    uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
    int num_ticks = (argc >= 3) ? std::atoi(argv[2]) : 1000;
    uint32_t symbol = (argc >= 4) ? static_cast<uint32_t>(std::atoi(argv[3])) : 1;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create pipeline
    zeptodb::core::PipelineConfig cfg;
    cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(cfg);

    // Ingest test data
    for (int i = 0; i < num_ticks; ++i) {
        zeptodb::ingestion::TickMessage msg{};
        msg.symbol_id = symbol;
        msg.price     = (10000 + i) * 1'000'000LL;
        msg.volume    = 100;
        msg.recv_ts   = static_cast<int64_t>(i) * 1'000'000'000LL;
        pipeline.ingest_tick(msg);
    }
    pipeline.drain_sync(static_cast<size_t>(num_ticks) + 100);

    std::cout << "Data node ready: port=" << port
              << " ticks=" << num_ticks
              << " symbol=" << symbol << std::endl;

    // Start RPC server
    zeptodb::cluster::TcpRpcServer srv;
    srv.start(port, [&](const std::string& sql) {
        zeptodb::sql::QueryExecutor ex(pipeline);
        return ex.execute(sql);
    });

    // Wait for signal
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    srv.stop();
    std::cout << "Data node stopped." << std::endl;
    return 0;
}
