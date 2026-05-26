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
#include "zeptodb/cluster/coordinator_routing_adapter.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/util/logger.h"
#include "zeptodb/common/logger.h"  // ZEPTO_INFO (engine logger, devlog 118 cold-tier)

#include <csignal>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <memory>
#include <unordered_map>

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

    // devlog 102: ingest scale-out phase 1. 0 = engine default (auto / 65536).
    // Precedence: CLI flag > env var > engine default. Env vars are set by the
    // Helm chart (both Deployment and StatefulSet) so the knobs are honored
    // even when the pod starts with the image's default CMD.
    size_t drain_threads_cfg = 0;
    size_t ring_buffer_capacity_cfg = 0;
    if (const char* e = std::getenv("ZEPTO_DRAIN_THREADS"); e && *e)
        drain_threads_cfg = static_cast<size_t>(std::atoll(e));
    if (const char* e = std::getenv("ZEPTO_RING_BUFFER_CAPACITY"); e && *e)
        ring_buffer_capacity_cfg = static_cast<size_t>(std::atoll(e));

    // devlog 118: cold-tier S3 Parquet sink. Same precedence (CLI > env >
    // default). Env vars are set by the Helm `coldTier.*` block.
    bool        cold_tier_enabled        = false;
    std::string cold_tier_format         = "parquet";   // parquet | both
    std::string cold_tier_layout         = "hive";      // hive | flat
    int         cold_tier_age_hours      = 24;
    bool        cold_tier_delete_local   = true;
    std::string cold_tier_s3_bucket;
    std::string cold_tier_s3_region      = "us-east-1";
    std::string cold_tier_s3_prefix      = "hdb";
    std::string cold_tier_s3_endpoint;
    bool        cold_tier_s3_path_style  = false;

    auto env_truthy = [](const char* v) {
        if (!v || !*v) return false;
        std::string s(v);
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
        return s == "1" || s == "true" || s == "yes" || s == "on";
    };
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_ENABLED"))
        cold_tier_enabled = env_truthy(e);
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_FORMAT"); e && *e)
        cold_tier_format = e;
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_LAYOUT"); e && *e)
        cold_tier_layout = e;
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_AGE_HOURS"); e && *e)
        cold_tier_age_hours = std::atoi(e);
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_DELETE_LOCAL_AFTER_S3"))
        cold_tier_delete_local = env_truthy(e);
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_S3_BUCKET"); e && *e)
        cold_tier_s3_bucket = e;
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_S3_REGION"); e && *e)
        cold_tier_s3_region = e;
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_S3_PREFIX"); e && *e)
        cold_tier_s3_prefix = e;
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_S3_ENDPOINT_URL"); e && *e)
        cold_tier_s3_endpoint = e;
    if (const char* e = std::getenv("ZEPTO_COLD_TIER_S3_USE_PATH_STYLE"))
        cold_tier_s3_path_style = env_truthy(e);

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
        else if (arg == "--drain-threads" && i + 1 < argc)
            drain_threads_cfg = static_cast<size_t>(std::atoll(argv[++i]));
        else if (arg == "--ring-buffer-capacity" && i + 1 < argc)
            ring_buffer_capacity_cfg = static_cast<size_t>(std::atoll(argv[++i]));
        // --- devlog 118: cold-tier S3 Parquet sink CLI flags ---
        else if (arg == "--cold-tier-enabled")
            cold_tier_enabled = true;
        else if (arg == "--cold-tier-format" && i + 1 < argc)
            cold_tier_format = argv[++i];
        else if (arg == "--cold-tier-layout" && i + 1 < argc)
            cold_tier_layout = argv[++i];
        else if (arg == "--cold-tier-age-hours" && i + 1 < argc)
            cold_tier_age_hours = std::atoi(argv[++i]);
        else if (arg == "--cold-tier-delete-local-after-s3")
            cold_tier_delete_local = true;
        else if (arg == "--cold-tier-s3-bucket" && i + 1 < argc)
            cold_tier_s3_bucket = argv[++i];
        else if (arg == "--cold-tier-s3-region" && i + 1 < argc)
            cold_tier_s3_region = argv[++i];
        else if (arg == "--cold-tier-s3-prefix" && i + 1 < argc)
            cold_tier_s3_prefix = argv[++i];
        else if (arg == "--cold-tier-s3-endpoint-url" && i + 1 < argc)
            cold_tier_s3_endpoint = argv[++i];
        else if (arg == "--cold-tier-s3-use-path-style")
            cold_tier_s3_path_style = true;
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
                      << "Ingest tuning (devlog 102):\n"
                      << "  --drain-threads N        0 = auto (max(2, hw/4)); N>0 = explicit drain thread count\n"
                      << "  --ring-buffer-capacity N Ring-buffer slots, power-of-two in [4096, 16777216]; 0 = default 65536\n\n"
                      << "Cold tier S3 Parquet sink (devlog 118):\n"
                      << "  --cold-tier-enabled                       Enable Parquet+S3 cold-tier flush\n"
                      << "  --cold-tier-format parquet|both           Output format (default: parquet)\n"
                      << "  --cold-tier-layout hive|flat              S3 path layout (default: hive)\n"
                      << "  --cold-tier-age-hours N                   Promote partitions older than N hours (default 24)\n"
                      << "  --cold-tier-delete-local-after-s3         Delete local Parquet after successful upload\n"
                      << "  --cold-tier-s3-bucket <name>              Target S3 bucket (required when enabled)\n"
                      << "  --cold-tier-s3-region <region>            AWS region (default: us-east-1)\n"
                      << "  --cold-tier-s3-prefix <prefix>            S3 key prefix (default: hdb)\n"
                      << "  --cold-tier-s3-endpoint-url <url>         Custom endpoint (MinIO / non-AWS)\n"
                      << "  --cold-tier-s3-use-path-style             MinIO compatibility\n"
                      << "  Env vars: ZEPTO_COLD_TIER_* (CLI > env > default)\n\n"
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
    cfg.drain_threads = drain_threads_cfg;
    cfg.ring_buffer_capacity = ring_buffer_capacity_cfg;

    // devlog 118: cold-tier S3 Parquet sink. When enabled, switches HDB
    // tier on (TIERED) so the FlushManager runs, sets output_format to
    // PARQUET (or BOTH for binary+parquet), and configures the S3 sink
    // with the resolved bucket/region/prefix/layout. CLI flags win over
    // env vars (already merged above).
    if (cold_tier_enabled) {
        if (cold_tier_s3_bucket.empty()) {
            std::cerr << "Error: --cold-tier-enabled requires --cold-tier-s3-bucket "
                         "(or ZEPTO_COLD_TIER_S3_BUCKET)\n";
            return 1;
        }
        // Cold-tier needs HDB on disk; switch to tiered if user hasn't
        // already supplied a path.
        if (cfg.storage_mode == zeptodb::core::StorageMode::PURE_IN_MEMORY) {
            cfg.storage_mode  = zeptodb::core::StorageMode::TIERED;
            if (cfg.hdb_base_path.empty()) cfg.hdb_base_path = "/tmp/zepto_hdb";
        }
        auto& fc = cfg.flush_config;
        fc.output_format = (cold_tier_format == "both")
            ? zeptodb::storage::HDBOutputFormat::BOTH
            : zeptodb::storage::HDBOutputFormat::PARQUET;
        fc.enable_s3_upload      = true;
        fc.delete_local_after_s3 = cold_tier_delete_local;
        fc.auto_seal_age_hours   = cold_tier_age_hours;
        fc.s3_config.bucket         = cold_tier_s3_bucket;
        fc.s3_config.region         = cold_tier_s3_region;
        fc.s3_config.prefix         = cold_tier_s3_prefix;
        fc.s3_config.endpoint_url   = cold_tier_s3_endpoint;
        fc.s3_config.use_path_style = cold_tier_s3_path_style;
        fc.s3_config.layout         = (cold_tier_layout == "flat")
            ? zeptodb::storage::S3Layout::FLAT
            : zeptodb::storage::S3Layout::HIVE;

        ZEPTO_INFO("Cold tier S3 Parquet sink ENABLED: format={} layout={} "
                   "age_hours={} delete_local={} bucket={} region={} prefix={} "
                   "endpoint={} path_style={}",
                   cold_tier_format, cold_tier_layout,
                   cold_tier_age_hours, cold_tier_delete_local,
                   cold_tier_s3_bucket, cold_tier_s3_region,
                   cold_tier_s3_prefix,
                   cold_tier_s3_endpoint.empty() ? "<aws>" : cold_tier_s3_endpoint,
                   cold_tier_s3_path_style);
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
    // Cluster-aware INSERT routing (BACKLOG P8-I3-wire, devlog 111): when
    // remote_nodes is non-empty we construct a CoordinatorRoutingAdapter
    // later in main() and call executor.set_cluster_node(&adapter). Single-
    // pod deployments keep executor.cluster_node_ = nullptr (original direct-
    // to-pipeline behaviour).
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

    // ── Cluster wire-up (when remote nodes exist) ──
    // BACKLOG P8-I3-wire (devlog 111): unify the PartitionRouter so the
    // rebalance path and the routing-adapter path both read/write the same
    // ring under the same shared_mutex, exposed by QueryCoordinator.
    std::unique_ptr<zeptodb::cluster::PartitionMigrator>       rebalance_migrator;
    std::unique_ptr<zeptodb::cluster::RebalanceManager>        rebalance_mgr;
    std::unordered_map<zeptodb::cluster::NodeId,
                       std::shared_ptr<zeptodb::cluster::RpcClientBase>> peer_rpc;
    std::unique_ptr<zeptodb::cluster::CoordinatorRoutingAdapter> routing_adapter;

    if (!remote_nodes.empty() && coordinator) {
        // --- Migrator ring (separate from the routing ring: migrator holds
        // host:port for physical moves; the routing PartitionRouter inside
        // coordinator holds just NodeId→slot). ---
        rebalance_migrator = std::make_unique<zeptodb::cluster::PartitionMigrator>();
        rebalance_migrator->add_node(node_id, "127.0.0.1", static_cast<uint16_t>(port + 100));
        for (auto& rn : remote_nodes) {
            rebalance_migrator->add_node(rn.id, rn.host, static_cast<uint16_t>(rn.port + 100));
        }

        // Rebalance manager mutates the COORDINATOR'S router (not a separate
        // instance) so the routing-adapter sees fresh ring state after any
        // add_node/remove_node.
        rebalance_mgr = std::make_unique<zeptodb::cluster::RebalanceManager>(
            coordinator->router(), *rebalance_migrator);
        server.set_rebalance_manager(rebalance_mgr.get());
        std::cout << "Rebalance manager: enabled (" << (remote_nodes.size() + 1) << " nodes)\n";

        // --- Peer RPC clients: one per remote node, keyed by NodeId ---
        for (auto& rn : remote_nodes) {
            // Peer RPC port = peer HTTP port + 100 (convention from
            // cluster_node.h:436 and migrator above).
            peer_rpc.emplace(rn.id,
                std::make_shared<zeptodb::cluster::TcpRpcClient>(
                    rn.host, static_cast<uint16_t>(rn.port + 100),
                    /*timeout_ms=*/2000));
        }

        // --- Peer RPC server so other pods can forward ticks TO us ---
        // HA mode already starts an RPC server for peer SQL; non-HA needs a
        // symmetric one with a tick_cb for TICK_INGEST.
        if (!rpc_srv) {
            rpc_srv = std::make_unique<zeptodb::cluster::TcpRpcServer>();
            rpc_srv->start(
                static_cast<uint16_t>(port + 100),
                // sql_cb: scatter-gathered SQL from another coordinator
                [&](const std::string& sql) {
                    zeptodb::sql::QueryExecutor ex(pipeline);
                    return ex.execute(sql);
                },
                // tick_cb: forwarded tick lands directly in local pipeline
                [&](const zeptodb::ingestion::TickMessage& msg) {
                    return pipeline.ingest_tick(msg);
                });
            std::cout << "Peer RPC server: port " << (port + 100) << "\n";
        }

        // --- The actual wire-up: cluster-aware INSERT routing ---
        routing_adapter = std::make_unique<zeptodb::cluster::CoordinatorRoutingAdapter>(
            &coordinator->router(),
            &coordinator->router_mutex(),
            &pipeline,
            static_cast<zeptodb::cluster::NodeId>(node_id),
            &peer_rpc);
        executor.set_cluster_node(routing_adapter.get());
        std::cout << "Cluster routing: enabled (" << remote_nodes.size()
                  << " remote nodes)\n";
    }

    server.start_async();

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (ha) ha->stop();
    if (rpc_srv) rpc_srv->stop();
    server.stop();
    return 0;
}
