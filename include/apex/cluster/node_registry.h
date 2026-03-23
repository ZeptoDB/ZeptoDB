#pragma once
// ============================================================================
// APEX-DB: NodeRegistry — pluggable node membership management
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

#include "apex/cluster/transport.h"
#include "apex/cluster/health_monitor.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace apex::cluster {

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
        running_ = true;
    }

    void stop() override {
        if (!running_) return;
        health_.mark_leaving(self_.id);
        health_.stop();
        running_ = false;
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
    bool          running_ = false;
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
    std::string service_name   = "apex-db";
    std::string label_selector = "app=apex-db";
    uint32_t    poll_interval_ms = 5000;  // endpoint poll interval
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
            for (auto& s : seeds) {
                nodes_[s.id] = {s, NodeState::ACTIVE, now_ns()};
                fire_event(s.id, NodeEvent::JOINED);
            }
        }
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

    /// Manual node registration (for testing or static config fallback)
    void register_node(const NodeAddress& addr) {
        std::lock_guard<std::mutex> lock(mu_);
        bool is_new = (nodes_.find(addr.id) == nodes_.end());
        nodes_[addr.id] = {addr, NodeState::ACTIVE, now_ns()};
        if (is_new) fire_event_unlocked(addr.id, NodeEvent::JOINED);
    }

    /// Manual node removal
    void deregister_node(NodeId id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = nodes_.find(id);
        if (it != nodes_.end()) {
            it->second.state = NodeState::LEAVING;
            fire_event_unlocked(id, NodeEvent::LEFT);
            nodes_.erase(it);
        }
    }

private:
    void poll_loop() {
        while (running_) {
            // In full implementation: GET K8s endpoints API, diff with nodes_
            // For now: just sleep (nodes managed via register/deregister)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg_.poll_interval_ms));
        }
    }

    void fire_event(NodeId id, NodeEvent event) {
        std::vector<NodeChangeCallback> cbs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs = callbacks_;
        }
        for (auto& cb : cbs) cb(id, event);
    }

    // Must be called with mu_ held
    void fire_event_unlocked(NodeId id, NodeEvent event) {
        auto cbs = callbacks_;  // copy under lock
        // Release lock before firing? No — caller holds lock.
        // Fire inline (callbacks should be fast).
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

} // namespace apex::cluster
