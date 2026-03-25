// ============================================================================
// zepto_http_server — standalone HTTP server for Web UI + API
// ============================================================================
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/util/logger.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>

#ifdef __linux__
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

#ifdef __linux__
static bool is_port_in_use(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ret = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return ret == 0;
}
#endif

int main(int argc, char* argv[]) {
    uint16_t port = 8123;
    int num_ticks = 10000;
    bool no_auth = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if ((arg == "--ticks" || arg == "-n") && i + 1 < argc)
            num_ticks = std::atoi(argv[++i]);
        else if (arg == "--no-auth")
            no_auth = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--port 8123] [--ticks 10000] [--no-auth]\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize logger — console + file output
    zeptodb::util::Logger::instance().init("/var/log/zeptodb",
                                           zeptodb::util::LogLevel::INFO);

#ifdef __linux__
    if (is_port_in_use(port)) {
        std::cerr << "Error: port " << port << " is already in use. "
                  << "Stop the existing server first or use --port <other>.\n";
        return 1;
    }
#endif

    // Pipeline
    zeptodb::core::PipelineConfig cfg;
    cfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(cfg);

    // Register default trades schema so SHOW TABLES / DESCRIBE work
    zeptodb::sql::QueryExecutor bootstrap_ex(pipeline);
    bootstrap_ex.execute("CREATE TABLE IF NOT EXISTS trades (symbol SYMBOL, price INT64, volume INT64, timestamp INT64)");

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

    // Auth setup
    zeptodb::auth::AuthManager::Config auth_cfg;
    auth_cfg.enabled = !no_auth;
    auth_cfg.api_keys_file = "dev_keys.txt";
    auth_cfg.jwt_enabled = false;
    auth_cfg.rate_limit_enabled = false;
    auth_cfg.audit_enabled = false;
    auth_cfg.audit_buffer_enabled = false;

    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);

    auto admin_key  = auth->create_api_key("dev-admin",  zeptodb::auth::Role::ADMIN);
    auto writer_key = auth->create_api_key("dev-writer", zeptodb::auth::Role::WRITER);
    auto reader_key = auth->create_api_key("dev-reader", zeptodb::auth::Role::READER);

    // HTTP server
    zeptodb::sql::QueryExecutor executor(pipeline);
    zeptodb::server::HttpServer server(executor, port, zeptodb::auth::TlsConfig{}, auth);
    server.set_ready(true);

    std::cout << "ZeptoDB HTTP server: http://localhost:" << port
              << "  (" << num_ticks << " sample ticks loaded)\n\n"
              << "=== Dev API Keys (shown once) ===\n"
              << "  admin:  " << admin_key  << "\n"
              << "  writer: " << writer_key << "\n"
              << "  reader: " << reader_key << "\n"
              << "=================================\n";

    server.start_async();

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    return 0;
}
