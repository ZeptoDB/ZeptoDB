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
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <limits>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

namespace {

bool parse_port(const char* text, uint16_t* port) {
    unsigned int parsed = 0;
    const std::string_view value{text};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed == 0 || parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    *port = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_nonnegative_int(const char* text, int* value) {
    int parsed = 0;
    const std::string_view input{text};
    const auto result = std::from_chars(
        input.data(), input.data() + input.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
        parsed < 0) {
        return false;
    }
    *value = parsed;
    return true;
}

bool ascii_case_equal(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
            std::tolower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

bool is_loopback_host(std::string_view host) {
    if (ascii_case_equal(host, "localhost") || host == "::1" ||
        host == "[::1]") {
        return true;
    }
    std::array<unsigned int, 4> octets{};
    size_t start = 0;
    for (size_t index = 0; index < octets.size(); ++index) {
        const size_t end = host.find('.', start);
        if ((index < octets.size() - 1 && end == std::string_view::npos) ||
            (index == octets.size() - 1 && end != std::string_view::npos)) {
            return false;
        }
        const auto segment = host.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                  : end - start);
        if (segment.empty() || segment.size() > 3) return false;
        unsigned int value = 0;
        for (const unsigned char ch : segment) {
            if (std::isdigit(ch) == 0) return false;
            value = value * 10 + static_cast<unsigned int>(ch - '0');
        }
        if (value > 255) return false;
        octets[index] = value;
        start = end == std::string_view::npos ? host.size() : end + 1;
    }
    return octets[0] == 127;
}

}  // namespace

int main(int argc, char* argv[]) {
    uint16_t http_port   = 8123;
    uint16_t flight_port = 8815;
    int num_ticks = 0;
    int query_timeout_ms = 30000;
    bool no_auth = false;
    bool bootstrap_dev_keys = false;
    bool allow_insecure_flight = false;
    bool allow_plaintext_http = false;
    bool allow_non_atomic_put = false;
    std::string flight_host = "127.0.0.1";
    std::string tls_cert_path;
    std::string tls_key_path;
    std::string api_keys_file;

    if (const char* value = std::getenv("ZEPTO_API_KEYS_FILE");
        value && *value) {
        api_keys_file = value;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--http-port" && i + 1 < argc) {
            if (!parse_port(argv[++i], &http_port)) {
                std::cerr << "Invalid --http-port value\n";
                return 2;
            }
        } else if ((arg == "--flight-port" || arg == "--port") &&
                   i + 1 < argc) {
            if (!parse_port(argv[++i], &flight_port)) {
                std::cerr << "Invalid Flight port value\n";
                return 2;
            }
        } else if (arg == "--flight-host" && i + 1 < argc) {
            flight_host = argv[++i];
        } else if ((arg == "--ticks" || arg == "-n") && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &num_ticks)) {
                std::cerr << "Invalid --ticks value\n";
                return 2;
            }
        } else if (arg == "--tls-cert" && i + 1 < argc) {
            tls_cert_path = argv[++i];
        } else if (arg == "--tls-key" && i + 1 < argc) {
            tls_key_path = argv[++i];
        } else if (arg == "--api-keys-file" && i + 1 < argc) {
            api_keys_file = argv[++i];
        } else if (arg == "--allow-insecure-flight") {
            allow_insecure_flight = true;
        } else if (arg == "--allow-plaintext-http") {
            allow_plaintext_http = true;
        } else if (arg == "--allow-non-atomic-put") {
            allow_non_atomic_put = true;
        } else if (arg == "--query-timeout-ms" && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &query_timeout_ms)) {
                std::cerr << "Invalid --query-timeout-ms value\n";
                return 2;
            }
        } else if (arg == "--no-auth") {
            no_auth = true;
        } else if (arg == "--bootstrap-dev-keys") {
            bootstrap_dev_keys = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                      << "Usage: " << argv[0] << " [OPTIONS]\n\n"
                      << "Starts both HTTP (ClickHouse compat) and Arrow Flight servers.\n"
                      << "  --flight-host HOST       Flight bind host (default: 127.0.0.1)\n"
                      << "  --flight-port PORT       Flight port (default: 8815)\n"
                      << "  --port PORT              Alias for --flight-port\n"
                      << "  --http-port PORT         Bundled HTTP port (default: 8123)\n"
                      << "  --tls-cert PATH          PEM server certificate for Flight and HTTP\n"
                      << "  --tls-key PATH           PEM private key for Flight and HTTP\n"
                      << "  --api-keys-file PATH     API key store (or ZEPTO_API_KEYS_FILE)\n"
                      << "  --bootstrap-dev-keys     Create and print development keys (explicit opt-in)\n"
                      << "  --query-timeout-ms N     Cooperative read timeout; 0 disables (default: 30000)\n"
                      << "  --no-auth                Disable auth (development only)\n"
                      << "  --allow-insecure-flight  Allow plaintext non-loopback Flight (development only)\n"
                      << "  --allow-plaintext-http   Allow bundled plaintext HTTP on non-loopback (development only)\n"
                      << "  --allow-non-atomic-put   Enable experimental non-atomic Flight DoPut\n"
                      << "  --ticks N, -n N          Seed sample rows (default: 0)\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << '\n';
            return 2;
        }
    }

    if (tls_cert_path.empty() != tls_key_path.empty()) {
        std::cerr << "--tls-cert and --tls-key must be provided together\n";
        return 2;
    }
    if (bootstrap_dev_keys && no_auth) {
        std::cerr
            << "--bootstrap-dev-keys cannot be combined with --no-auth\n";
        return 2;
    }
    if (bootstrap_dev_keys && api_keys_file.empty()) {
        api_keys_file = "dev_keys.txt";
    }
    if (!no_auth && api_keys_file.empty()) {
        std::cerr
            << "Authentication is enabled but no credential source is "
               "configured; supply --api-keys-file (or "
               "ZEPTO_API_KEYS_FILE), use --bootstrap-dev-keys for local "
               "development, or explicitly use --no-auth\n";
        return 2;
    }
    if (!api_keys_file.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(api_keys_file, error);
        if (error || (!exists && !bootstrap_dev_keys)) {
            std::cerr << "API-key file does not exist: "
                      << api_keys_file << '\n';
            return 2;
        }
        if (exists &&
            (!std::filesystem::is_regular_file(api_keys_file, error) ||
             error)) {
            std::cerr << "API-key store is not a readable regular file: "
                      << api_keys_file << '\n';
            return 2;
        }
        if (exists) {
            std::ifstream input(api_keys_file);
            if (!input.good()) {
                std::cerr << "API-key store is not readable: "
                          << api_keys_file << '\n';
                return 2;
            }
        }
    }
    if (tls_cert_path.empty() && !is_loopback_host(flight_host) &&
        !allow_plaintext_http) {
        std::cerr
            << "Bundled plaintext HTTP is restricted to loopback; configure "
               "TLS or use --allow-plaintext-http explicitly\n";
        return 2;
    }
    if (no_auth) {
        std::cerr << "WARNING: --no-auth is a development-only mode\n";
    }
    if (bootstrap_dev_keys) {
        std::cerr
            << "WARNING: --bootstrap-dev-keys is a development-only mode\n";
    }
    if (allow_insecure_flight) {
        std::cerr
            << "WARNING: --allow-insecure-flight exposes plaintext Flight\n";
    }
    if (allow_plaintext_http) {
        std::cerr
            << "WARNING: --allow-plaintext-http exposes plaintext HTTP\n";
    }
    if (allow_non_atomic_put) {
        std::cerr
            << "WARNING: Flight DoPut is non-atomic and may partially commit\n";
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
    auth_cfg.api_keys_file = api_keys_file;
    std::shared_ptr<zeptodb::auth::AuthManager> auth;
    try {
        auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);
    } catch (const std::exception& error) {
        std::cerr << "Failed to initialize authentication: "
                  << error.what() << '\n';
        return 1;
    }
    auto existing_keys = auth->list_api_keys();
    auto has_active_key = [&existing_keys]() {
        return std::any_of(
            existing_keys.begin(), existing_keys.end(),
            [](const auto& entry) {
                return entry.enabled && !entry.is_expired();
            });
    };
    if (bootstrap_dev_keys && !has_active_key()) {
        try {
            const std::string admin_key = auth->create_api_key(
                "dev-admin", zeptodb::auth::Role::ADMIN);
            const std::string reader_key = auth->create_api_key(
                "dev-reader", zeptodb::auth::Role::READER);
            std::cout
                << "Dev API Keys (shown once; local development only):\n"
                << "  admin:  " << admin_key << '\n'
                << "  reader: " << reader_key << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Failed to bootstrap development API keys: "
                      << error.what() << '\n';
            return 1;
        }
        existing_keys = auth->list_api_keys();
    }
    if (!no_auth && !has_active_key()) {
        std::cerr
            << "Authentication is enabled but the API-key store contains no "
               "active credentials\n";
        return 1;
    }
    auto tenant_manager =
        std::make_shared<zeptodb::auth::TenantManager>();

    zeptodb::auth::TlsConfig http_tls;
    http_tls.enabled = !tls_cert_path.empty();
    http_tls.bind_host = flight_host;
    http_tls.cert_path = tls_cert_path;
    http_tls.key_path = tls_key_path;
    http_tls.https_port = http_port;

    zeptodb::server::HttpServer http_server(executor, http_port,
                                            std::move(http_tls), auth);
    http_server.set_tenant_manager(tenant_manager);
    http_server.set_query_timeout_ms(
        static_cast<uint32_t>(query_timeout_ms));

    // Arrow Flight server
    zeptodb::server::FlightServer flight_server(
        executor, auth, tenant_manager);
    zeptodb::server::FlightServerConfig flight_config;
    flight_config.host = flight_host;
    flight_config.port = flight_port;
    flight_config.tls_cert_path = tls_cert_path;
    flight_config.tls_key_path = tls_key_path;
    flight_config.allow_insecure_non_loopback = allow_insecure_flight;
    flight_config.query_timeout_ms =
        static_cast<uint32_t>(query_timeout_ms);
    flight_config.allow_non_atomic_put = allow_non_atomic_put;
    if (!flight_server.start_async(flight_config)) {
        std::cerr << "Failed to start Arrow Flight: "
                  << flight_server.last_error() << '\n';
        return 1;
    }

    http_server.set_ready(true);
    if (!http_server.start_async()) {
        std::cerr << "Failed to start bundled HTTP server on "
                  << flight_host << ':' << http_port << '\n';
        flight_server.stop();
        return 1;
    }

    const bool tls_enabled = !tls_cert_path.empty();
    std::cout << "ZeptoDB servers started (" << num_ticks << " sample ticks)\n"
              << "  HTTP:   " << (tls_enabled ? "https" : "http")
              << "://localhost:" << http_port << "\n"
              << "  Flight: " << (tls_enabled ? "grpc+tls" : "grpc")
              << "://" << flight_host << ':' << flight_server.port() << "\n"
              << "\nPython example:\n"
              << "  import pyarrow.flight as fl\n"
              << "  client = fl.connect('"
              << (tls_enabled ? "grpc+tls" : "grpc") << "://localhost:"
              << flight_server.port() << "')\n"
              << "  opts = fl.FlightCallOptions(headers=[(b'authorization', b'Bearer <key>')])\n"
              << "  reader = client.do_get(fl.Ticket('SELECT * FROM trades LIMIT 5'), opts)\n"
              << "  print(reader.read_all().to_pandas())\n";

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    flight_server.stop();
    http_server.stop();
    return 0;
}
