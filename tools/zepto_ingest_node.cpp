// zepto_ingest_node — stateless HTTP ingest pod (P8-I3, devlog 113)
// ============================================================================
// Receives HTTP INSERT and forwards every tick to the correct storage pod via
// CoordinatorRoutingAdapter (devlog 111). Owns zero data.
//
// Design decision (Option A): self node_id defaults to 65534, a value that
// no real storage pod uses. The consistent-hash `PartitionRouter::route()`
// therefore never returns `self_id`, so every tick flows through a remote
// `TcpRpcClient` in the peer pool. The local ZeptoPipeline is only wired so
// the executor's SchemaRegistry can resolve `table_name → table_id` during
// `exec_insert`; it never stores data.
//
// Related binaries:
//   - zepto_http_server : full HTTP + local storage (+ optional HA)
//   - zepto_data_node   : storage-only leaf (SQL + tick RPC over TCP)
//   - zepto_ingest_node : ingest-only front-door (this file)
// ============================================================================
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/coordinator_routing_adapter.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/util/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct RemoteNodeSpec {
    uint32_t    id;
    std::string host;
    uint16_t    port;
};

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false); }

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  --port PORT               HTTP port (default: 8124)\n"
        << "  --node-id N               Self node ID (default: 99999, must not\n"
        << "                            collide with any storage node ID)\n"
        << "  --add-node id:host:port   Storage node to forward to (repeatable)\n"
        << "  --no-auth                 Disable authentication (bench / dev mode)\n"
        << "  --log-level LEVEL         info|debug|warn|error (default: info)\n"
        << "  -h, --help                Show this help and exit\n\n"
        << "Example (forward INSERTs to two storage pods):\n"
        << "  " << prog << " --port 8124 --no-auth \\\n"
        << "      --add-node 0:storage-0.zeptodb-headless:8123 \\\n"
        << "      --add-node 1:storage-1.zeptodb-headless:8123\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    uint16_t    port    = 8124;     // distinct from 8123 to avoid local collision
    uint32_t    node_id = 65534;    // must not collide with storage pod IDs; fits uint16_t
    bool        no_auth = false;
    std::string log_level = "info";
    std::vector<RemoteNodeSpec> remote_nodes;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--no-auth") {
            no_auth = true;
        } else if (arg == "--log-level" && i + 1 < argc) {
            log_level = argv[++i];
        } else if (arg == "--add-node" && i + 1 < argc) {
            std::string spec = argv[++i];
            auto p1 = spec.find(':');
            auto p2 = spec.find(':', p1 == std::string::npos ? 0 : p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) {
                std::cerr << "Error: --add-node expects id:host:port, got '"
                          << spec << "'\n";
                return 1;
            }
            remote_nodes.push_back({
                static_cast<uint32_t>(std::stoi(spec.substr(0, p1))),
                spec.substr(p1 + 1, p2 - p1 - 1),
                static_cast<uint16_t>(std::stoi(spec.substr(p2 + 1)))
            });
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Error: unknown argument '" << arg << "'\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (remote_nodes.empty()) {
        std::cerr << "Error: zepto_ingest_node requires at least one "
                     "--add-node id:host:port (nothing to forward to)\n";
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Logger
    zeptodb::util::LogLevel ll = zeptodb::util::LogLevel::INFO;
    if (log_level == "debug")      ll = zeptodb::util::LogLevel::DEBUG;
    else if (log_level == "warn")  ll = zeptodb::util::LogLevel::WARN;
    else if (log_level == "error") ll = zeptodb::util::LogLevel::ERROR;
    zeptodb::util::Logger::instance().init("/var/log/zeptodb", ll);

    // Minimal pipeline — needed only for SchemaRegistry (table_name → table_id
    // resolution inside QueryExecutor::exec_insert). The routing adapter
    // never calls local_->ingest_tick() because self_id=99999 is not in the
    // ring, so no data is ever stored here.
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode         = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    pcfg.drain_threads        = 1;      // minimal; no ingest lands locally
    pcfg.ring_buffer_capacity = 4096;   // minimum allowed
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    // QueryCoordinator — owns the shared PartitionRouter that the adapter
    // reads and the forward_ddl_to_remotes() path that replicates CREATE /
    // DROP / ALTER to every storage pod.
    auto coordinator = std::make_unique<zeptodb::cluster::QueryCoordinator>();
    zeptodb::cluster::NodeAddress self_addr{"localhost", port, node_id};
    coordinator->add_local_node(self_addr, pipeline);
    // CRITICAL: remove self from the hash ring so route() never returns
    // self_id. add_local_node registered the endpoint (needed for DDL
    // execution + SchemaRegistry) but also inserted self into the ring.
    // An ingest node must forward ALL ticks to storage pods — if self
    // stays in the ring, ~1/(N+1) of INSERTs silently land in the
    // ephemeral local pipeline and are lost on pod restart.
    {
        auto wlock = coordinator->router_write_lock();
        coordinator->router().remove_node(node_id);
    }
    for (const auto& rn : remote_nodes) {
        zeptodb::cluster::NodeAddress addr{rn.host, rn.port, rn.id};
        coordinator->add_remote_node(addr);
    }

    // Peer RPC clients — one per storage node. Convention from
    // zepto_http_server / cluster_node.h: peer RPC port = peer HTTP port + 100.
    std::unordered_map<zeptodb::cluster::NodeId,
                       std::shared_ptr<zeptodb::cluster::RpcClientBase>> peer_rpc;
    for (const auto& rn : remote_nodes) {
        peer_rpc.emplace(rn.id,
            std::make_shared<zeptodb::cluster::TcpRpcClient>(
                rn.host,
                static_cast<uint16_t>(rn.port + 100),
                /*timeout_ms=*/2000));
    }

    // Routing adapter — self_id=99999 means route() always picks a storage
    // node (see Option A in devlog 113).
    auto routing_adapter =
        std::make_unique<zeptodb::cluster::CoordinatorRoutingAdapter>(
            &coordinator->router(),
            &coordinator->router_mutex(),
            &pipeline,
            static_cast<zeptodb::cluster::NodeId>(node_id),
            &peer_rpc);

    // Auth — minimal. --no-auth is the default for the bench image.
    zeptodb::auth::AuthManager::Config auth_cfg;
    auth_cfg.enabled            = !no_auth;
    auth_cfg.rate_limit_enabled = false;
    auth_cfg.audit_enabled      = false;
    auth_cfg.audit_buffer_enabled = false;
    auto auth = std::make_shared<zeptodb::auth::AuthManager>(auth_cfg);

    // Executor wired to the cluster routing adapter so INSERT forwards via RPC.
    zeptodb::sql::QueryExecutor executor(pipeline);
    executor.set_cluster_node(routing_adapter.get());

    // HTTP server. Attaching the coordinator enables DDL replication
    // (devlog 112): CREATE / DROP / ALTER TABLE executes locally (updating
    // our SchemaRegistry so the executor can resolve table IDs) and then
    // fire-and-forget replicates to every storage pod.
    zeptodb::server::HttpServer server(executor, port,
                                       zeptodb::auth::TlsConfig{}, auth);
    server.set_coordinator(coordinator.get(), static_cast<uint16_t>(node_id));
    server.set_ready(true);

    std::cout << "ZeptoDB ingest node: http://localhost:" << port
              << " (stateless, node_id=" << node_id
              << ", forwarding to " << remote_nodes.size()
              << " storage node(s))\n";
    for (const auto& rn : remote_nodes) {
        std::cout << "  Storage node " << rn.id << " → " << rn.host
                  << ":" << rn.port << " (rpc " << rn.host << ":"
                  << (rn.port + 100) << ")\n";
    }
    std::cout << std::flush;

    server.start_async();

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    return 0;
}
