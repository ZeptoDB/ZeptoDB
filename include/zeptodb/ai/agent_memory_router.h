#pragma once
// ============================================================================
// ZeptoDB: Agent Memory Router
// ============================================================================
// Deterministic owner routing for multi-node Agent Memory. The router is a
// lightweight, dependency-free consistent hash ring over live Agent Memory
// nodes. It does not perform RPC; callers use the owner decision to choose a
// local AgentMemoryStore write or a future remote write path.
//
// Thread-safety: all public methods are safe for concurrent callers. Returned
// node lists and owner decisions are snapshots of the current ring.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace zeptodb::ai {

using AgentMemoryNodeId = uint32_t;

enum class AgentMemoryEntryKind {
    Memory,
    Cache,
};

enum class AgentMemoryRoutingMode {
    Local,
    Routed,
};

// Stable ownership key. namespace_id defaults to "default" when empty. The
// logical subject should be session_id, agent_id, user_id, then memory/cache
// identity, selected by select_logical_subject().
struct AgentMemoryRoutingKey {
    std::string tenant_id;
    std::string namespace_id = "default";
    std::string logical_subject;
    AgentMemoryEntryKind entry_kind = AgentMemoryEntryKind::Memory;
};

struct AgentMemoryOwner {
    AgentMemoryNodeId node_id = 0;
    uint64_t ring_epoch = 0;
    bool local = true;
};

struct AgentMemoryRouterConfig {
    AgentMemoryNodeId self_node_id = 0;
    uint64_t ring_epoch = 0;
    AgentMemoryRoutingMode mode = AgentMemoryRoutingMode::Local;
    size_t virtual_nodes_per_node = 128;
};

class AgentMemoryRouter {
public:
    explicit AgentMemoryRouter(AgentMemoryRouterConfig config = {});

    void set_config(AgentMemoryRouterConfig config);
    AgentMemoryRouterConfig config() const;

    void add_node(AgentMemoryNodeId node_id);
    void remove_node(AgentMemoryNodeId node_id);
    void clear_nodes();
    std::vector<AgentMemoryNodeId> nodes() const;

    // Resolve the current owner. Local mode, or routed mode with an empty ring,
    // always returns self_node_id.
    AgentMemoryOwner route(const AgentMemoryRoutingKey& key) const;
    bool is_local_owner(const AgentMemoryRoutingKey& key) const;

    static AgentMemoryRoutingKey memory_key(
        std::string tenant_id,
        std::string namespace_id,
        std::string session_id,
        std::string agent_id,
        std::string user_id,
        std::string memory_id);

    static AgentMemoryRoutingKey cache_key(
        std::string tenant_id,
        std::string namespace_id,
        std::string normalized_prompt_hash);

    static std::string select_logical_subject(
        const std::string& session_id,
        const std::string& agent_id,
        const std::string& user_id,
        const std::string& fallback);

    static std::string stable_key(const AgentMemoryRoutingKey& key);

private:
    void rebuild_ring_locked_();
    static uint64_t hash_(std::string_view value);

    mutable std::mutex mu_;
    AgentMemoryRouterConfig config_;
    std::set<AgentMemoryNodeId> nodes_;
    std::map<uint64_t, AgentMemoryNodeId> ring_;
};

} // namespace zeptodb::ai
