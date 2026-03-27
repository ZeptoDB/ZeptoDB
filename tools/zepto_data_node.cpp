// ============================================================================
// zepto_data_node — standalone data node for multi-process cluster
// ============================================================================
// Usage: ./zepto_data_node <port> [num_ticks] [--node-id N] [--symbol S]
//                          [--coordinator host:port] [--api-key KEY]
//   Starts a TcpRpcServer on <port>, optionally self-registers with coordinator.
//   node_id defaults to port number if not specified.
// ============================================================================

#include "zeptodb/core/pipeline.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/server/metrics_collector.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <thread>
#include <string>

// Minimal HTTP POST for self-registration (no external deps)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

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

/// POST JSON to coordinator's /admin/nodes endpoint
static bool register_with_coordinator(
    const std::string& coord_host, uint16_t coord_port,
    uint32_t node_id, const std::string& self_host, uint16_t self_port,
    const std::string& api_key)
{
    // Resolve hostname
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(coord_host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return false; }

    auto* addr = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    addr->sin_port = htons(coord_port);

    if (connect(fd, reinterpret_cast<sockaddr*>(addr), sizeof(*addr)) < 0) {
        close(fd); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);

    std::string body = "{\"id\":" + std::to_string(node_id)
        + ",\"host\":\"" + self_host + "\""
        + ",\"port\":" + std::to_string(self_port) + "}";

    std::string req = "POST /admin/nodes HTTP/1.1\r\n"
        "Host: " + coord_host + ":" + std::to_string(coord_port) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!api_key.empty())
        req += "Authorization: Bearer " + api_key + "\r\n";
    req += "Connection: close\r\n\r\n" + body;

    send(fd, req.data(), req.size(), 0);

    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    if (n <= 0) return false;
    buf[n] = '\0';
    // Check for HTTP 200
    return std::string(buf, static_cast<size_t>(n)).find("200") != std::string::npos;
}

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cerr << "Usage: " << argv[0]
                  << " <port> [num_ticks] [options]\n\n"
                  << "Options:\n"
                  << "  --node-id N                 node ID (default: port number)\n"
                  << "  --symbol S                  symbol_id for sample data (default: 1)\n"
                  << "  --coordinator host:port      auto-register with coordinator\n"
                  << "  --api-key KEY               admin API key for registration\n"
                  << "  --advertise-host HOST       host to advertise (default: localhost)\n";
        return 1;
    }

    uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
    int num_ticks = 0;
    uint32_t node_id = port;
    uint32_t symbol = 1;
    std::string coordinator_spec;  // "host:port"
    std::string api_key;
    std::string advertise_host = "localhost";

    // Parse positional num_ticks (argv[2] if not a flag)
    if (argc >= 3 && argv[2][0] != '-')
        num_ticks = std::atoi(argv[2]);

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--node-id" && i + 1 < argc)
            node_id = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--symbol" && i + 1 < argc)
            symbol = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--coordinator" && i + 1 < argc)
            coordinator_spec = argv[++i];
        else if (arg == "--api-key" && i + 1 < argc)
            api_key = argv[++i];
        else if (arg == "--advertise-host" && i + 1 < argc)
            advertise_host = argv[++i];
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (is_port_in_use(port)) {
        std::cerr << "Error: port " << port << " is already in use.\n";
        return 1;
    }

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
    if (num_ticks > 0)
        pipeline.drain_sync(static_cast<size_t>(num_ticks) + 100);

    std::cout << "Data node ready: port=" << port
              << " node_id=" << node_id
              << " ticks=" << num_ticks
              << " symbol=" << symbol << std::endl;

    // Start RPC server
    zeptodb::cluster::TcpRpcServer srv;

    // Metrics collector for this data node
    zeptodb::server::MetricsCollector mc(pipeline.stats());
    mc.start();

    srv.set_stats_callback([&, port, node_id]() {
        const auto& s = pipeline.stats();
        return std::string("{\"id\":") + std::to_string(node_id)
            + ",\"host\":\"" + advertise_host + "\""
            + ",\"port\":" + std::to_string(port)
            + ",\"state\":\"ACTIVE\""
            + ",\"ticks_ingested\":" + std::to_string(s.ticks_ingested.load())
            + ",\"ticks_stored\":" + std::to_string(s.ticks_stored.load())
            + ",\"queries_executed\":" + std::to_string(s.queries_executed.load())
            + "}";
    });

    srv.set_metrics_callback([&](int64_t since_ms, uint32_t limit) {
        auto snaps = mc.get_history(since_ms, limit > 0 ? limit : 0);
        return zeptodb::server::MetricsCollector::to_json(snaps);
    });

    srv.start(port, [&](const std::string& sql) {
        zeptodb::sql::QueryExecutor ex(pipeline);
        return ex.execute(sql);
    });

    // Self-register with coordinator
    if (!coordinator_spec.empty()) {
        auto colon = coordinator_spec.find(':');
        if (colon != std::string::npos) {
            std::string coord_host = coordinator_spec.substr(0, colon);
            uint16_t coord_port = static_cast<uint16_t>(
                std::atoi(coordinator_spec.substr(colon + 1).c_str()));

            std::cout << "Registering with coordinator " << coord_host
                      << ":" << coord_port << " ..." << std::flush;

            // Retry a few times (coordinator might not be ready yet)
            bool ok = false;
            for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
                if (attempt > 0)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                ok = register_with_coordinator(
                    coord_host, coord_port, node_id,
                    advertise_host, port, api_key);
            }
            std::cout << (ok ? " OK" : " FAILED") << std::endl;
        }
    }

    // Wait for signal
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    srv.stop();
    std::cout << "Data node stopped." << std::endl;
    return 0;
}
