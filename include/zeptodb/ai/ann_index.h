#pragma once
// ============================================================================
// ZeptoDB: Agent Memory ANN Index
// ============================================================================
// Dependency-free approximate nearest-neighbor candidate index for Agent Memory.
// v0 uses sparse random-projection buckets partitioned by tenant/namespace. It
// returns row ids only; AgentMemoryStore still owns filtering, TTL checks,
// access counters, and final ranking.
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
};

enum class AnnIndexKind {
    SparseProjection,
    Hnsw,
};

class AgentAnnIndex {
public:
    virtual ~AgentAnnIndex() = default;

    virtual bool add(const std::string& partition_key,
                     size_t row_id,
                     const std::vector<float>& embedding) = 0;
    virtual std::vector<AnnSearchResult> search(const std::string& partition_key,
                                                const std::vector<float>& query,
                                                size_t limit) const = 0;

    virtual void clear() = 0;
    virtual bool empty() const = 0;
    virtual size_t dimension() const = 0;
    virtual AnnIndexStats stats() const = 0;
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
        std::vector<float> unit_vectors;
        std::vector<std::unordered_map<uint32_t, std::vector<size_t>>> tables;
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
