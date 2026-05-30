#include "zeptodb/ai/agent_memory_router.h"

#include <sstream>
#include <utility>

namespace zeptodb::ai {

namespace {

const char* entry_kind_name(AgentMemoryEntryKind kind) {
    return kind == AgentMemoryEntryKind::Cache ? "cache" : "memory";
}

} // namespace

AgentMemoryRouter::AgentMemoryRouter(AgentMemoryRouterConfig config)
    : config_(config) {
    if (config_.virtual_nodes_per_node == 0) {
        config_.virtual_nodes_per_node = 1;
    }
}

void AgentMemoryRouter::set_config(AgentMemoryRouterConfig config) {
    std::lock_guard<std::mutex> lock(mu_);
    if (config.virtual_nodes_per_node == 0) {
        config.virtual_nodes_per_node = 1;
    }
    config_ = config;
    rebuild_ring_locked_();
}

AgentMemoryRouterConfig AgentMemoryRouter::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

void AgentMemoryRouter::add_node(AgentMemoryNodeId node_id) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.insert(node_id);
    rebuild_ring_locked_();
}

void AgentMemoryRouter::remove_node(AgentMemoryNodeId node_id) {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.erase(node_id);
    rebuild_ring_locked_();
}

void AgentMemoryRouter::clear_nodes() {
    std::lock_guard<std::mutex> lock(mu_);
    nodes_.clear();
    ring_.clear();
}

std::vector<AgentMemoryNodeId> AgentMemoryRouter::nodes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {nodes_.begin(), nodes_.end()};
}

AgentMemoryOwner AgentMemoryRouter::route(
    const AgentMemoryRoutingKey& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    AgentMemoryOwner owner;
    owner.node_id = config_.self_node_id;
    owner.ring_epoch = config_.ring_epoch;
    owner.local = true;
    if (config_.mode == AgentMemoryRoutingMode::Local || ring_.empty()) {
        return owner;
    }

    const uint64_t point = hash_(stable_key(key));
    auto it = ring_.lower_bound(point);
    if (it == ring_.end()) it = ring_.begin();
    owner.node_id = it->second;
    owner.local = owner.node_id == config_.self_node_id;
    return owner;
}

bool AgentMemoryRouter::is_local_owner(
    const AgentMemoryRoutingKey& key) const {
    return route(key).local;
}

AgentMemoryRoutingKey AgentMemoryRouter::memory_key(
    std::string tenant_id,
    std::string namespace_id,
    std::string session_id,
    std::string agent_id,
    std::string user_id,
    std::string memory_id) {
    AgentMemoryRoutingKey key;
    key.tenant_id = std::move(tenant_id);
    key.namespace_id = namespace_id.empty() ? "default" : std::move(namespace_id);
    key.logical_subject = select_logical_subject(session_id, agent_id, user_id,
                                                 memory_id);
    key.entry_kind = AgentMemoryEntryKind::Memory;
    return key;
}

AgentMemoryRoutingKey AgentMemoryRouter::cache_key(
    std::string tenant_id,
    std::string namespace_id,
    std::string normalized_prompt_hash) {
    AgentMemoryRoutingKey key;
    key.tenant_id = std::move(tenant_id);
    key.namespace_id = namespace_id.empty() ? "default" : std::move(namespace_id);
    key.logical_subject = std::move(normalized_prompt_hash);
    key.entry_kind = AgentMemoryEntryKind::Cache;
    return key;
}

std::string AgentMemoryRouter::select_logical_subject(
    const std::string& session_id,
    const std::string& agent_id,
    const std::string& user_id,
    const std::string& fallback) {
    if (!session_id.empty()) return session_id;
    if (!agent_id.empty()) return agent_id;
    if (!user_id.empty()) return user_id;
    return fallback;
}

std::string AgentMemoryRouter::stable_key(
    const AgentMemoryRoutingKey& key) {
    std::ostringstream os;
    os << key.tenant_id << '\n'
       << (key.namespace_id.empty() ? "default" : key.namespace_id) << '\n'
       << key.logical_subject << '\n'
       << entry_kind_name(key.entry_kind);
    return os.str();
}

void AgentMemoryRouter::rebuild_ring_locked_() {
    ring_.clear();
    if (nodes_.empty()) return;
    for (const AgentMemoryNodeId node_id : nodes_) {
        for (size_t vnode = 0; vnode < config_.virtual_nodes_per_node; ++vnode) {
            std::ostringstream os;
            os << node_id << '#' << vnode;
            uint64_t point = hash_(os.str());
            while (ring_.find(point) != ring_.end()) {
                ++point;
            }
            ring_[point] = node_id;
        }
    }
}

uint64_t AgentMemoryRouter::hash_(std::string_view value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

} // namespace zeptodb::ai
