// ============================================================================
// zepto_http_server — HTTP server for Web UI + API
// ============================================================================
// Always starts with QueryCoordinator enabled.
// - No data nodes → standalone behavior (local pipeline only)
// - Data nodes added → cluster behavior
// - --ha active/standby --peer host:port → HA failover
// ============================================================================
#include "zeptodb/core/pipeline.h"
#include "zeptodb/ai/agent_memory.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/coordinator_ha.h"
#include "zeptodb/cluster/coordinator_routing_adapter.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/failover_manager.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/ring_consensus.h"
#include "zeptodb/cluster/rebalance_manager.h"
#include "zeptodb/cluster/partition_migrator.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/util/logger.h"
#include "zeptodb/common/logger.h"  // ZEPTO_INFO (engine logger, devlog 118 cold-tier)

#include <csignal>
#include <charconv>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <iostream>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

struct RemoteNodeSpec { uint32_t id; std::string host; uint16_t port; };

template <typename UInt>
static bool parse_unsigned_cli(std::string_view text, UInt* output) {
    static_assert(std::is_unsigned_v<UInt>);
    if (output == nullptr || text.empty()) return false;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > static_cast<uint64_t>(std::numeric_limits<UInt>::max())) {
        return false;
    }
    *output = static_cast<UInt>(value);
    return true;
}

static bool is_loopback_bind_host(std::string_view host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

static bool is_loopback_http_url(std::string_view url) {
    constexpr std::string_view scheme = "http://";
    if (!url.starts_with(scheme)) return false;
    url.remove_prefix(scheme.size());
    const size_t authority_end = url.find_first_of("/?#");
    std::string_view authority = url.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }

    std::string_view host;
    std::string_view port;
    bool has_port_delimiter = false;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(1, close - 1);
        const auto remainder = authority.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') return false;
            has_port_delimiter = true;
            port = remainder.substr(1);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            has_port_delimiter = true;
            port = authority.substr(colon + 1);
        }
    }
    if (host != "localhost" && host != "127.0.0.1" && host != "::1") {
        return false;
    }
    if (has_port_delimiter && port.empty()) {
        return false;
    }
    if (!port.empty()) {
        uint16_t parsed_port = 0;
        if (!parse_unsigned_cli(port, &parsed_port) || parsed_port == 0) {
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    zeptodb::auth::license().load();

    uint16_t port = 8123;
    uint16_t rpc_port = 0;  // RPC port for HA peer communication
    int num_ticks = 0;
    bool no_auth = false;
    bool allow_insecure_cluster = false;
    bool allow_plaintext_http = false;
    bool bootstrap_dev_keys = false;
    bool no_bootstrap_dev_keys = false;
    bool disable_rate_limit = false;
    bool disable_audit = false;
    bool secure_cookie = false;
    bool allow_experimental_distributed_queries = false;
    bool acknowledge_incomplete_durability = false;
    uint32_t node_id = 0;
    std::vector<RemoteNodeSpec> remote_nodes;
    std::string log_level = "info";
    std::string ha_role_str;   // "active" or "standby"
    std::string peer_spec;     // "host:port"
    std::string bind_host = "127.0.0.1";
    std::string tls_cert_path;
    std::string tls_key_path;
    std::string tls_ca_path;
    std::string api_keys_file;
    std::string audit_log_file;
    size_t max_request_bytes = 64ULL * 1024ULL * 1024ULL;
    uint32_t query_timeout_ms = 30'000;

    if (const char* value = std::getenv("ZEPTO_API_KEYS_FILE");
        value && *value) {
        api_keys_file = value;
    }

    // JWT / SSO settings
    std::string jwt_issuer;
    std::string jwt_audience;
    std::string jwt_secret;       // HS256
    std::string jwt_public_key;   // RS256 PEM file path
    std::string jwks_url;         // JWKS endpoint URL
    std::string oidc_issuer;
    std::string oidc_client_id;
    std::string oidc_client_secret;
    std::string oidc_client_secret_env;
    std::string oidc_client_secret_file;
    std::string oidc_redirect_uri;
    std::string oidc_audience;
    bool oidc_secret_from_argv = false;
    std::string web_dir;          // Web UI static files directory

    // Storage mode (devlog 086 D4): allow operators to persist data to HDB
    // instead of the default PURE_IN_MEMORY.
    std::string hdb_dir;                 // --hdb-dir <path>
    std::string storage_mode = "pure";   // --storage-mode pure|tiered
    std::string agent_memory_dir;        // --agent-memory-dir <path>
    size_t agent_memory_flush_every = 100; // --agent-memory-flush-every N
    size_t agent_memory_max_memories = 0; // --agent-memory-max-memories N
    size_t agent_memory_max_cache_entries = 0; // --agent-memory-max-cache-entries N
    std::string agent_memory_replication_mode = "routed"; // local|routed|quorum|sync
    std::string agent_memory_ann = "off";  // --agent-memory-ann off|auto|sparse_projection|hnsw|ivf
    size_t agent_memory_ann_min_records = 50'000;
    size_t agent_memory_ann_max_candidates = 50'000;
    size_t agent_memory_ann_ivf_centroids = 256;
    size_t agent_memory_ann_ivf_probe = 8;
    uint64_t agent_memory_ring_epoch = 1;

    bool failover_enabled = false;
    uint16_t health_heartbeat_port = 9100;
    uint16_t health_tcp_port = 9101;
    uint32_t health_suspect_ms = 3000;
    uint32_t health_dead_ms = 10000;

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
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &port) || port == 0) {
                std::cerr << "Error: --port expects an integer in [1, 65535]\n";
                return 1;
            }
        }
        else if (arg == "--rpc-port" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &rpc_port) || rpc_port == 0) {
                std::cerr << "Error: --rpc-port expects an integer in [1, 65535]\n";
                return 1;
            }
        }
        else if ((arg == "--ticks" || arg == "-n") && i + 1 < argc) {
            uint32_t parsed = 0;
            if (!parse_unsigned_cli(argv[++i], &parsed) ||
                parsed > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                std::cerr << "Error: --ticks expects a non-negative integer\n";
                return 1;
            }
            num_ticks = static_cast<int>(parsed);
        }
        else if (arg == "--no-auth")
            no_auth = true;
        else if (arg == "--allow-insecure-cluster")
            allow_insecure_cluster = true;
        else if (arg == "--allow-plaintext-http")
            allow_plaintext_http = true;
        else if (arg == "--bootstrap-dev-keys")
            bootstrap_dev_keys = true;
        else if (arg == "--no-bootstrap-dev-keys")
            no_bootstrap_dev_keys = true;
        else if (arg == "--disable-rate-limit")
            disable_rate_limit = true;
        else if (arg == "--disable-audit")
            disable_audit = true;
        else if (arg == "--secure-cookie")
            secure_cookie = true;
        else if (arg == "--allow-experimental-distributed-queries")
            allow_experimental_distributed_queries = true;
        else if (arg == "--acknowledge-incomplete-durability")
            acknowledge_incomplete_durability = true;
        else if (arg == "--node-id" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &node_id)) {
                std::cerr << "Error: --node-id expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--bind" && i + 1 < argc)
            bind_host = argv[++i];
        else if (arg == "--tls-cert" && i + 1 < argc)
            tls_cert_path = argv[++i];
        else if (arg == "--tls-key" && i + 1 < argc)
            tls_key_path = argv[++i];
        else if (arg == "--tls-ca" && i + 1 < argc)
            tls_ca_path = argv[++i];
        else if (arg == "--api-keys-file" && i + 1 < argc)
            api_keys_file = argv[++i];
        else if (arg == "--audit-log-file" && i + 1 < argc)
            audit_log_file = argv[++i];
        else if (arg == "--max-request-bytes" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &max_request_bytes) ||
                max_request_bytes == 0) {
                std::cerr << "Error: --max-request-bytes expects a positive integer\n";
                return 1;
            }
        }
        else if (arg == "--query-timeout-ms" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &query_timeout_ms)) {
                std::cerr << "Error: --query-timeout-ms expects an unsigned integer\n";
                return 1;
            }
        }
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
        else if (arg == "--oidc-issuer" && i + 1 < argc)
            oidc_issuer = argv[++i];
        else if (arg == "--oidc-client-id" && i + 1 < argc)
            oidc_client_id = argv[++i];
        else if (arg == "--oidc-client-secret" && i + 1 < argc) {
            oidc_client_secret = argv[++i];
            oidc_secret_from_argv = true;
        }
        else if (arg == "--oidc-client-secret-env" && i + 1 < argc)
            oidc_client_secret_env = argv[++i];
        else if (arg == "--oidc-client-secret-file" && i + 1 < argc)
            oidc_client_secret_file = argv[++i];
        else if (arg == "--oidc-redirect-uri" && i + 1 < argc)
            oidc_redirect_uri = argv[++i];
        else if (arg == "--oidc-audience" && i + 1 < argc)
            oidc_audience = argv[++i];
        else if (arg == "--web-dir" && i + 1 < argc)
            web_dir = argv[++i];
        else if (arg == "--hdb-dir" && i + 1 < argc)
            hdb_dir = argv[++i];
        else if (arg == "--storage-mode" && i + 1 < argc)
            storage_mode = argv[++i];
        else if (arg == "--agent-memory-dir" && i + 1 < argc)
            agent_memory_dir = argv[++i];
        else if (arg == "--agent-memory-flush-every" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_flush_every)) {
                std::cerr << "Error: --agent-memory-flush-every expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-max-memories" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_max_memories)) {
                std::cerr << "Error: --agent-memory-max-memories expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-max-cache-entries" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_max_cache_entries)) {
                std::cerr << "Error: --agent-memory-max-cache-entries expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-replication-mode" && i + 1 < argc)
            agent_memory_replication_mode = argv[++i];
        else if (arg == "--agent-memory-ann" && i + 1 < argc)
            agent_memory_ann = argv[++i];
        else if (arg == "--agent-memory-ann-min-records" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_ann_min_records)) {
                std::cerr << "Error: --agent-memory-ann-min-records expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-ann-max-candidates" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_ann_max_candidates)) {
                std::cerr << "Error: --agent-memory-ann-max-candidates expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-ann-ivf-centroids" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_ann_ivf_centroids)) {
                std::cerr << "Error: --agent-memory-ann-ivf-centroids expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-ann-ivf-probe" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_ann_ivf_probe)) {
                std::cerr << "Error: --agent-memory-ann-ivf-probe expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--agent-memory-ring-epoch" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &agent_memory_ring_epoch)) {
                std::cerr << "Error: --agent-memory-ring-epoch expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--failover-enabled")
            failover_enabled = true;
        else if (arg == "--health-heartbeat-port" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &health_heartbeat_port) ||
                health_heartbeat_port == 0) {
                std::cerr << "Error: --health-heartbeat-port expects an integer in [1, 65535]\n";
                return 1;
            }
        }
        else if (arg == "--health-tcp-port" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &health_tcp_port) ||
                health_tcp_port == 0) {
                std::cerr << "Error: --health-tcp-port expects an integer in [1, 65535]\n";
                return 1;
            }
        }
        else if (arg == "--health-suspect-ms" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &health_suspect_ms) ||
                health_suspect_ms == 0) {
                std::cerr << "Error: --health-suspect-ms expects a positive integer\n";
                return 1;
            }
        }
        else if (arg == "--health-dead-ms" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &health_dead_ms) ||
                health_dead_ms == 0) {
                std::cerr << "Error: --health-dead-ms expects a positive integer\n";
                return 1;
            }
        }
        else if (arg == "--drain-threads" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &drain_threads_cfg)) {
                std::cerr << "Error: --drain-threads expects an unsigned integer\n";
                return 1;
            }
        }
        else if (arg == "--ring-buffer-capacity" && i + 1 < argc) {
            if (!parse_unsigned_cli(argv[++i], &ring_buffer_capacity_cfg)) {
                std::cerr << "Error: --ring-buffer-capacity expects an unsigned integer\n";
                return 1;
            }
        }
        // --- devlog 118: cold-tier S3 Parquet sink CLI flags ---
        else if (arg == "--cold-tier-enabled")
            cold_tier_enabled = true;
        else if (arg == "--cold-tier-format" && i + 1 < argc)
            cold_tier_format = argv[++i];
        else if (arg == "--cold-tier-layout" && i + 1 < argc)
            cold_tier_layout = argv[++i];
        else if (arg == "--cold-tier-age-hours" && i + 1 < argc) {
            uint32_t parsed = 0;
            if (!parse_unsigned_cli(argv[++i], &parsed) || parsed == 0 ||
                parsed > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                std::cerr << "Error: --cold-tier-age-hours expects a positive integer\n";
                return 1;
            }
            cold_tier_age_hours = static_cast<int>(parsed);
        }
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
            uint32_t remote_id = 0;
            uint16_t remote_port = 0;
            if (p1 == std::string::npos || p2 == std::string::npos ||
                p1 == 0 || p2 == p1 + 1 || p2 + 1 >= spec.size() ||
                !parse_unsigned_cli(
                    std::string_view(spec).substr(0, p1), &remote_id) ||
                !parse_unsigned_cli(
                    std::string_view(spec).substr(p2 + 1), &remote_port) ||
                remote_port == 0) {
                std::cerr << "Error: --add-node expects id:host:port, got '"
                          << spec << "'\n";
                return 1;
            }
            remote_nodes.push_back({
                remote_id, spec.substr(p1 + 1, p2 - p1 - 1), remote_port});
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--port 8123] [--ticks 0] [--no-auth]\n"
                      << "       [--node-id 0] [--add-node id:host:port ...]\n"
                      << "       [--log-level info|debug|warn|error]\n"
                      << "       [--ha active|standby] [--peer host:port] [--rpc-port PORT]\n\n"
                      << "HTTP security:\n"
                      << "  --bind <host>             Listener address (default: 127.0.0.1)\n"
                      << "  --allow-plaintext-http    Allow plaintext on a non-loopback listener\n"
                      << "  --tls-cert <path>         Server certificate chain (PEM)\n"
                      << "  --tls-key <path>          Server private key (PEM; required with cert)\n"
                      << "  --tls-ca <path>           Require client certificates from this CA (mTLS)\n"
                      << "  --secure-cookie           Mark session cookies Secure behind a TLS proxy\n"
                      << "  --max-request-bytes <n>   HTTP body ceiling (default: 67108864)\n"
                      << "  --query-timeout-ms <n>    Cooperative query timeout (default: 30000; 0 disables)\n\n"
                      << "  --allow-experimental-distributed-queries\n"
                      << "                            Enable distributed SELECT without end-to-end cap/cancel\n\n"
                      << "Authentication:\n"
                      << "  --api-keys-file <path>    API-key store (or ZEPTO_API_KEYS_FILE)\n"
                      << "  --bootstrap-dev-keys      Create and print development keys (explicit opt-in)\n"
                      << "  --no-bootstrap-dev-keys   Compatibility no-op; bootstrap is off by default\n"
                      << "  --disable-rate-limit      Disable rate limiting (development only)\n"
                      << "  --disable-audit           Disable audit logging (development only)\n"
                      << "  --audit-log-file <path>   Dedicated audit log output path\n"
                      << "  --no-auth                 Disable HTTP authentication (development only)\n\n"
                      << "Cluster RPC:\n"
                      << "  Set ZEPTO_CLUSTER_SECRET_FILE (recommended) or ZEPTO_CLUSTER_SECRET\n"
                      << "  with at least 32 bytes whenever --ha or --add-node is used.\n"
                      << "  --allow-insecure-cluster  Allow unauthenticated RPC (development only)\n\n"
                      << "JWT / SSO:\n"
                      << "  --jwt-issuer <url>       Expected issuer (iss claim)\n"
                      << "  --jwt-audience <aud>     Expected audience (aud claim)\n"
                      << "  --jwt-secret <secret>    HS256 shared secret\n"
                      << "  --jwt-public-key <path>  RS256 PEM public key file\n"
                      << "  --jwks-url <url>         JWKS endpoint (auto-fetch RS256 keys)\n"
                      << "  --oidc-issuer <url>      OIDC discovery issuer (HTTPS)\n"
                      << "  --oidc-client-id <id>    OAuth2 client identifier\n"
                      << "  --oidc-client-secret <s> Client secret (argv is visible; prefer env/file)\n"
                      << "  --oidc-client-secret-env <name> Read client secret from an environment variable\n"
                      << "  --oidc-client-secret-file <path> Read client secret from a mounted file\n"
                      << "  --oidc-redirect-uri <url> Registered callback URL\n"
                      << "  --oidc-audience <aud>    Optional token audience override\n"
                      << "  ZEPTO_OIDC_CLIENT_SECRET is used when no explicit secret source is supplied.\n\n"
                      << "Storage:\n"
                      << "  --hdb-dir <path>         HDB base directory (implies --storage-mode tiered)\n"
                      << "  --storage-mode <mode>    pure (in-memory, default) | tiered (requires --hdb-dir)\n\n"
                      << "  --acknowledge-incomplete-durability\n"
                      << "                            Required for tiered/cold storage until abrupt-loss recovery is complete\n\n"
                      << "  --agent-memory-dir <path> Persist Agent Memory sidecar files\n"
                      << "                            (routed mode uses node-{id}/shard-0 under this path;\n"
                      << "                             default: <hdb-dir>/agent_memory when HDB is enabled)\n\n"
                      << "  --agent-memory-flush-every N\n"
                      << "                            Save sidecar after N memory/cache mutations\n"
                      << "                            (0 = stop only, default: 100)\n\n"
                      << "  --agent-memory-max-memories N\n"
                      << "                            Max retained memories (0 = unbounded, default: 0)\n"
                      << "  --agent-memory-max-cache-entries N\n"
                      << "                            Max retained cache entries (0 = unbounded, default: 0)\n\n"
                      << "  --agent-memory-replication-mode local|routed|quorum|sync\n"
                      << "                            Agent Memory owner WAL replica ACK policy\n"
                      << "                            (local/routed = owner only, default: routed)\n\n"
                      << "  --agent-memory-ann off|auto|sparse_projection|hnsw|ivf\n"
                      << "                            Enable ANN candidate generation for memory search\n"
                      << "                            (default: off)\n"
                      << "  --agent-memory-ann-min-records N\n"
                      << "                            Auto mode threshold (default: 50000)\n"
                      << "  --agent-memory-ann-max-candidates N\n"
                      << "                            Max ANN candidates reranked per search (default: 50000)\n\n"
                      << "  --agent-memory-ann-ivf-centroids N\n"
                      << "                            IVF centroid/list count per partition (default: 256)\n"
                      << "  --agent-memory-ann-ivf-probe N\n"
                      << "                            IVF lists probed per query (default: 8)\n\n"
                      << "  --agent-memory-ring-epoch N\n"
                      << "                            Initial routed Agent Memory ring epoch (default: 1)\n\n"
                      << "Cluster failover:\n"
                      << "  --failover-enabled       Start HealthMonitor + FailoverManager in non-HA cluster mode\n"
                      << "  --health-heartbeat-port N UDP heartbeat port (default: 9100)\n"
                      << "  --health-tcp-port N       TCP heartbeat probe port (default: 9101)\n"
                      << "  --health-suspect-ms N     Suspect timeout (default: 3000)\n"
                      << "  --health-dead-ms N        Dead timeout and failover trigger (default: 10000)\n\n"
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
        else {
            std::cerr << "Error: unknown or incomplete option '" << arg
                      << "'. Use --help for supported options.\n";
            return 1;
        }
    }

    if (bootstrap_dev_keys && no_bootstrap_dev_keys) {
        std::cerr << "Error: --bootstrap-dev-keys and --no-bootstrap-dev-keys "
                     "cannot be combined\n";
        return 1;
    }
    if (bootstrap_dev_keys && no_auth) {
        std::cerr << "Error: --bootstrap-dev-keys cannot be used with --no-auth\n";
        return 1;
    }
    if (bind_host.empty()) {
        std::cerr << "Error: --bind must not be empty\n";
        return 1;
    }
    if (log_level != "debug" && log_level != "info" &&
        log_level != "warn" && log_level != "error") {
        std::cerr << "Error: --log-level must be debug, info, warn, or error\n";
        return 1;
    }
    if (storage_mode != "pure" && storage_mode != "tiered") {
        std::cerr << "Error: --storage-mode must be pure or tiered\n";
        return 1;
    }
    if (cold_tier_format != "parquet" && cold_tier_format != "both") {
        std::cerr << "Error: --cold-tier-format must be parquet or both\n";
        return 1;
    }
    if (cold_tier_layout != "hive" && cold_tier_layout != "flat") {
        std::cerr << "Error: --cold-tier-layout must be hive or flat\n";
        return 1;
    }
    if (ha_role_str != "" && ha_role_str != "active" &&
        ha_role_str != "standby") {
        std::cerr << "Error: --ha must be active or standby\n";
        return 1;
    }
    const bool tls_enabled = !tls_cert_path.empty() || !tls_key_path.empty();
    if (tls_cert_path.empty() != tls_key_path.empty()) {
        std::cerr << "Error: --tls-cert and --tls-key must be provided together\n";
        return 1;
    }
    if (!tls_ca_path.empty() && !tls_enabled) {
        std::cerr << "Error: --tls-ca requires --tls-cert and --tls-key\n";
        return 1;
    }
    if (!tls_enabled && !is_loopback_bind_host(bind_host) &&
        !allow_plaintext_http) {
        std::cerr
            << "Error: refusing a plaintext non-loopback HTTP listener. "
               "Configure --tls-cert/--tls-key or explicitly pass "
               "--allow-plaintext-http when TLS terminates at a trusted proxy.\n";
        return 1;
    }
    const bool jwt_requested =
        !jwt_secret.empty() || !jwt_public_key.empty() || !jwks_url.empty();
    if (!jwt_secret.empty() && jwt_secret.size() < 32) {
        std::cerr << "Error: --jwt-secret must contain at least 32 bytes\n";
        return 1;
    }
    if (!jwt_public_key.empty() && !jwks_url.empty()) {
        std::cerr << "Error: --jwt-public-key and --jwks-url are mutually exclusive\n";
        return 1;
    }
    if (jwt_requested && (jwt_issuer.empty() || jwt_audience.empty())) {
        std::cerr << "Error: JWT authentication requires both --jwt-issuer and "
                     "--jwt-audience\n";
        return 1;
    }
    if (!jwks_url.empty() && !jwks_url.starts_with("https://")) {
        std::cerr << "Error: --jwks-url must use HTTPS\n";
        return 1;
    }
    if (!jwt_issuer.empty() && !jwt_issuer.starts_with("https://")) {
        std::cerr << "Error: --jwt-issuer must use HTTPS\n";
        return 1;
    }

    const bool oidc_requested =
        !oidc_issuer.empty() || !oidc_client_id.empty() ||
        oidc_secret_from_argv || !oidc_client_secret_env.empty() ||
        !oidc_client_secret_file.empty() || !oidc_redirect_uri.empty() ||
        !oidc_audience.empty();
    if (oidc_requested && no_auth) {
        std::cerr << "Error: OIDC cannot be configured with --no-auth\n";
        return 1;
    }
    const int explicit_oidc_secret_sources =
        static_cast<int>(oidc_secret_from_argv) +
        static_cast<int>(!oidc_client_secret_env.empty()) +
        static_cast<int>(!oidc_client_secret_file.empty());
    if (explicit_oidc_secret_sources > 1) {
        std::cerr
            << "Error: choose exactly one OIDC client-secret source: argv, "
               "environment variable, or file\n";
        return 1;
    }
    if (oidc_requested &&
        (oidc_issuer.empty() || oidc_client_id.empty() ||
         oidc_redirect_uri.empty())) {
        std::cerr
            << "Error: OIDC requires --oidc-issuer, --oidc-client-id, and "
               "--oidc-redirect-uri\n";
        return 1;
    }
    if (!oidc_issuer.empty() && !oidc_issuer.starts_with("https://")) {
        std::cerr << "Error: --oidc-issuer must use HTTPS\n";
        return 1;
    }
    const bool redirect_is_https = oidc_redirect_uri.starts_with("https://");
    const bool redirect_is_loopback_http =
        is_loopback_http_url(oidc_redirect_uri);
    if (!oidc_redirect_uri.empty() && !redirect_is_https &&
        !redirect_is_loopback_http) {
        std::cerr
            << "Error: --oidc-redirect-uri must use HTTPS, except for an "
               "HTTP loopback development callback\n";
        return 1;
    }
    if (oidc_requested && !tls_enabled && redirect_is_https &&
        !secure_cookie) {
        std::cerr
            << "Error: an HTTPS OIDC redirect behind a TLS proxy requires "
               "--secure-cookie\n";
        return 1;
    }

    bool ha_mode = !ha_role_str.empty();
    std::string ha_peer_host;
    uint16_t ha_peer_port = 0;
    if (ha_mode && peer_spec.empty()) {
        std::cerr << "Error: --ha requires --peer host:port\n";
        return 1;
    }
    if (ha_mode && rpc_port == 0) {
        if (port > std::numeric_limits<uint16_t>::max() - 1000) {
            std::cerr << "Error: HTTP port is too high for the default HA RPC offset\n";
            return 1;
        }
        rpc_port = port + 1000;  // default: HTTP port + 1000
    }
    if (ha_mode) {
        const auto colon = peer_spec.rfind(':');
        if (colon == std::string::npos || colon == 0 ||
            colon + 1 >= peer_spec.size() ||
            !parse_unsigned_cli(
                std::string_view(peer_spec).substr(colon + 1), &ha_peer_port) ||
            ha_peer_port == 0) {
            std::cerr << "Error: --peer expects host:port\n";
            return 1;
        }
        ha_peer_host = peer_spec.substr(0, colon);
    }
    if (!remote_nodes.empty()) {
        if (port > std::numeric_limits<uint16_t>::max() - 100) {
            std::cerr << "Error: HTTP port is too high for the cluster RPC offset\n";
            return 1;
        }
        for (const auto& remote : remote_nodes) {
            if (remote.port > std::numeric_limits<uint16_t>::max() - 100) {
                std::cerr << "Error: remote HTTP port is too high for the "
                             "cluster RPC offset: " << remote.port << "\n";
                return 1;
            }
        }
    }

    auto require_readable_file = [](const std::string& path,
                                    const char* option) -> bool {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            std::cerr << "Error: " << option << " is not a readable regular file: "
                      << path << "\n";
            return false;
        }
        std::ifstream input(path);
        if (!input.good()) {
            std::cerr << "Error: cannot read " << option << ": " << path << "\n";
            return false;
        }
        return true;
    };
    if (tls_enabled) {
        if (!require_readable_file(tls_cert_path, "--tls-cert") ||
            !require_readable_file(tls_key_path, "--tls-key")) {
            return 1;
        }
        if (!tls_ca_path.empty() &&
            !require_readable_file(tls_ca_path, "--tls-ca")) {
            return 1;
        }
    }
    if (!jwt_public_key.empty() &&
        !require_readable_file(jwt_public_key, "--jwt-public-key")) {
        return 1;
    }
    if (!oidc_client_secret_env.empty()) {
        const char* secret = std::getenv(oidc_client_secret_env.c_str());
        if (!secret || !*secret) {
            std::cerr
                << "Error: OIDC client-secret environment variable is unset "
                   "or empty\n";
            return 1;
        }
        oidc_client_secret = secret;
    } else if (!oidc_client_secret_file.empty()) {
        if (!require_readable_file(
                oidc_client_secret_file, "--oidc-client-secret-file")) {
            return 1;
        }
        std::ifstream secret_file(oidc_client_secret_file, std::ios::binary);
        oidc_client_secret.assign(
            std::istreambuf_iterator<char>(secret_file),
            std::istreambuf_iterator<char>());
        while (!oidc_client_secret.empty() &&
               (oidc_client_secret.back() == '\n' ||
                oidc_client_secret.back() == '\r')) {
            oidc_client_secret.pop_back();
        }
    } else if (oidc_requested && !oidc_secret_from_argv) {
        if (const char* secret = std::getenv("ZEPTO_OIDC_CLIENT_SECRET");
            secret && *secret) {
            oidc_client_secret = secret;
        }
    }
    if (oidc_requested && oidc_client_secret.empty()) {
        std::cerr
            << "Error: OIDC requires a non-empty client secret from "
               "--oidc-client-secret, --oidc-client-secret-env, "
               "--oidc-client-secret-file, or ZEPTO_OIDC_CLIENT_SECRET\n";
        return 1;
    }
    if (oidc_requested && oidc_audience.empty()) {
        oidc_audience = oidc_client_id;
    }
    if (oidc_secret_from_argv) {
        std::cerr
            << "WARNING: --oidc-client-secret may be visible in process "
               "arguments; prefer --oidc-client-secret-env or "
               "--oidc-client-secret-file\n";
    }
    if (allow_experimental_distributed_queries) {
        std::cerr
            << "WARNING: distributed SELECT lacks end-to-end row bounds and "
               "cancellation and is experimental\n";
    }
    if (!api_keys_file.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(api_keys_file, error);
        if (error || (!exists && !bootstrap_dev_keys)) {
            std::cerr << "Error: API-key file does not exist: "
                      << api_keys_file << "\n";
            return 1;
        }
        if (exists && !require_readable_file(api_keys_file, "--api-keys-file")) {
            return 1;
        }
    }
    if (!no_auth && api_keys_file.empty() && !jwt_requested && !oidc_requested &&
        !bootstrap_dev_keys) {
        std::cerr
            << "Error: authentication is enabled but no credential source is "
               "configured. Supply --api-keys-file (or ZEPTO_API_KEYS_FILE), "
               "configure JWT/OIDC, or explicitly use --bootstrap-dev-keys for "
               "local development.\n";
        return 1;
    }

    const bool durable_storage_requested =
        storage_mode == "tiered" || !hdb_dir.empty() || cold_tier_enabled;
    if (durable_storage_requested && !acknowledge_incomplete_durability) {
        std::cerr
            << "Error: tiered/cold storage is not fully crash durable; pass "
               "--acknowledge-incomplete-durability only after accepting "
               "the documented abrupt-loss limits\n";
        return 1;
    }
    if (durable_storage_requested && hdb_dir.empty()) {
        std::cerr << "Error: tiered/cold storage requires an explicit --hdb-dir "
                     "on durable storage\n";
        return 1;
    }
    if (!hdb_dir.empty()) {
        std::error_code error;
        std::filesystem::create_directories(hdb_dir, error);
        if (error || !std::filesystem::is_directory(hdb_dir)) {
            std::cerr << "Error: cannot create or access --hdb-dir '" << hdb_dir
                      << "': " << (error ? error.message() : "not a directory")
                      << "\n";
            return 1;
        }
        const std::string catalog_path = hdb_dir + "/_schema.json";
        zeptodb::storage::SchemaRegistry catalog_validator;
        if (catalog_validator.load_from_checked(catalog_path) ==
            zeptodb::storage::SchemaCatalogLoadResult::Invalid) {
            std::cerr << "Error: refusing to start with an invalid schema "
                         "catalog: " << catalog_path << "\n";
            return 1;
        }
    }

    const bool cluster_rpc_required = ha_mode || !remote_nodes.empty();
    auto rpc_security = zeptodb::cluster::RpcSecurityConfig::from_environment();
    if (cluster_rpc_required) {
        if (const auto error = rpc_security.validation_error(); !error.empty()) {
            std::cerr << "Error: invalid cluster RPC security configuration: "
                      << error << "\n";
            return 1;
        }
        if (!rpc_security.enabled && !allow_insecure_cluster) {
            std::cerr
                << "Error: cluster RPC authentication is required for HA and "
                   "remote nodes. Set ZEPTO_CLUSTER_SECRET_FILE (recommended) "
                   "or ZEPTO_CLUSTER_SECRET (minimum 32 bytes). Use "
                   "--allow-insecure-cluster only for isolated development.\n";
            return 1;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize logger
    zeptodb::util::LogLevel ll = zeptodb::util::LogLevel::INFO;
    if (log_level == "debug")    ll = zeptodb::util::LogLevel::DEBUG;
    else if (log_level == "warn")  ll = zeptodb::util::LogLevel::WARN;
    else if (log_level == "error") ll = zeptodb::util::LogLevel::ERROR;
    zeptodb::util::Logger::instance().init("/var/log/zeptodb", ll);
    if (no_auth) {
        std::cerr << "Warning: HTTP authentication is disabled\n";
    }
    if (!tls_enabled && !is_loopback_bind_host(bind_host)) {
        std::cerr << "Warning: serving plaintext HTTP on non-loopback address "
                  << bind_host << "; TLS must terminate at a trusted proxy\n";
    }
    if (disable_rate_limit) {
        std::cerr << "Warning: HTTP rate limiting is disabled\n";
    }
    if (disable_audit) {
        std::cerr << "Warning: HTTP audit logging is disabled\n";
    }

    // Pipeline
    zeptodb::core::PipelineConfig cfg;
    // --hdb-dir or --storage-mode tiered switches to the on-disk HDB tier.
    // Validation above requires an explicit durable path for this mode.
    if (storage_mode == "tiered" || !hdb_dir.empty()) {
        cfg.storage_mode  = zeptodb::core::StorageMode::TIERED;
        cfg.hdb_base_path = hdb_dir;
        // QueryExecutor is not yet HDB-aware for every SQL path. Keep flushed
        // partitions resident so background persistence cannot make rows
        // disappear from live SQL visibility.
        cfg.flush_config.reclaim_after_flush = false;
        // Publish a complete RDB snapshot on graceful shutdown and recover it
        // before accepting traffic on the next start. The ordinary HDB flush
        // path is not yet sufficient for general SQL restart visibility.
        const std::string recovery_path =
            (std::filesystem::path(hdb_dir) / "_rdb_snapshots").string();
        cfg.flush_config.snapshot_path = recovery_path;
        cfg.enable_recovery = true;
        cfg.recovery_snapshot_path = recovery_path;
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
        // Cold-tier needs the validated local HDB staging directory.
        if (cfg.storage_mode == zeptodb::core::StorageMode::PURE_IN_MEMORY) {
            cfg.storage_mode  = zeptodb::core::StorageMode::TIERED;
            cfg.hdb_base_path = hdb_dir;
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
    try {
        pipeline.start();
    } catch (const std::exception& error) {
        std::cerr << "Error: pipeline recovery/startup failed: "
                  << error.what() << "\n";
        return 1;
    }

    // Register default trades schema
    zeptodb::sql::QueryExecutor bootstrap_ex(pipeline);
    const auto bootstrap_schema = bootstrap_ex.execute(
        "CREATE TABLE IF NOT EXISTS trades "
        "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)");
    if (!bootstrap_schema.ok()) {
        std::cerr << "Error: failed to initialize the trades schema: "
                  << bootstrap_schema.error << "\n";
        return 1;
    }

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
    if (bootstrap_dev_keys && api_keys_file.empty()) {
        api_keys_file = "dev_keys.txt";
    }
    auth_cfg.api_keys_file = api_keys_file;
    auth_cfg.rate_limit_enabled = !disable_rate_limit;
    auth_cfg.audit_enabled = !disable_audit;
    auth_cfg.audit_buffer_enabled = !disable_audit;
    auth_cfg.audit_log_file = audit_log_file;
    auth_cfg.session_config.cookie_secure = tls_enabled || secure_cookie;
    auth_cfg.sessions_enabled = oidc_requested;
    auth_cfg.oidc_issuer = oidc_issuer;
    auth_cfg.oidc_client_id = oidc_client_id;
    auth_cfg.oidc_client_secret = oidc_client_secret;
    auth_cfg.oidc_redirect_uri = oidc_redirect_uri;
    auth_cfg.oidc_audience = oidc_audience;

    // JWT / SSO — enabled if any jwt flag is provided
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
        auth_cfg.jwks_url = jwks_url;
    } else {
        auth_cfg.jwt_enabled = false;
    }

    std::shared_ptr<zeptodb::auth::AuthManager> auth;
    try {
        auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);
    } catch (const std::exception& error) {
        std::cerr << "Error: failed to initialize HTTP authentication: "
                  << error.what() << "\n";
        return 1;
    }
    if (oidc_requested && !auth->oidc_metadata()) {
        std::cerr
            << "Error: OIDC discovery failed; refusing to start with an "
               "inactive configured identity provider\n";
        return 1;
    }

    // Development key creation is an explicit, one-time bootstrap path.
    auto existing = auth->list_api_keys();
    auto has_key = [&](const std::string& name) {
        return std::any_of(existing.begin(), existing.end(),
            [&](const auto& e) {
                return e.name == name && e.enabled && !e.is_expired();
            });
    };
    std::string admin_key, writer_key, reader_key;
    if (bootstrap_dev_keys) {
        try {
            if (!has_key("dev-admin")) {
                admin_key = auth->create_api_key(
                    "dev-admin", zeptodb::auth::Role::ADMIN);
            }
            if (!has_key("dev-writer")) {
                writer_key = auth->create_api_key(
                    "dev-writer", zeptodb::auth::Role::WRITER);
            }
            if (!has_key("dev-reader")) {
                reader_key = auth->create_api_key(
                    "dev-reader", zeptodb::auth::Role::READER);
            }
        } catch (const std::exception& error) {
            std::cerr << "Error: failed to bootstrap development API keys: "
                      << error.what() << "\n";
            return 1;
        }
        existing = auth->list_api_keys();
    }
    const bool has_active_api_key = std::any_of(
        existing.begin(), existing.end(), [](const auto& entry) {
            return entry.enabled && !entry.is_expired();
        });
    if (!no_auth && !has_active_api_key && !jwt_requested && !oidc_requested) {
        std::cerr
            << "Error: authentication is enabled but no active API key or JWT "
               "method is configured. Supply --api-keys-file, configure "
               "JWT/OIDC, "
               "or explicitly use --bootstrap-dev-keys for local development.\n";
        return 1;
    }

    // HTTP server
    zeptodb::sql::QueryExecutor executor(pipeline);
    // Cluster-aware INSERT routing (BACKLOG P8-I3-wire, devlog 111): when
    // remote_nodes is non-empty we construct a CoordinatorRoutingAdapter
    // later in main() and call executor.set_cluster_node(&adapter). Single-
    // pod deployments keep executor.cluster_node_ = nullptr (original direct-
    // to-pipeline behaviour).
    zeptodb::auth::TlsConfig tls_config;
    tls_config.enabled = tls_enabled;
    tls_config.bind_host = bind_host;
    tls_config.cert_path = tls_cert_path;
    tls_config.key_path = tls_key_path;
    tls_config.ca_cert_path = tls_ca_path;
    tls_config.https_port = port;
    zeptodb::server::HttpServer server(executor, port, tls_config, auth);
    server.set_max_request_body_bytes(max_request_bytes);
    server.set_query_timeout_ms(query_timeout_ms);
    server.set_allow_experimental_distributed_queries(
        allow_experimental_distributed_queries);
    server.set_allow_insecure_cluster(allow_insecure_cluster);
    if (agent_memory_dir.empty() && cfg.storage_mode != zeptodb::core::StorageMode::PURE_IN_MEMORY) {
        agent_memory_dir = cfg.hdb_base_path + "/agent_memory";
    }
    if (!agent_memory_dir.empty()) {
        std::string error;
        if (!server.set_agent_memory_persistence(agent_memory_dir, &error,
                                                 agent_memory_flush_every)) {
            std::cerr << "Error: failed to load agent memory snapshot: " << error << "\n";
            return 1;
        }
        std::cout << "Agent memory persistence: " << agent_memory_dir
                  << " (flush_every=" << agent_memory_flush_every << ")\n";
    }
    if (agent_memory_max_memories > 0 || agent_memory_max_cache_entries > 0) {
        zeptodb::ai::AgentMemoryEvictionConfig eviction;
        eviction.max_memories = agent_memory_max_memories;
        eviction.max_cache_entries = agent_memory_max_cache_entries;
        server.agent_memory_store().set_eviction_config(eviction);
        std::cout << "Agent memory eviction: max_memories="
                  << agent_memory_max_memories
                  << ", max_cache_entries="
                  << agent_memory_max_cache_entries
                  << ", protect_pinned=true\n";
    }
    if (agent_memory_ann != "off") {
        zeptodb::ai::AgentMemoryAnnConfig ann;
        ann.min_records = agent_memory_ann_min_records;
        ann.index.max_candidates = agent_memory_ann_max_candidates;
        ann.index.ivf_centroids = agent_memory_ann_ivf_centroids == 0
            ? 1
            : agent_memory_ann_ivf_centroids;
        ann.index.ivf_probe = agent_memory_ann_ivf_probe == 0
            ? 1
            : agent_memory_ann_ivf_probe;
        if (agent_memory_ann == "auto") {
            ann.mode = zeptodb::ai::AgentMemoryAnnMode::Auto;
        } else if (agent_memory_ann == "sparse_projection" ||
                   agent_memory_ann == "projection") {
            ann.mode = zeptodb::ai::AgentMemoryAnnMode::SparseProjection;
        } else if (agent_memory_ann == "hnsw") {
            if (!zeptodb::ai::hnsw_ann_available()) {
                std::cerr << "Error: --agent-memory-ann hnsw requires "
                          << "ZEPTO_ENABLE_HNSWLIB=ON\n";
                return 1;
            }
            ann.mode = zeptodb::ai::AgentMemoryAnnMode::Hnsw;
        } else if (agent_memory_ann == "ivf") {
            ann.mode = zeptodb::ai::AgentMemoryAnnMode::Ivf;
        } else {
            std::cerr << "Error: invalid --agent-memory-ann mode: "
                      << agent_memory_ann << "\n";
            return 1;
        }
        server.agent_memory_store().set_ann_config(ann);
        std::cout << "Agent memory ANN: " << agent_memory_ann
                  << " (min_records=" << agent_memory_ann_min_records
                  << ", max_candidates=" << agent_memory_ann_max_candidates
                  << ", ivf_centroids=" << ann.index.ivf_centroids
                  << ", ivf_probe=" << ann.index.ivf_probe
                  << ")\n";
    }
    if (agent_memory_replication_mode == "local" ||
        agent_memory_replication_mode == "routed") {
        server.set_agent_memory_replication_mode(
            zeptodb::server::AgentMemoryReplicationMode::Routed);
    } else if (agent_memory_replication_mode == "quorum") {
        server.set_agent_memory_replication_mode(
            zeptodb::server::AgentMemoryReplicationMode::Quorum);
    } else if (agent_memory_replication_mode == "sync") {
        server.set_agent_memory_replication_mode(
            zeptodb::server::AgentMemoryReplicationMode::Sync);
    } else {
        std::cerr << "Error: invalid --agent-memory-replication-mode: "
                  << agent_memory_replication_mode << "\n";
        return 1;
    }
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
    std::unordered_map<zeptodb::ai::AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>>
        agent_memory_rpc;

    // ── Non-HA: plain coordinator ──
    std::unique_ptr<zeptodb::cluster::QueryCoordinator> coordinator;
    std::atomic<uint64_t> agent_memory_ring_epoch_state{agent_memory_ring_epoch};

    if (ha_mode) {
        auto role = (ha_role_str == "active")
            ? zeptodb::cluster::CoordinatorRole::ACTIVE
            : zeptodb::cluster::CoordinatorRole::STANDBY;

        ha = std::make_unique<zeptodb::cluster::CoordinatorHA>();
        ha->init(role, ha_peer_host, ha_peer_port);

        // Register local node
        zeptodb::cluster::NodeAddress self_addr{"localhost", port, node_id};
        ha->add_local_node(self_addr, pipeline);

        // Register remote data nodes. --add-node uses peer HTTP ports, while
        // QueryCoordinator talks to the peer TCP RPC server on HTTP+100.
        for (auto& rn : remote_nodes) {
            zeptodb::cluster::NodeAddress addr{
                rn.host, static_cast<uint16_t>(rn.port + 100), rn.id};
            ha->add_remote_node(addr);
        }

        // Start RPC server for peer communication (ping/query forwarding)
        rpc_srv = std::make_unique<zeptodb::cluster::TcpRpcServer>();
        rpc_srv->set_security(rpc_security);
        rpc_srv->set_agent_memory_put_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_memory_put_rpc(data, len);
            });
        rpc_srv->set_agent_cache_store_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_cache_store_rpc(data, len);
            });
        rpc_srv->set_agent_memory_delete_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_memory_delete_rpc(data, len);
            });
        rpc_srv->set_agent_cache_delete_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_cache_delete_rpc(data, len);
            });
        rpc_srv->set_agent_memory_get_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_memory_get_rpc(data, len);
            });
        rpc_srv->set_agent_memory_search_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_memory_search_rpc(data, len);
            });
        rpc_srv->set_agent_cache_lookup_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_cache_lookup_rpc(data, len);
            });
        rpc_srv->set_agent_memory_replica_append_callback(
            [&server](const uint8_t* data, size_t len) {
                return server.handle_agent_memory_replica_append_rpc(data, len);
            });
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
            // --add-node uses peer HTTP ports, while QueryCoordinator talks to
            // the peer TCP RPC server on HTTP+100.
            zeptodb::cluster::NodeAddress addr{
                rn.host, static_cast<uint16_t>(rn.port + 100), rn.id};
            coordinator->add_remote_node(addr);
        }

        server.set_coordinator(coordinator.get(), static_cast<uint16_t>(node_id));
    }

    std::cout << "ZeptoDB HTTP server: "
              << (tls_enabled ? "https://" : "http://")
              << bind_host << ":" << port
              << "  (" << num_ticks << " sample ticks loaded, node_id=" << node_id << ")\n";
    if (!remote_nodes.empty()) {
        std::cout << "Remote nodes:\n";
        for (auto& rn : remote_nodes)
            std::cout << "  Node " << rn.id << " → " << rn.host << ":" << rn.port << "\n";
    }
    if (bootstrap_dev_keys) {
        if (!admin_key.empty() || !writer_key.empty() || !reader_key.empty()) {
            std::cout << "\n=== Dev API Keys (shown once) ===\n";
            if (!admin_key.empty())  std::cout << "  admin:  " << admin_key  << "\n";
            if (!writer_key.empty()) std::cout << "  writer: " << writer_key << "\n";
            if (!reader_key.empty()) std::cout << "  reader: " << reader_key << "\n";
            std::cout << "=================================\n";
        } else {
            std::cout << "Development API keys already exist; no secrets printed\n";
        }
    }

    // ── Cluster wire-up (when remote nodes exist) ──
    // BACKLOG P8-I3-wire (devlog 111): unify the PartitionRouter so the
    // rebalance path and the routing-adapter path both read/write the same
    // ring under the same shared_mutex, exposed by QueryCoordinator.
    std::unique_ptr<zeptodb::cluster::PartitionMigrator>       rebalance_migrator;
    std::unique_ptr<zeptodb::cluster::RebalanceManager>        rebalance_mgr;
    std::unique_ptr<zeptodb::cluster::FencingToken>            ring_fencing_token;
    std::unique_ptr<zeptodb::cluster::EpochBroadcastConsensus> ring_consensus;
    std::unique_ptr<zeptodb::cluster::FailoverManager>         failover_mgr;
    std::unique_ptr<zeptodb::cluster::HealthMonitor>           health_monitor;
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
        ring_fencing_token = std::make_unique<zeptodb::cluster::FencingToken>();
        ring_consensus = std::make_unique<zeptodb::cluster::EpochBroadcastConsensus>(
            coordinator->router(), coordinator->router_mutex(), *ring_fencing_token);
        for (auto& rn : remote_nodes) {
            ring_consensus->add_peer(
                rn.id, rn.host, static_cast<uint16_t>(rn.port + 100));
        }
        rebalance_mgr->set_consensus(ring_consensus.get());
        server.set_rebalance_manager(rebalance_mgr.get());
        std::cout << "Rebalance manager: enabled (" << (remote_nodes.size() + 1) << " nodes)\n";

        // --- Peer RPC clients: one per remote node, keyed by NodeId ---
        for (auto& rn : remote_nodes) {
            // Peer RPC port = peer HTTP port + 100 (convention from
            // cluster_node.h:436 and migrator above).
            auto rpc = std::make_shared<zeptodb::cluster::TcpRpcClient>(
                rn.host, static_cast<uint16_t>(rn.port + 100),
                /*timeout_ms=*/2000);
            rpc->set_security(rpc_security);
            peer_rpc.emplace(rn.id, rpc);
            agent_memory_rpc.emplace(static_cast<zeptodb::ai::AgentMemoryNodeId>(rn.id),
                                     rpc);
        }

        // --- Peer RPC server so other pods can forward ticks TO us ---
        // HA mode already starts an RPC server for peer SQL; non-HA needs a
        // symmetric one with a tick_cb for TICK_INGEST.
        if (!rpc_srv) {
            rpc_srv = std::make_unique<zeptodb::cluster::TcpRpcServer>();
            rpc_srv->set_security(rpc_security);
            rpc_srv->set_ring_update_callback(
                [&ring_consensus](const uint8_t* data, size_t len) {
                    return ring_consensus && ring_consensus->apply_update(data, len);
                });
            rpc_srv->set_agent_memory_put_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_memory_put_rpc(data, len);
                });
            rpc_srv->set_agent_cache_store_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_cache_store_rpc(data, len);
                });
            rpc_srv->set_agent_memory_delete_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_memory_delete_rpc(data, len);
                });
            rpc_srv->set_agent_cache_delete_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_cache_delete_rpc(data, len);
                });
            rpc_srv->set_agent_memory_get_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_memory_get_rpc(data, len);
                });
            rpc_srv->set_agent_memory_search_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_memory_search_rpc(data, len);
                });
            rpc_srv->set_agent_cache_lookup_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_cache_lookup_rpc(data, len);
                });
            rpc_srv->set_agent_memory_replica_append_callback(
                [&server](const uint8_t* data, size_t len) {
                    return server.handle_agent_memory_replica_append_rpc(data, len);
                });
            rpc_srv->set_typed_row_ingest_callback(
                [&pipeline](zeptodb::core::TypedRowMessage row) {
                    return pipeline.ingest_typed_row(std::move(row));
                });
            rpc_srv->start(
                static_cast<uint16_t>(port + 100),
                // sql_cb: scatter-gathered SQL from another coordinator
                [&](const std::string& sql) {
                    zeptodb::sql::QueryExecutor ex(pipeline);
                    return ex.execute(sql);
                },
                // tick_cb: forwarded tick lands directly in local pipeline
                [&](const zeptodb::ingestion::TickMessage& msg) {
                    const bool ok = pipeline.ingest_tick(msg);
                    if (ok) {
                        pipeline.schema_registry().mark_has_data(msg.table_id);
                        // Forwarded single-tick RPCs are synchronous from the
                        // HTTP caller's perspective, so make the row visible
                        // before reporting success.
                        pipeline.drain_sync(101);
                    }
                    return ok;
                },
                [&](const std::vector<zeptodb::ingestion::TickMessage>& batch) -> size_t {
                    size_t applied = 0;
                    for (const auto& msg : batch) {
                        if (pipeline.ingest_tick(msg)) {
                            pipeline.schema_registry().mark_has_data(msg.table_id);
                            ++applied;
                        }
                    }
                    if (applied > 0) {
                        pipeline.drain_sync(applied + 100);
                    }
                    return applied;
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
        zeptodb::ai::AgentMemoryRouterConfig memory_routing;
        memory_routing.self_node_id =
            static_cast<zeptodb::ai::AgentMemoryNodeId>(node_id);
        memory_routing.ring_epoch = agent_memory_ring_epoch_state.load();
        memory_routing.mode = zeptodb::ai::AgentMemoryRoutingMode::Routed;
        std::vector<zeptodb::ai::AgentMemoryNodeId> memory_nodes{
            static_cast<zeptodb::ai::AgentMemoryNodeId>(node_id)};
        for (auto& rn : remote_nodes) {
            memory_nodes.push_back(
                static_cast<zeptodb::ai::AgentMemoryNodeId>(rn.id));
        }
        {
            std::string error;
            if (!server.set_agent_memory_routing(memory_routing, memory_nodes,
                                                 agent_memory_rpc, &error)) {
                std::cerr << "Error: failed to enable agent memory routing: "
                          << error << "\n";
                return 1;
            }
        }
        std::cout << "Cluster routing: enabled (" << remote_nodes.size()
                  << " remote nodes)\n";
        std::cout << "Agent memory routing: enabled (" << memory_nodes.size()
                  << " nodes, ring_epoch="
                  << agent_memory_ring_epoch_state.load() << ")\n";

        if (failover_enabled) {
            zeptodb::cluster::FailoverConfig failover_config;
            failover_config.auto_re_replicate = true;
            failover_config.async_re_replicate = true;
            failover_mgr = std::make_unique<zeptodb::cluster::FailoverManager>(
                coordinator->router(), *coordinator, failover_config);
            failover_mgr->register_node(
                node_id, "127.0.0.1", static_cast<uint16_t>(port + 100));
            for (auto& rn : remote_nodes) {
                failover_mgr->register_node(
                    rn.id, rn.host, static_cast<uint16_t>(rn.port + 100));
            }
            failover_mgr->on_failover(
                [&server, &coordinator, &agent_memory_ring_epoch_state](
                    const zeptodb::cluster::FailoverEvent& event) {
                    const uint64_t source_epoch =
                        agent_memory_ring_epoch_state.fetch_add(1);
                    const uint64_t new_epoch = source_epoch + 1;
                    std::vector<zeptodb::ai::AgentMemoryNodeId> live_nodes;
                    for (const auto node : coordinator->router().all_nodes()) {
                        live_nodes.push_back(
                            static_cast<zeptodb::ai::AgentMemoryNodeId>(node));
                    }
                    auto result = server.handle_agent_memory_owner_failover(
                        static_cast<zeptodb::ai::AgentMemoryNodeId>(event.dead_node),
                        source_epoch, new_epoch, live_nodes);
                    if (result.ok) {
                        std::cout << "Agent memory failover: dead_node="
                                  << event.dead_node
                                  << ", replacement="
                                  << result.replacement_node_id
                                  << ", adopted="
                                  << (result.adopted ? "true" : "false")
                                  << ", ring_epoch=" << new_epoch << "\n";
                    } else {
                        std::cerr << "Agent memory failover failed: dead_node="
                                  << event.dead_node
                                  << ", error=" << result.error << "\n";
                    }
                });

            zeptodb::cluster::HealthConfig health_config;
            health_config.heartbeat_port = health_heartbeat_port;
            health_config.tcp_heartbeat_port = health_tcp_port;
            health_config.suspect_timeout_ms = health_suspect_ms;
            health_config.dead_timeout_ms = health_dead_ms;
            health_monitor =
                std::make_unique<zeptodb::cluster::HealthMonitor>(health_config);
            failover_mgr->connect(*health_monitor);
            std::vector<zeptodb::cluster::NodeAddress> peers;
            peers.reserve(remote_nodes.size());
            for (auto& rn : remote_nodes) {
                peers.push_back({rn.host, rn.port, rn.id});
            }
            health_monitor->start({"127.0.0.1", port, node_id}, peers);
            std::cout << "Failover manager: enabled (heartbeat_port="
                      << health_heartbeat_port
                      << ", tcp_heartbeat_port=" << health_tcp_port
                      << ", suspect_ms=" << health_suspect_ms
                      << ", dead_ms=" << health_dead_ms << ")\n";
        }
    }

    if (!server.start_async()) {
        std::cerr << "Error: failed to bind HTTP listener on " << bind_host
                  << ":" << port << "\n";
        if (health_monitor) health_monitor->stop();
        if (ha) ha->stop();
        if (rpc_srv) rpc_srv->stop();
        if (!pipeline.stop()) {
            std::cerr << "Error: graceful pipeline shutdown could not publish "
                         "the final recovery snapshot\n";
        }
        return 1;
    }

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (health_monitor) health_monitor->stop();
    if (ha) ha->stop();
    if (rpc_srv) rpc_srv->stop();
    server.stop();
    if (!pipeline.stop()) {
        std::cerr << "Error: graceful pipeline shutdown could not publish "
                     "the final recovery snapshot\n";
        return 1;
    }
    return 0;
}
