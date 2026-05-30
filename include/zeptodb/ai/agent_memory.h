#pragma once
// ============================================================================
// ZeptoDB: Agent Memory Store
// ============================================================================
// In-memory context substrate for AI agents. Stores short/long-lived memories,
// retrieves context under token budgets, and serves exact/semantic LLM cache
// hits. Embeddings are client-supplied float32 vectors; ZeptoDB never calls an
// embedding provider or LLM from this layer.
//
// Thread-safety: all public methods are safe for concurrent callers. Returned
// records are snapshots.
// ============================================================================

#include "zeptodb/ai/ann_index.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeptodb::ai {

// A single agent memory item. The embedding is optional but, when present, must
// match the store-wide embedding dimension.
struct MemoryRecord {
    std::string memory_id;
    std::string tenant_id;
    std::string namespace_id = "default";
    std::string user_id;
    std::string session_id;
    std::string agent_id;
    std::string type = "memory";
    std::string content;
    std::string metadata_json = "{}";
    std::vector<float> embedding;
    int64_t token_count = 0;
    double importance = 0.0;
    int64_t created_at_ns = 0;
    int64_t last_accessed_ns = 0;
    int64_t expires_at_ns = 0;
    bool pinned = false;
    uint64_t access_count = 0;
};

// Search filter and ranking request. Empty string fields are wildcards; limit 0
// returns no rows. Timestamps are nanoseconds since Unix epoch. force_scan
// bypasses ANN candidate generation for diagnostics and exact comparisons.
// update_access can be disabled for read-only benchmark/diagnostic probes that
// must not perturb recency/access-count ranking.
struct MemoryQuery {
    std::string tenant_id;
    std::string namespace_id = "default";
    std::string user_id;
    std::string session_id;
    std::string agent_id;
    std::string type;
    std::vector<float> query_embedding;
    size_t limit = 10;
    int64_t now_ns = 0;
    bool include_expired = false;
    bool force_scan = false;
    bool update_access = true;
};

struct MemorySearchResult {
    MemoryRecord record;
    double score = 0.0;
    double similarity = 0.0;
};

// Context assembly request. token_budget 0 means no budget limit; negative
// budgets are invalid and return an empty result.
struct ContextRequest : MemoryQuery {
    int64_t token_budget = 0;  // 0 = no budget limit
};

struct ContextResult {
    std::vector<MemorySearchResult> memories;
    int64_t token_count = 0;
};

// Application-level LLM cache entry. Exact lookup is keyed by tenant,
// namespace, and normalized prompt; embedding enables semantic fallback.
struct CacheEntry {
    std::string cache_id;
    std::string tenant_id;
    std::string namespace_id = "default";
    std::string prompt;
    std::string response;
    std::string metadata_json = "{}";
    std::vector<float> embedding;
    int64_t token_count = 0;
    int64_t created_at_ns = 0;
    int64_t last_accessed_ns = 0;
    int64_t expires_at_ns = 0;
    uint64_t access_count = 0;
};

// Cache lookup request. Exact prompt lookup is attempted before semantic scan.
struct CacheLookup {
    std::string tenant_id;
    std::string namespace_id = "default";
    std::string prompt;
    std::vector<float> embedding;
    double semantic_threshold = 0.92;
    int64_t now_ns = 0;
};

struct CacheLookupResult {
    bool hit = false;
    bool exact = false;
    double score = 0.0;
    CacheEntry entry;
};

struct StoreResult {
    bool ok = false;
    std::string id;
    std::string error;
};

// Runtime eviction policy. A max value of 0 means unbounded. Pinned memories are
// protected from capacity eviction by default, but explicit TTL expiry still
// removes them.
struct AgentMemoryEvictionConfig {
    size_t max_memories = 0;
    size_t max_cache_entries = 0;
    bool evict_expired_on_write = true;
    bool protect_pinned = true;
};

struct AgentMemoryStats {
    size_t memory_count = 0;
    size_t cache_count = 0;
    size_t embedding_dim = 0;
    uint64_t evicted_memory_count = 0;
    uint64_t evicted_cache_count = 0;
    bool ann_enabled = false;
    size_t ann_indexed_vectors = 0;
    size_t ann_partitions = 0;
    size_t ann_buckets = 0;
    size_t ann_max_bucket_size = 0;
    uint64_t ann_rebuild_count = 0;
    double ann_last_rebuild_ms = 0.0;
    uint64_t ann_search_count = 0;
    uint64_t ann_fallback_count = 0;
};

// Runtime identity used for owner-scoped automatic ids in routed multi-node
// deployments. The default keeps legacy local ids such as mem_1 and cache_1.
struct AgentMemoryIdConfig {
    bool owner_scoped = false;
    uint32_t node_id = 0;
    uint64_t ring_epoch = 0;
};

enum class AgentMemoryAnnMode {
    Off,
    Auto,
    SparseProjection,
    Hnsw,
};

// ANN accelerates semantic candidate generation. The final result still passes
// through AgentMemoryStore's TTL/filter/ranking path.
struct AgentMemoryAnnConfig {
    AgentMemoryAnnMode mode = AgentMemoryAnnMode::Off;
    size_t min_records = 50'000;
    size_t oversample = 8;
    AnnIndexConfig index;
};

class AgentMemoryStore {
public:
    AgentMemoryStore();
    ~AgentMemoryStore();

    // Store or update a memory record. Returns ok=false for invalid embeddings,
    // negative token_count, or tenant conflicts on an existing memory_id.
    StoreResult put_memory(MemoryRecord record);

    // Return a memory snapshot by id. tenant_id is optional; when provided it
    // must match the stored record.
    std::optional<MemoryRecord> get_memory(const std::string& memory_id,
                                           const std::string& tenant_id = "") const;

    // Return an exact cache entry snapshot without updating access counters.
    std::optional<CacheEntry> get_cache(const std::string& tenant_id,
                                        const std::string& namespace_id,
                                        const std::string& prompt) const;

    // Return full memory/cache snapshots without updating access counters.
    // Intended for persistence, failover adoption, and diagnostics.
    std::vector<MemoryRecord> memory_records_snapshot() const;
    std::vector<CacheEntry> cache_entries_snapshot() const;

    // Remove a memory by id. tenant_id is optional; when provided it must match
    // the stored record. Intended for failed durability rollback and explicit
    // administrative cleanup.
    bool remove_memory(const std::string& memory_id,
                       const std::string& tenant_id = "");

    // Search live memories using filters and ranking. Updates access counters
    // for returned records. The exact path uses a bounded top-K scan and may
    // parallelize large full scans internally.
    std::vector<MemorySearchResult> search(MemoryQuery query);

    // Search and select deduplicated memories under token_budget.
    ContextResult get_context(ContextRequest request);

    // Store or update a cache entry keyed by normalized prompt.
    StoreResult store_cache(CacheEntry entry);

    // Remove an exact cache entry by tenant, namespace, and normalized prompt.
    // Intended for failed durability rollback and explicit administrative
    // cleanup.
    bool remove_cache(const std::string& tenant_id,
                      const std::string& namespace_id,
                      const std::string& prompt);

    // Lookup exact prompt cache first, then semantic cache by threshold.
    CacheLookupResult lookup_cache(CacheLookup lookup);

    // Configure bounded in-memory retention. If the current store is already
    // over the configured limits, eviction runs immediately.
    void set_eviction_config(AgentMemoryEvictionConfig config);
    AgentMemoryEvictionConfig eviction_config() const;

    // Configure optional ANN candidate generation. Off preserves the exact
    // filtered-scan path; Auto uses ANN once min_records is reached.
    void set_ann_config(AgentMemoryAnnConfig config);
    AgentMemoryAnnConfig ann_config() const;

    // Rebuild the derived ANN index from live memories synchronously. The build
    // work runs outside the store mutex and swaps in only if no newer mutation
    // superseded the snapshot. Search schedules this work on the background ANN
    // worker when needed; benchmarks and startup paths can call it explicitly.
    StoreResult rebuild_ann_index();

    // Configure automatic id format. owner_scoped=false preserves mem_N/cache_N;
    // owner_scoped=true emits mem_<node>_<epoch>_<counter> and
    // cache_<node>_<epoch>_<counter>.
    void set_id_config(AgentMemoryIdConfig config);
    AgentMemoryIdConfig id_config() const;

    // Reserve internal containers for bulk loading. This is a capacity hint only
    // and does not change visible store contents.
    void reserve_memory_capacity(size_t memory_count, size_t cache_count = 0);

    // Remove expired memories/cache entries. now_ns=0 uses wall-clock time.
    // Returns the total number of entries removed.
    size_t evict_expired(int64_t now_ns = 0);

    // Persist memory/cache metadata plus embedding vectors to a sidecar
    // directory. v0 uses two native-endian binary files:
    //   records.bin  - scalar/string metadata and vector offsets
    //   vectors.bin  - row-major float32 payloads
    StoreResult save_to_directory(const std::string& directory) const;

    // Load a previously saved sidecar snapshot. Missing files mean an empty
    // store; partial/corrupt snapshots return ok=false and leave the current
    // store unchanged.
    StoreResult load_from_directory(const std::string& directory);

    AgentMemoryStats stats() const;
    void clear();

    static int64_t now_ns();
    static std::string normalize_prompt(const std::string& prompt);
    static double cosine_similarity(const std::vector<float>& a,
                                    const std::vector<float>& b);
    static int64_t estimate_tokens(const std::string& content);

private:
    struct AnnBuildEntry {
        std::string partition_key;
        size_t row_id = 0;
        std::vector<float> embedding;
    };

    struct AnnBuildSnapshot {
        AnnIndexKind kind = AnnIndexKind::SparseProjection;
        AnnIndexConfig config;
        uint64_t generation = 0;
        std::vector<AnnBuildEntry> entries;
    };

    void ann_rebuild_worker_loop_();
    void request_ann_rebuild_locked_();
    void stop_ann_rebuild_worker_();
    bool validate_embedding_(const std::vector<float>& embedding,
                             std::string* error);
    bool expired_(int64_t expires_at_ns, int64_t now) const;
    bool matches_(const MemoryRecord& record, const MemoryQuery& query,
                  int64_t now) const;
    double score_(const MemoryRecord& record, const MemoryQuery& query,
                  int64_t now, double similarity) const;
    std::vector<float> normalize_embedding_(const std::vector<float>& embedding) const;
    void append_unit_embedding_locked_(const std::vector<float>& embedding);
    void set_unit_embedding_locked_(size_t idx, const std::vector<float>& embedding);
    void rebuild_unit_embeddings_locked_();
    double memory_similarity_locked_(size_t idx,
                                     const std::vector<float>& query_unit) const;
    std::vector<MemorySearchResult> search_scan_locked_(
        const MemoryQuery& query,
        int64_t now,
        const std::vector<size_t>* candidate_rows);
    bool should_use_ann_locked_(const MemoryQuery& query) const;
    AnnIndexKind ann_index_kind_locked_() const;
    std::optional<AnnBuildSnapshot> ann_build_snapshot_locked_() const;
    StoreResult build_and_swap_ann_index_(AnnBuildSnapshot snapshot);
    bool try_add_ann_entry_locked_(size_t row_id, const MemoryRecord& record);
    void mark_ann_dirty_locked_();
    std::string ann_partition_key_(const std::string& tenant_id,
                                   const std::string& namespace_id) const;
    double memory_retention_score_(const MemoryRecord& record, int64_t now) const;
    double cache_retention_score_(const CacheEntry& entry, int64_t now) const;
    size_t evict_expired_locked_(int64_t now);
    void enforce_eviction_locked_(int64_t now);
    bool evict_one_memory_for_capacity_locked_(int64_t now);
    bool evict_one_cache_for_capacity_locked_(int64_t now);
    void rebuild_expiry_counts_locked_();
    void rebuild_memory_index_locked_();
    void rebuild_cache_index_locked_();
    std::string next_id_(const char* prefix);

    mutable std::mutex mu_;
    size_t embedding_dim_ = 0;
    uint64_t next_memory_id_ = 1;
    uint64_t next_cache_id_ = 1;
    uint64_t evicted_memory_count_ = 0;
    uint64_t evicted_cache_count_ = 0;
    size_t expiring_memory_count_ = 0;
    size_t expiring_cache_count_ = 0;
    AgentMemoryIdConfig id_config_;
    AgentMemoryEvictionConfig eviction_config_;
    AgentMemoryAnnConfig ann_config_;
    std::unique_ptr<AgentAnnIndex> ann_index_;
    std::thread ann_rebuild_worker_;
    std::condition_variable ann_rebuild_cv_;
    std::vector<float> memory_unit_embeddings_;
    std::vector<uint8_t> memory_has_embedding_;
    bool ann_dirty_ = true;
    bool ann_rebuild_stop_ = false;
    bool ann_rebuild_requested_ = false;
    bool ann_rebuild_running_ = false;
    uint64_t ann_generation_ = 0;
    uint64_t ann_rebuild_requested_generation_ = 0;
    uint64_t ann_rebuild_running_generation_ = 0;
    uint64_t ann_rebuild_count_ = 0;
    double ann_last_rebuild_ms_ = 0.0;
    uint64_t ann_search_count_ = 0;
    uint64_t ann_fallback_count_ = 0;
    std::vector<MemoryRecord> memories_;
    std::unordered_map<std::string, size_t> memory_index_;
    std::vector<CacheEntry> cache_entries_;
    std::unordered_map<std::string, size_t> cache_exact_index_;
};

} // namespace zeptodb::ai
