#pragma once
// ============================================================================
// Phase C-2: PartitionRouter — Consistent Hashing 기반 파티션 라우팅
// ============================================================================
// 설계:
//   - 물리 노드 1개 = 가상 노드(vnode) 128개 → 균등 분배
//   - xxHash 기반 해시 링 (O(log n) 조회, 캐시 O(1) 조회)
//   - 노드 추가/제거 시 최소 파티션 이동
//
// route() 구현:
//   1. symbol_id → hash value
//   2. hash ring에서 >= hash_value인 첫 번째 vnode 찾기 (upper_bound)
//   3. 없으면 링의 첫 번째 노드 (wrap-around)
// ============================================================================

#include "apex/common/types.h"
#include "apex/cluster/transport.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace apex::cluster {

// ============================================================================
// PartitionRouter: 일관 해시 링 기반 심볼 → 노드 라우팅
// ============================================================================
class PartitionRouter {
public:
    // 물리 노드 1개당 가상 노드 수 (균등 분배를 위해 충분히 크게)
    static constexpr size_t VNODES_PER_NODE = 128;

    PartitionRouter() = default;

    // Copy constructor: copies ring and node_set; cache is not copied (rebuilt
    // on demand) and each instance gets its own fresh mutex.
    PartitionRouter(const PartitionRouter& other)
        : ring_(other.ring_), node_set_(other.node_set_) {}

    PartitionRouter& operator=(const PartitionRouter& other) {
        if (this != &other) {
            ring_      = other.ring_;
            node_set_  = other.node_set_;
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.clear();
        }
        return *this;
    }

    // ----------------------------------------------------------------
    // 노드 관리
    // ----------------------------------------------------------------

    /// 클러스터에 노드 추가
    void add_node(NodeId node) {
        if (node_set_.count(node)) return;  // 이미 존재

        node_set_.insert(node);
        for (size_t i = 0; i < VNODES_PER_NODE; ++i) {
            uint64_t h = vnode_hash(node, i);
            ring_[h]   = node;
        }
        // Invalidate routing cache under its own mutex
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        cache_.clear();
    }

    /// 클러스터에서 노드 제거
    void remove_node(NodeId node) {
        if (!node_set_.count(node)) return;

        node_set_.erase(node);
        for (size_t i = 0; i < VNODES_PER_NODE; ++i) {
            uint64_t h = vnode_hash(node, i);
            ring_.erase(h);
        }
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        cache_.clear();
    }

    /// 현재 노드 수
    size_t node_count() const { return node_set_.size(); }

    /// 모든 노드 목록
    std::vector<NodeId> all_nodes() const {
        return std::vector<NodeId>(node_set_.begin(), node_set_.end());
    }

    // ----------------------------------------------------------------
    // 라우팅
    // ----------------------------------------------------------------

    /// Symbol → 담당 NodeId 반환 (O(1) 캐시 or O(log n) 링 조회)
    /// Pinned symbols bypass the hash ring and return the pinned node directly.
    /// Thread-safe: cache reads/writes protected by cache_mutex_ (separate from
    /// the ring, which must be protected by the caller's outer lock).
    NodeId route(SymbolId symbol) const {
        // Check pin table first (hot symbol override)
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            auto pin_it = pinned_.find(symbol);
            if (pin_it != pinned_.end()) return pin_it->second;
        }

        if (ring_.empty()) {
            throw std::runtime_error("PartitionRouter: no nodes in cluster");
        }

        // Check cache
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            auto cache_it = cache_.find(symbol);
            if (cache_it != cache_.end()) {
                return cache_it->second;
            }
        }

        uint64_t h = symbol_hash(symbol);
        NodeId   n = find_node(h);

        // Store result in cache
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            if (cache_.size() < MAX_CACHE_SIZE) {
                cache_[symbol] = n;
            }
        }
        return n;
    }

    // ----------------------------------------------------------------
    // Hot symbol pinning
    // ----------------------------------------------------------------

    /// Pin a hot symbol to a dedicated node (bypasses hash ring)
    void pin_symbol(SymbolId symbol, NodeId node) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        pinned_[symbol] = node;
        cache_.erase(symbol);  // invalidate cache for this symbol
    }

    /// Unpin a symbol (returns to hash ring routing)
    void unpin_symbol(SymbolId symbol) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        pinned_.erase(symbol);
        cache_.erase(symbol);
    }

    /// Get all pinned symbols
    std::unordered_map<SymbolId, NodeId> pinned_symbols() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return pinned_;
    }

    /// Number of pinned symbols
    size_t pinned_count() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return pinned_.size();
    }

    /// Symbol → replica NodeId (next distinct physical node on the ring).
    /// Returns INVALID_NODE_ID if only 1 physical node exists.
    NodeId route_replica(SymbolId symbol) const {
        if (node_set_.size() < 2) return INVALID_NODE_ID;

        uint64_t h = symbol_hash(symbol);
        NodeId primary = find_node(h);

        // Walk the ring from primary's position to find a different node
        auto it = ring_.lower_bound(h);
        if (it == ring_.end()) it = ring_.begin();

        // Advance past all vnodes belonging to the primary
        for (size_t steps = 0; steps < ring_.size(); ++steps) {
            ++it;
            if (it == ring_.end()) it = ring_.begin();
            if (it->second != primary) return it->second;
        }
        return INVALID_NODE_ID;  // shouldn't happen with ≥2 nodes
    }

    // ----------------------------------------------------------------
    // 마이그레이션 계획
    // ----------------------------------------------------------------

    /// 마이그레이션 이동 단위
    struct Move {
        SymbolId symbol;  // 이동할 심볼 (대표)
        NodeId   from;
        NodeId   to;
    };

    struct MigrationPlan {
        std::vector<Move> moves;
        size_t total_moves() const { return moves.size(); }
    };

    /// 새 노드 추가 시 마이그레이션 계획 (실제 추가 전 계획만 반환)
    /// 최소 파티션만 이동: 1/N 비율
    MigrationPlan plan_add(NodeId new_node) const {
        if (node_set_.count(new_node)) {
            return {};  // 이미 존재
        }

        MigrationPlan plan;

        // 임시로 new_node를 추가한 상태에서 비교
        PartitionRouter temp = *this;
        temp.add_node(new_node);

        // 현재 링에서 각 vnode가 담당하는 구간을 new_node로 재할당되는 것 계산
        // 가상 노드 레벨에서 이동: new_node의 각 vnode 앞에 있던 구간의 이전 담당 노드
        for (size_t i = 0; i < VNODES_PER_NODE; ++i) {
            uint64_t h = vnode_hash(new_node, i);

            // 현재 링에서 h 위치를 담당하던 노드 (= 이 구간의 이전 주인)
            if (ring_.empty()) break;
            NodeId old_owner = find_node(h);

            if (old_owner != new_node) {
                // 이 vnode 구간은 old_owner → new_node로 이동
                // Move에 심볼 대신 vnode 해시를 기록 (대표 심볼로 표현)
                plan.moves.push_back(Move{
                    static_cast<SymbolId>(h & 0xFFFFFFFF),  // 대표 심볼 (근사치)
                    old_owner,
                    new_node
                });
            }
        }

        // 중복 from→to 쌍 제거 (같은 노드 간 여러 vnode 이동을 하나로 집계)
        deduplicate_moves(plan);
        return plan;
    }

    /// 노드 제거 시 마이그레이션 계획
    MigrationPlan plan_remove(NodeId leaving) const {
        if (!node_set_.count(leaving)) {
            return {};
        }
        if (node_set_.size() <= 1) {
            // 마지막 노드 → 이동할 곳 없음
            return {};
        }

        MigrationPlan plan;

        // 임시로 leaving 제거 후 비교
        PartitionRouter temp = *this;
        temp.remove_node(leaving);

        // leaving 노드의 각 vnode 구간이 누구에게 가는지 계산
        for (size_t i = 0; i < VNODES_PER_NODE; ++i) {
            uint64_t h = vnode_hash(leaving, i);

            // 제거 후 h를 담당하는 노드
            NodeId new_owner = temp.find_node(h);

            plan.moves.push_back(Move{
                static_cast<SymbolId>(h & 0xFFFFFFFF),
                leaving,
                new_owner
            });
        }

        deduplicate_moves(plan);
        return plan;
    }

private:
    // ----------------------------------------------------------------
    // 해시 함수 (xxHash 스타일, 간단한 구현)
    // ----------------------------------------------------------------

    /// Symbol ID → ring 해시
    static uint64_t symbol_hash(SymbolId sym) {
        // FNV-1a 64비트
        uint64_t h = 14695981039346656037ULL;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&sym);
        for (size_t i = 0; i < sizeof(sym); ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    /// 가상 노드 해시 (물리 노드 ID + 가상 인덱스)
    static uint64_t vnode_hash(NodeId node, size_t idx) {
        // node_id와 idx를 합쳐서 해시
        uint64_t key = (static_cast<uint64_t>(node) << 32) |
                       static_cast<uint64_t>(idx & 0xFFFFFFFF);
        // Splitmix64
        key += 0x9e3779b97f4a7c15ULL;
        key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
        key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
        return key ^ (key >> 31);
    }

    /// 해시 값에 해당하는 노드 찾기 (consistent hash lookup)
    NodeId find_node(uint64_t h) const {
        auto it = ring_.lower_bound(h);
        if (it == ring_.end()) {
            // wrap-around: 링의 첫 번째 노드
            it = ring_.begin();
        }
        return it->second;
    }

    /// 마이그레이션 플랜에서 같은 (from, to) 쌍 중복 제거
    static void deduplicate_moves(MigrationPlan& plan) {
        // (from, to) 쌍당 1개만 유지
        std::set<std::pair<NodeId, NodeId>> seen;
        std::vector<Move> unique_moves;
        for (auto& m : plan.moves) {
            auto key = std::make_pair(m.from, m.to);
            if (!seen.count(key)) {
                seen.insert(key);
                unique_moves.push_back(m);
            }
        }
        plan.moves = std::move(unique_moves);
    }

    // ----------------------------------------------------------------
    // 데이터 멤버
    // ----------------------------------------------------------------

    // 해시 링: hash → NodeId (정렬된 맵)
    std::map<uint64_t, NodeId>          ring_;

    // 물리 노드 집합
    std::set<NodeId>                    node_set_;

    // Routing cache — protected by its own mutex so that concurrent route()
    // calls from multiple reader threads do not race on the unordered_map.
    mutable std::mutex                           cache_mutex_;
    mutable std::unordered_map<SymbolId, NodeId> cache_;
    // Hot symbol pin table — symbols pinned to dedicated nodes bypass hash ring
    mutable std::unordered_map<SymbolId, NodeId> pinned_;

    static constexpr size_t MAX_CACHE_SIZE = 65536;
};

} // namespace apex::cluster
