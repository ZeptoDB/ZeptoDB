// ============================================================================
// zepto_flight_server — Arrow Flight RPC server
// ============================================================================
// Starts an Arrow Flight server alongside the HTTP server.
// Python clients: pyarrow.flight.connect("grpc://localhost:8815")
// ============================================================================
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/flight_server.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    uint16_t http_port   = 8123;
    uint16_t flight_port = 8815;
    int num_ticks = 10000;
    bool no_auth = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--http-port" && i + 1 < argc)
            http_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--flight-port" && i + 1 < argc)
            flight_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if ((arg == "--ticks" || arg == "-n") && i + 1 < argc)
            num_ticks = std::atoi(argv[++i]);
        else if (arg == "--no-auth")
            no_auth = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--http-port 8123] [--flight-port 8815] [--ticks 10000] [--no-auth]\n\n"
                      << "Starts both HTTP (ClickHouse compat) and Arrow Flight servers.\n"
                      << "Python: pyarrow.flight.connect('grpc://localhost:8815')\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Pipeline
    zeptodb::core::PipelineConfig cfg;
    cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(cfg);

    zeptodb::sql::QueryExecutor bootstrap_ex(pipeline);
    bootstrap_ex.execute("CREATE TABLE IF NOT EXISTS trades "
                         "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)");

    // Seed sample data
    for (int i = 0; i < num_ticks; ++i) {
        zeptodb::ingestion::TickMessage msg{};
        msg.symbol_id = static_cast<uint32_t>(1 + (i % 3));
        msg.price     = (15000 + (i % 200)) * 1'000'000LL;
        msg.volume    = 100 + (i % 50);
        msg.recv_ts   = 1711234567'000'000'000LL + static_cast<int64_t>(i) * 1'000'000LL;
        pipeline.ingest_tick(msg);
    }
    pipeline.drain_sync(static_cast<size_t>(num_ticks) + 100);

    zeptodb::sql::QueryExecutor executor(pipeline);

    // HTTP server (optional, for Web UI / REST)
    zeptodb::auth::AuthManager::Config auth_cfg;
    auth_cfg.enabled = !no_auth;
    auth_cfg.api_keys_file = "dev_keys.txt";
    auth_cfg.rate_limit_enabled = false;
    auth_cfg.audit_enabled = false;
    auth_cfg.audit_buffer_enabled = false;
    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);
    zeptodb::server::HttpServer http_server(executor, http_port,
                                            zeptodb::auth::TlsConfig{}, auth);
    http_server.set_ready(true);
    http_server.start_async();

    // Arrow Flight server
    zeptodb::server::FlightServer flight_server(executor);
    flight_server.start_async(flight_port);

    std::cout << "ZeptoDB servers started (" << num_ticks << " sample ticks)\n"
              << "  HTTP:   http://localhost:" << http_port << "\n"
              << "  Flight: grpc://localhost:" << flight_port << "\n"
              << "\nPython example:\n"
              << "  import pyarrow.flight as fl\n"
              << "  client = fl.connect('grpc://localhost:" << flight_port << "')\n"
              << "  reader = client.do_get(fl.Ticket('SELECT * FROM trades LIMIT 5'))\n"
              << "  print(reader.read_all().to_pandas())\n";

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    flight_server.stop();
    http_server.stop();
    return 0;
}
