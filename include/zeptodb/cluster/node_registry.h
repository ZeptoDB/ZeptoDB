#pragma once
// ============================================================================
// ZeptoDB: NodeRegistry — pluggable node membership management
// ============================================================================
// Two implementations:
//   GossipNodeRegistry  — bare-metal / on-prem (UDP heartbeat, zero deps)
//   K8sNodeRegistry     — cloud / Kubernetes (API server watch, lease-based)
//
// Usage:
//   auto reg = make_node_registry(mode, config);
//   reg->start(self);
//   auto nodes = reg->active_nodes();
//   reg->on_change([](NodeId, NodeEvent) { ... });
// ============================================================================

#include "zeptodb/cluster/transport.h"
#include "zeptodb/cluster/health_monitor.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// NodeEvent: membership change events
// ============================================================================
enum class NodeEvent : uint8_t {
    JOINED,     // new node became active
    LEFT,       // graceful departure
    FAILED,     // node declared dead
    SUSPECTED,  // node suspected (may recover)
};

inline const char* node_event_str(NodeEvent e) {
    switch (e) {
        case NodeEvent::JOINED:    return "JOINED";
        case NodeEvent::LEFT:      return "LEFT";
        case NodeEvent::FAILED:    return "FAILED";
        case NodeEvent::SUSPECTED: return "SUSPECTED";
    }
    return "???";
}

// ============================================================================
// NodeInfo: per-node metadata
// ============================================================================
struct NodeInfo {
    NodeAddress address;
    NodeState   state = NodeState::UNKNOWN;
    uint64_t    last_seen_ns = 0;  // nanosecond timestamp
};

// ============================================================================
// NodeRegistry: abstract interface
// ============================================================================
using NodeChangeCallback = std::function<void(NodeId, NodeEvent)>;

class NodeRegistry {
public:
    virtual ~NodeRegistry() = default;

    /// Start the registry (begin discovery / heartbeats)
    virtual void start(const NodeAddress& self,
                       const std::vector<NodeAddress>& seeds = {}) = 0;

    /// Stop the registry (graceful leave)
    virtual void stop() = 0;

    /// List currently active nodes
    virtual std::vector<NodeInfo> active_nodes() const = 0;

    /// Get specific node info (nullopt if unknown)
    virtual std::optional<NodeInfo> get_node(NodeId id) const = 0;

    /// Total known nodes (all states)
    virtual size_t node_count() const = 0;

    /// Register callback for membership changes
    virtual void on_change(NodeChangeCallback cb) = 0;

    /// Check if running
    virtual bool is_running() const = 0;
};

// ============================================================================
// GossipNodeRegistry: bare-metal / on-prem (wraps HealthMonitor)
// ============================================================================
class GossipNodeRegistry : public NodeRegistry {
public:
    explicit GossipNodeRegistry(HealthConfig cfg = {}) : health_(cfg) {}

    void start(const NodeAddress& self,
               const std::vector<NodeAddress>& seeds = {}) override {
        self_ = self;
        {
            std::lock_guard<std::mutex> lock(mu_);
            nodes_[self.id] = {self, NodeState::ACTIVE, now_ns()};
            for (auto& s : seeds)
                nodes_[s.id] = {s, NodeState::JOINING, now_ns()};
        }

        health_.start(self, seeds);
        health_.on_state_change([this](NodeId id, NodeState old_s, NodeState new_s) {
            on_state_change(id, old_s, new_s);
        });
        running_.store(true);
    }

    void stop() override {
        if (!running_.load()) return;
        health_.mark_leaving(self_.id);
        health_.stop();
        running_.store(false);
    }

    std::vector<NodeInfo> active_nodes() const override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<NodeInfo> result;
        for (auto& [id, info] : nodes_) {
            if (info.state == NodeState::ACTIVE)
                result.push_back(info);
        }
        return result;
    }

    std::optional<NodeInfo> get_node(NodeId id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = nodes_.find(id);
        if (it == nodes_.end()) return std::nullopt;
        return it->second;
    }

    size_t node_count() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return nodes_.size();
    }

    void on_change(NodeChangeCallback cb) override {
        std::lock_guard<std::mutex> lock(mu_);
        callbacks_.push_back(std::move(cb));
    }

    bool is_running() const override { return running_.load(); }

    HealthMonitor& health() { return health_; }

private:
    void on_state_change(NodeId id, NodeState old_s, NodeState new_s) {
        NodeEvent event;
        if (new_s == NodeState::ACTIVE && old_s == NodeState::JOINING)
            event = NodeEvent::JOINED;
        else if (new_s == NodeState::DEAD)
            event = NodeEvent::FAILED;
        else if (new_s == NodeState::LEAVING)
            event = NodeEvent::LEFT;
        else if (new_s == NodeState::SUSPECT)
            event = NodeEvent::SUSPECTED;
        else
            return;

        {
            std::lock_guard<std::mutex> lock(mu_);
            nodes_[id].state = new_s;
            nodes_[id].last_seen_ns = now_ns();
        }

        // Fire callbacks outside lock
        std::vector<NodeChangeCallback> cbs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = callbacks_;
        }
        for (auto& cb : cbs) cb(id, event);
    }

    static uint64_t now_ns() {
        return static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count());
    }

    HealthMonitor health_;
    NodeAddress   self_{};
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::unordered_map<NodeId, NodeInfo> nodes_;
    std::vector<NodeChangeCallback> callbacks_;
};

// ============================================================================
// K8sNodeRegistry: Kubernetes-based (API server + Lease)
// ============================================================================
// In production, this polls the K8s API for StatefulSet pod endpoints.
// For now: a lightweight implementation that accepts manual registration
// and simulates K8s-style behavior (pod discovery via seed list).
//
// Full K8s integration requires:
//   - GET /api/v1/namespaces/{ns}/endpoints/{svc} (watch)
//   - Lease API for coordinator election
//   - Service account token auth
// ============================================================================
// ============================================================================
// K8sConfig: Kubernetes registry configuration
// ============================================================================
struct K8sConfig {
    std::string namespace_name = "default";
    std::string service_name   = "zeptodb";
    std::string label_selector = "app=zeptodb";
    uint32_t    poll_interval_ms = 5000;  // endpoint poll interval
    std::string api_host = "";            // K8s API server (empty = auto-detect from env)
    uint16_t    api_port = 443;
    std::string token_path = "/var/run/secrets/kubernetes.io/serviceaccount/token";
    std::string ca_cert_path = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
    uint16_t    rpc_port_offset = 100;    // pod port + offset = RPC port
};

class K8sNodeRegistry : public NodeRegistry {
public:
    explicit K8sNodeRegistry(K8sConfig cfg = K8sConfig{}) : cfg_(std::move(cfg)) {}

    void start(const NodeAddress& self,
               const std::vector<NodeAddress>& seeds = {}) override {
        self_ = self;
        {
            std::lock_guard<std::mutex> lock(mu_);
            nodes_[self.id] = {self, NodeState::ACTIVE, now_ns()};
            for (auto& s : seeds)
                nodes_[s.id] = {s, NodeState::ACTIVE, now_ns()};
        }
        for (auto& s : seeds)
            fire_event(s.id, NodeEvent::JOINED);

        running_ = true;

        // Start endpoint polling thread
        poll_thread_ = std::thread([this] { poll_loop(); });
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        if (poll_thread_.joinable()) poll_thread_.join();
    }

    std::vector<NodeInfo> active_nodes() const override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<NodeInfo> result;
        for (auto& [id, info] : nodes_) {
            if (info.state == NodeState::ACTIVE)
                result.push_back(info);
        }
        return result;
    }

    std::optional<NodeInfo> get_node(NodeId id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = nodes_.find(id);
        if (it == nodes_.end()) return std::nullopt;
        return it->second;
    }

    size_t node_count() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return nodes_.size();
    }

    void on_change(NodeChangeCallback cb) override {
        std::lock_guard<std::mutex> lock(mu_);
        callbacks_.push_back(std::move(cb));
    }

    bool is_running() const override { return running_; }

    /// Parse K8s Endpoints JSON response into NodeAddress list (public for testing).
    static std::vector<NodeAddress> parse_endpoints_json(const std::string& json) {
        std::vector<NodeAddress> result;
        // Find port from first "port": <number> in subsets.ports
        uint16_t svc_port = 0;
        auto port_key = json.find("\"port\":");
        if (port_key != std::string::npos) {
            auto num_start = port_key + 7;
            while (num_start < json.size() && !std::isdigit(json[num_start])) num_start++;
            svc_port = static_cast<uint16_t>(std::stoi(json.substr(num_start)));
        }
        // Find all "ip": "x.x.x.x" entries
        std::string needle = "\"ip\":";
        size_t pos = 0;
        while ((pos = json.find(needle, pos)) != std::string::npos) {
            pos += needle.size();
            auto q1 = json.find('"', pos);
            if (q1 == std::string::npos) break;
            auto q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string ip = json.substr(q1 + 1, q2 - q1 - 1);
            NodeId id = 0;
            for (char c : ip) id = id * 31 + static_cast<uint8_t>(c);
            result.push_back({ip, svc_port, id});
            pos = q2 + 1;
        }
        return result;
    }

    /// Manual node registration (for testing or static config fallback)
    void register_node(const NodeAddress& addr) {
        bool is_new = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            is_new = (nodes_.find(addr.id) == nodes_.end());
            nodes_[addr.id] = {addr, NodeState::ACTIVE, now_ns()};
        }
        if (is_new) fire_event(addr.id, NodeEvent::JOINED);
    }

    /// Manual node removal
    void deregister_node(NodeId id) {
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = nodes_.find(id);
            if (it != nodes_.end()) {
                it->second.state = NodeState::LEAVING;
                nodes_.erase(it);
                found = true;
            }
        }
        if (found) fire_event(id, NodeEvent::LEFT);
    }

private:
    void poll_loop() {
        // Auto-detect K8s API host from environment
        if (cfg_.api_host.empty()) {
            const char* host = std::getenv("KUBERNETES_SERVICE_HOST");
            const char* port = std::getenv("KUBERNETES_SERVICE_PORT");
            if (host) cfg_.api_host = host;
            if (port) cfg_.api_port = static_cast<uint16_t>(std::stoi(port));
        }

        while (running_) {
            if (!cfg_.api_host.empty()) {
                auto discovered = fetch_endpoints();
                reconcile(discovered);
            }
            // Sleep in small increments so stop() is responsive
            for (uint32_t elapsed = 0;
                 elapsed < cfg_.poll_interval_ms && running_;
                 elapsed += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    /// Fetch endpoints from K8s API. Returns list of {host, port, id}.
    /// ID is derived from pod IP hash for stable identity.
    std::vector<NodeAddress> fetch_endpoints() {
        std::vector<NodeAddress> result;

        // Read service account token
        std::string token;
        {
            std::ifstream f(cfg_.token_path);
            if (!f.is_open()) return result;
            std::getline(f, token);
        }

        // Build HTTP request
        std::string path = "/api/v1/namespaces/" + cfg_.namespace_name
                         + "/endpoints/" + cfg_.service_name;
        std::string req = "GET " + path + " HTTP/1.1\r\n"
                        + "Host: " + cfg_.api_host + "\r\n"
                        + "Authorization: Bearer " + token + "\r\n"
                        + "Accept: application/json\r\n"
                        + "Connection: close\r\n\r\n";

        // TCP connect to K8s API server
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return result;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg_.api_port);
        if (::inet_pton(AF_INET, cfg_.api_host.c_str(), &addr.sin_addr) <= 0) {
            // Try hostname resolution
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (::getaddrinfo(cfg_.api_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                ::close(fd);
                return result;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            ::freeaddrinfo(res);
        }

        // Set connect timeout
        struct timeval tv{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return result;
        }

        // Send request (plain HTTP — in production, use TLS via ca_cert_path)
        ::send(fd, req.data(), req.size(), 0);

        // Read response
        std::string body;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
            body.append(buf, static_cast<size_t>(n));
        ::close(fd);

        // Skip HTTP headers
        auto body_start = body.find("\r\n\r\n");
        if (body_start == std::string::npos) return result;
        body = body.substr(body_start + 4);

        // Minimal JSON parsing: extract "ip" and "port" from subsets[].addresses[]
        result = parse_endpoints_json(body);
        return result;
    }

    /// Diff discovered endpoints with current nodes_ and fire events.
    void reconcile(const std::vector<NodeAddress>& discovered) {
        std::unordered_map<NodeId, NodeAddress> disc_map;
        for (auto& a : discovered) disc_map[a.id] = a;

        // Detect new nodes (JOINED)
        std::vector<std::pair<NodeId, NodeAddress>> joined;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [id, addr] : disc_map) {
                if (id == self_.id) continue;  // skip self
                if (nodes_.find(id) == nodes_.end()) {
                    nodes_[id] = {addr, NodeState::ACTIVE, now_ns()};
                    joined.push_back({id, addr});
                } else {
                    nodes_[id].last_seen_ns = now_ns();
                    nodes_[id].state = NodeState::ACTIVE;
                }
            }
        }
        for (auto& [id, _] : joined)
            fire_event(id, NodeEvent::JOINED);

        // Detect removed nodes (LEFT)
        std::vector<NodeId> left;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto it = nodes_.begin(); it != nodes_.end(); ) {
                if (it->first != self_.id && disc_map.find(it->first) == disc_map.end()) {
                    left.push_back(it->first);
                    it = nodes_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto id : left)
            fire_event(id, NodeEvent::LEFT);
    }

    void fire_event(NodeId id, NodeEvent event) {
        std::vector<NodeChangeCallback> cbs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = callbacks_;
        }
        for (auto& cb : cbs) cb(id, event);
    }

    static uint64_t now_ns() {
        return static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count());
    }

    K8sConfig   cfg_;
    NodeAddress self_{};
    std::atomic<bool> running_{false};
    std::thread poll_thread_;
    mutable std::mutex mu_;
    std::unordered_map<NodeId, NodeInfo> nodes_;
    std::vector<NodeChangeCallback> callbacks_;
};

// ============================================================================
// Factory: create registry by mode
// ============================================================================
enum class RegistryMode { GOSSIP, K8S };

inline std::unique_ptr<NodeRegistry> make_node_registry(RegistryMode mode) {
    switch (mode) {
        case RegistryMode::K8S:
            return std::make_unique<K8sNodeRegistry>();
        case RegistryMode::GOSSIP:
        default:
            return std::make_unique<GossipNodeRegistry>();
    }
}

} // namespace zeptodb::cluster
