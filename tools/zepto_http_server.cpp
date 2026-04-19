// ============================================================================
// zepto_http_server — HTTP server for Web UI + API
// ============================================================================
// Always starts with QueryCoordinator enabled.
// - No data nodes → standalone behavior (local pipeline only)
// - Data nodes added → cluster behavior
// - --ha active/standby --peer host:port → HA failover
// ============================================================================
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/coordinator_ha.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/util/logger.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <fstream>
#include <filesystem>

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

struct RemoteNodeSpec { uint32_t id; std::string host; uint16_t port; };

int main(int argc, char* argv[]) {
    uint16_t port = 8123;
    uint16_t rpc_port = 0;  // RPC port for HA peer communication
    int num_ticks = 10000;
    bool no_auth = false;
    uint32_t node_id = 0;
    std::vector<RemoteNodeSpec> remote_nodes;
    std::string log_level = "info";
    std::string ha_role_str;   // "active" or "standby"
    std::string peer_spec;     // "host:port"

    // JWT / SSO settings
    std::string jwt_issuer;
    std::string jwt_audience;
    std::string jwt_secret;       // HS256
    std::string jwt_public_key;   // RS256 PEM file path
    std::string jwks_url;         // JWKS endpoint URL
    std::string web_dir;          // Web UI static files directory

    // Storage mode (devlog 086 D4): allow operators to persist data to HDB
    // instead of the default PURE_IN_MEMORY.
    std::string hdb_dir;                 // --hdb-dir <path>
    std::string storage_mode = "pure";   // --storage-mode pure|tiered

    // devlog 091 F1: provision tenants at startup.
    std::vector<std::pair<std::string, std::string>> tenants;  // (id, namespace)

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--rpc-port" && i + 1 < argc)
            rpc_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if ((arg == "--ticks" || arg == "-n") && i + 1 < argc)
            num_ticks = std::atoi(argv[++i]);
        else if (arg == "--no-auth")
            no_auth = true;
        else if (arg == "--node-id" && i + 1 < argc)
            node_id = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--log-level" && i + 1 < argc)
            log_level = argv[++i];
        else if (arg == "--ha" && i + 1 < argc)
            ha_role_str = argv[++i];
        else if (arg == "--peer" && i + 1 < argc)
            peer_spec = argv[++i];
        else if (arg == "--cluster") {}  // no-op (always enabled)
        else if (arg == "--jwt-issuer" && i + 1 < argc)
            jwt_issuer = argv[++i];
        else if (arg == "--jwt-audience" && i + 1 < argc)
            jwt_audience = argv[++i];
        else if (arg == "--jwt-secret" && i + 1 < argc)
            jwt_secret = argv[++i];
        else if (arg == "--jwt-public-key" && i + 1 < argc)
            jwt_public_key = argv[++i];
        else if (arg == "--jwks-url" && i + 1 < argc)
            jwks_url = argv[++i];
        else if (arg == "--web-dir" && i + 1 < argc)
            web_dir = argv[++i];
        else if (arg == "--hdb-dir" && i + 1 < argc)
            hdb_dir = argv[++i];
        else if (arg == "--storage-mode" && i + 1 < argc)
            storage_mode = argv[++i];
        else if (arg == "--tenant" && i + 1 < argc) {
            // --tenant <id:namespace>  (repeatable) — devlog 091 F1
            std::string spec = argv[++i];
            auto colon = spec.find(':');
            if (colon == std::string::npos) {
                std::cerr << "Error: --tenant expects <id:namespace>, got '" << spec << "'\n";
                return 1;
            }
            tenants.emplace_back(spec.substr(0, colon), spec.substr(colon + 1));
        }
        else if (arg == "--add-node" && i + 1 < argc) {
            std::string spec = argv[++i];
            auto p1 = spec.find(':');
            auto p2 = spec.find(':', p1 + 1);
            if (p1 != std::string::npos && p2 != std::string::npos) {
                remote_nodes.push_back({
                    static_cast<uint32_t>(std::stoi(spec.substr(0, p1))),
                    spec.substr(p1 + 1, p2 - p1 - 1),
                    static_cast<uint16_t>(std::stoi(spec.substr(p2 + 1)))
                });
            }
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--port 8123] [--ticks 10000] [--no-auth]\n"
                      << "       [--node-id 0] [--add-node id:host:port ...]\n"
                      << "       [--log-level info|debug|warn|error]\n"
                      << "       [--ha active|standby] [--peer host:port] [--rpc-port PORT]\n\n"
                      << "JWT / SSO:\n"
                      << "  --jwt-issuer <url>       Expected issuer (iss claim)\n"
                      << "  --jwt-audience <aud>     Expected audience (aud claim)\n"
                      << "  --jwt-secret <secret>    HS256 shared secret\n"
                      << "  --jwt-public-key <path>  RS256 PEM public key file\n"
                      << "  --jwks-url <url>         JWKS endpoint (auto-fetch RS256 keys)\n\n"
                      << "Storage:\n"
                      << "  --hdb-dir <path>         HDB base directory (implies --storage-mode tiered)\n"
                      << "  --storage-mode <mode>    pure (in-memory, default) | tiered (RDB+HDB)\n\n"
                      << "Tenants:\n"
                      << "  --tenant <id:namespace>  Register a tenant with a table-namespace prefix (repeatable)\n\n"
                      << "HA mode:\n"
                      << "  --ha active  --peer standby-host:rpc-port  --rpc-port 9100\n"
                      << "  --ha standby --peer active-host:rpc-port   --rpc-port 9101\n\n"
                      << "Example:\n"
                      << "  # Active coordinator\n"
                      << "  ./zepto_http_server --port 8123 --ha active --peer localhost:9101 --rpc-port 9100\n\n"
                      << "  # Standby coordinator\n"
                      << "  ./zepto_http_server --port 8124 --ha standby --peer localhost:9100 --rpc-port 9101\n\n"
                      << "  # SSO with Okta (JWKS)\n"
                      << "  ./zepto_http_server --jwks-url https://dev-123.okta.com/oauth2/default/v1/keys --jwt-issuer https://dev-123.okta.com/oauth2/default\n\n"
                      << "  # SSO with HS256 secret\n"
                      << "  ./zepto_http_server --jwt-secret my-shared-secret --jwt-issuer https://auth.example.com\n";
            return 0;
        }
    }

    bool ha_mode = !ha_role_str.empty();
    if (ha_mode && peer_spec.empty()) {
        std::cerr << "Error: --ha requires --peer host:port\n";
        return 1;
    }
    if (ha_mode && rpc_port == 0) {
        rpc_port = port + 1000;  // default: HTTP port + 1000
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize logger
    zeptodb::util::LogLevel ll = zeptodb::util::LogLevel::INFO;
    if (log_level == "debug")    ll = zeptodb::util::LogLevel::DEBUG;
    else if (log_level == "warn")  ll = zeptodb::util::LogLevel::WARN;
    else if (log_level == "error") ll = zeptodb::util::LogLevel::ERROR;
    zeptodb::util::Logger::instance().init("/var/log/zeptodb", ll);

#ifdef __linux__
    if (is_port_in_use(port)) {
        std::cerr << "Error: port " << port << " is already in use.\n";
        return 1;
    }
#endif

    // Pipeline
    zeptodb::core::PipelineConfig cfg;
    // devlog 086 (D4): --hdb-dir or --storage-mode tiered switches to on-disk
    // HDB tier. Default stays PURE_IN_MEMORY for HFT dev-server parity.
    if (storage_mode == "tiered" || !hdb_dir.empty()) {
        cfg.storage_mode  = zeptodb::core::StorageMode::TIERED;
        cfg.hdb_base_path = hdb_dir.empty() ? std::string("/tmp/zepto_hdb") : hdb_dir;
    } else {
        cfg.storage_mode  = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    }
    zeptodb::core::ZeptoPipeline pipeline(cfg);

    // Register default trades schema
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
    auth_cfg.rate_limit_enabled = false;
    auth_cfg.audit_enabled = false;
    auth_cfg.audit_buffer_enabled = false;

    // JWT / SSO — enabled if any jwt flag is provided
    bool jwt_requested = !jwt_secret.empty() || !jwt_public_key.empty() || !jwks_url.empty();
    if (jwt_requested) {
        if (!jwt_secret.empty() && (!jwt_public_key.empty() || !jwks_url.empty())) {
            std::cerr << "Error: --jwt-secret (HS256) cannot be combined with --jwt-public-key or --jwks-url (RS256)\n";
            return 1;
        }
        auth_cfg.jwt_enabled = true;
        auth_cfg.jwt.expected_issuer  = jwt_issuer;
        auth_cfg.jwt.expected_audience = jwt_audience;
        if (!jwt_secret.empty()) {
            auth_cfg.jwt.hs256_secret = jwt_secret;
        } else if (!jwt_public_key.empty()) {
            std::ifstream pem_file(jwt_public_key);
            if (!pem_file.is_open()) {
                std::cerr << "Error: cannot open PEM file: " << jwt_public_key << "\n";
                return 1;
            }
            auth_cfg.jwt.rs256_public_key_pem = std::string(
                std::istreambuf_iterator<char>(pem_file), std::istreambuf_iterator<char>());
        }
        // jwks_url is handled after JwksProvider is implemented (step 2)
        auth_cfg.jwks_url = jwks_url;
    } else {
        auth_cfg.jwt_enabled = false;
    }

    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);

    // Only create dev keys if they don't already exist
    auto existing = auth->list_api_keys();
    auto has_key = [&](const std::string& name) {
        return std::any_of(existing.begin(), existing.end(),
            [&](const auto& e) { return e.name == name && e.enabled; });
    };
    std::string admin_key, writer_key, reader_key;
    if (!has_key("dev-admin"))  admin_key  = auth->create_api_key("dev-admin",  zeptodb::auth::Role::ADMIN);
    if (!has_key("dev-writer")) writer_key = auth->create_api_key("dev-writer", zeptodb::auth::Role::WRITER);
    if (!has_key("dev-reader")) reader_key = auth->create_api_key("dev-reader", zeptodb::auth::Role::READER);

    // HTTP server
    zeptodb::sql::QueryExecutor executor(pipeline);
    zeptodb::server::HttpServer server(executor, port, zeptodb::auth::TlsConfig{}, auth);
    server.set_ready(true);

    // Web UI static files
    if (web_dir.empty()) {
        // Auto-detect: check common locations
        for (const auto& candidate : {"/opt/zeptodb/web", "./web/out"}) {
            if (std::filesystem::is_directory(candidate)) {
                web_dir = candidate;
                break;
            }
        }
    }
    if (!web_dir.empty()) server.set_web_dir(web_dir);

    // devlog 091 F1: provision tenants from --tenant id:namespace flags
    if (!tenants.empty()) {
        auto tm = std::make_shared<zeptodb::auth::TenantManager>();
        for (auto& [tid, ns] : tenants) {
            zeptodb::auth::TenantConfig cfg;
            cfg.tenant_id       = tid;
            cfg.table_namespace = ns;
            tm->create_tenant(std::move(cfg));
        }
        server.set_tenant_manager(tm);
        std::cout << "Tenants provisioned: " << tenants.size() << "\n";
    }

    // ── HA mode ──
    std::unique_ptr<zeptodb::cluster::CoordinatorHA> ha;
    std::unique_ptr<zeptodb::cluster::TcpRpcServer> rpc_srv;

    // ── Non-HA: plain coordinator ──
    std::unique_ptr<zeptodb::cluster::QueryCoordinator> coordinator;

    if (ha_mode) {
        auto role = (ha_role_str == "active")
            ? zeptodb::cluster::CoordinatorRole::ACTIVE
            : zeptodb::cluster::CoordinatorRole::STANDBY;

        auto colon = peer_spec.find(':');
        std::string peer_host = peer_spec.substr(0, colon);
        uint16_t peer_port = static_cast<uint16_t>(
            std::atoi(peer_spec.substr(colon + 1).c_str()));

        ha = std::make_unique<zeptodb::cluster::CoordinatorHA>();
        ha->init(role, peer_host, peer_port);

        // Register local node
        zeptodb::cluster::NodeAddress self_addr{"localhost", port, node_id};
        ha->add_local_node(self_addr, pipeline);

        // Register remote data nodes
        for (auto& rn : remote_nodes) {
            zeptodb::cluster::NodeAddress addr{rn.host, rn.port, rn.id};
            ha->add_remote_node(addr);
        }

        // Start RPC server for peer communication (ping/query forwarding)
        rpc_srv = std::make_unique<zeptodb::cluster::TcpRpcServer>();
        rpc_srv->start(rpc_port, [&](const std::string& sql) {
            return ha->execute_sql(sql);
        });

        ha->on_promotion([port, &ha]() {
            std::cout << "\n*** PROMOTED to ACTIVE (port " << port << ") ***\n"
                      << "Promotions: " << ha->promotion_count() << "\n";
        });

        ha->start();
        server.set_coordinator(&ha->coordinator(), static_cast<uint16_t>(node_id));

        std::cout << "HA mode: " << ha_role_str
                  << " (rpc_port=" << rpc_port
                  << ", peer=" << peer_spec << ")\n";
    } else {
        coordinator = std::make_unique<zeptodb::cluster::QueryCoordinator>();
        zeptodb::cluster::NodeAddress self_addr{"localhost", port, node_id};
        coordinator->add_local_node(self_addr, pipeline);

        for (auto& rn : remote_nodes) {
            zeptodb::cluster::NodeAddress addr{rn.host, rn.port, rn.id};
            coordinator->add_remote_node(addr);
        }

        server.set_coordinator(coordinator.get(), static_cast<uint16_t>(node_id));
    }

    std::cout << "ZeptoDB HTTP server: http://localhost:" << port
              << "  (" << num_ticks << " sample ticks loaded, node_id=" << node_id << ")\n";
    if (!remote_nodes.empty()) {
        std::cout << "Remote nodes:\n";
        for (auto& rn : remote_nodes)
            std::cout << "  Node " << rn.id << " → " << rn.host << ":" << rn.port << "\n";
    }
    if (!admin_key.empty() || !writer_key.empty() || !reader_key.empty()) {
        std::cout << "\n=== Dev API Keys (shown once) ===\n";
        if (!admin_key.empty())  std::cout << "  admin:  " << admin_key  << "\n";
        if (!writer_key.empty()) std::cout << "  writer: " << writer_key << "\n";
        if (!reader_key.empty()) std::cout << "  reader: " << reader_key << "\n";
        std::cout << "=================================\n";
    } else {
        std::cout << "\n=== Dev API Keys: already exist (skipped creation) ===\n";
    }

    // ── Rebalance manager (when remote nodes exist) ──
    std::unique_ptr<zeptodb::cluster::PartitionRouter> rebalance_router;
    std::unique_ptr<zeptodb::cluster::PartitionMigrator> rebalance_migrator;
    std::unique_ptr<zeptodb::cluster::RebalanceManager> rebalance_mgr;

    if (!remote_nodes.empty()) {
        rebalance_router = std::make_unique<zeptodb::cluster::PartitionRouter>();
        rebalance_migrator = std::make_unique<zeptodb::cluster::PartitionMigrator>();

        rebalance_router->add_node(node_id);
        rebalance_migrator->add_node(node_id, "127.0.0.1", static_cast<uint16_t>(port + 100));

        for (auto& rn : remote_nodes) {
            rebalance_router->add_node(rn.id);
            rebalance_migrator->add_node(rn.id, rn.host, static_cast<uint16_t>(rn.port + 100));
        }

        rebalance_mgr = std::make_unique<zeptodb::cluster::RebalanceManager>(
            *rebalance_router, *rebalance_migrator);
        server.set_rebalance_manager(rebalance_mgr.get());
        std::cout << "Rebalance manager: enabled (" << (remote_nodes.size() + 1) << " nodes)\n";
    }

    server.start_async();

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (ha) ha->stop();
    if (rpc_srv) rpc_srv->stop();
    server.stop();
    return 0;
}
