#pragma once
// ============================================================================
// ZeptoDB: Agent Memory ANN Index
// ============================================================================
// Approximate nearest-neighbor candidate indexes for Agent Memory. Backends are
// partitioned by tenant/namespace and return row ids only; AgentMemoryStore
// still owns filtering, TTL checks, access counters, and final ranking.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::ai {

struct AnnIndexConfig {
    size_t tables = 8;
    size_t bits_per_table = 12;
    size_t terms_per_bit = 8;
    size_t probe_radius = 2;
    size_t max_candidates = 50'000;
    size_t hnsw_m = 16;
    size_t hnsw_ef_construction = 200;
    size_t hnsw_ef_search = 64;
    size_t ivf_centroids = 256;
    size_t ivf_probe = 8;
};

struct AnnSearchResult {
    size_t row_id = 0;
    double similarity = 0.0;
};

struct AnnIndexStats {
    size_t partitions = 0;
    size_t indexed_vectors = 0;
    size_t buckets = 0;
    size_t max_bucket_size = 0;
    size_t memory_bytes = 0;
    size_t tombstone_entries = 0;
};

enum class AnnIndexKind {
    SparseProjection,
    Hnsw,
    Ivf,
};

class AgentAnnIndex {
public:
    virtual ~AgentAnnIndex() = default;

    virtual bool add(const std::string& partition_key,
                     size_t row_id,
                     const std::vector<float>& embedding) = 0;
    virtual bool remove(const std::string& partition_key,
                        size_t row_id) = 0;
    virtual bool update_row_id(const std::string& partition_key,
                               size_t old_row_id,
                               size_t new_row_id) = 0;
    virtual std::vector<AnnSearchResult> search(const std::string& partition_key,
                                                const std::vector<float>& query,
                                                size_t limit) const = 0;

    virtual void clear() = 0;
    virtual bool empty() const = 0;
    virtual size_t dimension() const = 0;
    virtual AnnIndexStats stats() const = 0;
};

// IvfAnnIndex is a dependency-free inverted-file baseline. It builds one
// online centroid list per tenant/namespace partition and scans only the
// nearest configured lists for a query. It is intended for benchmark comparison,
// not as a trained production quantizer.
class IvfAnnIndex final : public AgentAnnIndex {
public:
    explicit IvfAnnIndex(AnnIndexConfig config = {});

    bool add(const std::string& partition_key,
             size_t row_id,
             const std::vector<float>& embedding) override;
    bool remove(const std::string& partition_key,
                size_t row_id) override;
    bool update_row_id(const std::string& partition_key,
                       size_t old_row_id,
                       size_t new_row_id) override;
    std::vector<AnnSearchResult> search(const std::string& partition_key,
                                        const std::vector<float>& query,
                                        size_t limit) const override;

    void clear() override;
    bool empty() const override;
    size_t dimension() const override;
    AnnIndexStats stats() const override;

private:
    struct Partition {
        std::vector<size_t> row_ids;
        std::vector<uint8_t> active;
        std::unordered_map<size_t, size_t> row_to_entry;
        std::vector<float> unit_vectors;
        std::vector<float> centroids;
        std::vector<std::vector<size_t>> lists;
        size_t active_count = 0;
    };

    bool ensure_dimension_(size_t dim);
    std::vector<float> normalize_(const std::vector<float>& embedding) const;
    double dot_(const std::vector<float>& a,
                const float* b) const;
    size_t nearest_centroid_(const Partition& partition,
                             const std::vector<float>& unit) const;

    AnnIndexConfig config_;
    size_t dimension_ = 0;
    std::unordered_map<std::string, Partition> partitions_;
};

// SparseProjectionAnnIndex is optimized for fast rebuilds from the sidecar
// vector arena and low-latency candidate generation. It is not a durability
// boundary; callers rebuild it from live MemoryRecord rows after snapshot load.
class SparseProjectionAnnIndex final : public AgentAnnIndex {
public:
    explicit SparseProjectionAnnIndex(AnnIndexConfig config = {});

    bool add(const std::string& partition_key,
             size_t row_id,
             const std::vector<float>& embedding) override;
    bool remove(const std::string& partition_key,
                size_t row_id) override;
    bool update_row_id(const std::string& partition_key,
                       size_t old_row_id,
                       size_t new_row_id) override;
    std::vector<AnnSearchResult> search(const std::string& partition_key,
                                        const std::vector<float>& query,
                                        size_t limit) const override;

    void clear() override;
    bool empty() const override;
    size_t dimension() const override;
    AnnIndexStats stats() const override;

private:
    struct ProjectionTerm {
        uint32_t dim = 0;
        float sign = 1.0f;
    };

    struct Partition {
        std::vector<size_t> row_ids;
        std::vector<uint8_t> active;
        std::unordered_map<size_t, size_t> row_to_entry;
        std::vector<float> unit_vectors;
        std::vector<std::unordered_map<uint32_t, std::vector<size_t>>> tables;
        size_t active_count = 0;
    };

    bool ensure_dimension_(size_t dim);
    std::vector<float> normalize_(const std::vector<float>& embedding) const;
    uint32_t signature_(const std::vector<float>& unit_embedding,
                        size_t table) const;
    uint32_t signature_(const float* unit_embedding,
                        size_t table) const;
    void collect_probe_signatures_(uint32_t base,
                                   std::vector<uint32_t>* out) const;
    double dot_(const std::vector<float>& a,
                const std::vector<float>& b) const;
    double dot_(const std::vector<float>& a,
                const float* b) const;
    void rebuild_projections_();
    static uint64_t mix_(uint64_t value);

    AnnIndexConfig config_;
    size_t dimension_ = 0;
    std::vector<std::vector<std::vector<ProjectionTerm>>> projections_;
    std::unordered_map<std::string, Partition> partitions_;
};

std::unique_ptr<AgentAnnIndex> make_ann_index(AnnIndexKind kind,
                                              AnnIndexConfig config);
bool hnsw_ann_available();

} // namespace zeptodb::ai
