#include "zeptodb/ai/ann_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>

#ifdef ZEPTO_ENABLE_HNSWLIB
#include <hnswlib/hnswlib.h>
#endif

namespace zeptodb::ai {

namespace {

constexpr size_t kMaxTables = 32;
constexpr size_t kMaxBits = 20;
constexpr size_t kMaxTermsPerBit = 32;
constexpr uint64_t kProjectionSeed = 0x9E3779B97F4A7C15ULL;

struct ScoredEntry {
    size_t entry_index = 0;
    double similarity = 0.0;
};

bool worse_score(const ScoredEntry& a, const ScoredEntry& b) {
    if (a.similarity != b.similarity) return a.similarity > b.similarity;
    return a.entry_index < b.entry_index;
}

} // namespace

#ifdef ZEPTO_ENABLE_HNSWLIB
class HnswAnnIndex final : public AgentAnnIndex {
public:
    explicit HnswAnnIndex(AnnIndexConfig config)
        : config_(config) {
        config_.hnsw_m = std::max<size_t>(config_.hnsw_m, 2);
        config_.hnsw_ef_construction =
            std::max<size_t>(config_.hnsw_ef_construction, config_.hnsw_m);
        config_.hnsw_ef_search = std::max<size_t>(config_.hnsw_ef_search, 1);
    }

    bool add(const std::string& partition_key,
             size_t row_id,
             const std::vector<float>& embedding) override {
        if (embedding.empty() || !ensure_dimension_(embedding.size())) return false;
        auto unit = normalize_(embedding);
        if (unit.empty()) return false;

        auto& partition = partitions_[partition_key];
        if (!partition.space) {
            partition.space =
                std::make_unique<hnswlib::L2Space>(static_cast<int>(dimension_));
        }
        if (!partition.index) {
            partition.index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                partition.space.get(),
                std::max<size_t>(1, config_.max_candidates),
                config_.hnsw_m,
                config_.hnsw_ef_construction,
                kHnswSeed);
        } else if (partition.row_ids.size() >= partition.index->getMaxElements()) {
            const size_t next_capacity = std::max<size_t>(
                partition.row_ids.size() + 1,
                partition.index->getMaxElements() * 2);
            partition.index->resizeIndex(next_capacity);
        }

        const size_t entry_index = partition.row_ids.size();
        partition.row_ids.push_back(row_id);
        partition.unit_vectors.insert(partition.unit_vectors.end(),
                                      unit.begin(), unit.end());
        partition.index->addPoint(unit.data(), entry_index);
        partition.index->setEf(std::max<size_t>(config_.hnsw_ef_search, 1));
        return true;
    }

    std::vector<AnnSearchResult> search(const std::string& partition_key,
                                        const std::vector<float>& query,
                                        size_t limit) const override {
        if (limit == 0 || query.empty() || query.size() != dimension_) return {};
        const auto partition_it = partitions_.find(partition_key);
        if (partition_it == partitions_.end()) return {};

        const auto query_unit = normalize_(query);
        if (query_unit.empty()) return {};

        const auto& partition = partition_it->second;
        if (!partition.index || partition.row_ids.empty()) return {};

        const size_t k = std::min(limit, partition.row_ids.size());
        if (k == 0) return {};

        std::priority_queue<std::pair<float, hnswlib::labeltype>> matches;
        try {
            partition.index->setEf(std::max(config_.hnsw_ef_search, k));
            matches = partition.index->searchKnn(query_unit.data(), k);
        } catch (const std::runtime_error&) {
            return {};
        }

        std::vector<AnnSearchResult> out;
        out.reserve(matches.size());
        while (!matches.empty()) {
            const auto [_, label] = matches.top();
            matches.pop();
            const size_t entry_index = static_cast<size_t>(label);
            if (entry_index >= partition.row_ids.size()) continue;
            const float* stored =
                partition.unit_vectors.data() + entry_index * dimension_;
            out.push_back({partition.row_ids[entry_index],
                           dot_(query_unit, stored)});
        }

        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.similarity != b.similarity) return a.similarity > b.similarity;
            return a.row_id < b.row_id;
        });
        return out;
    }

    void clear() override {
        dimension_ = 0;
        partitions_.clear();
    }

    bool empty() const override {
        return partitions_.empty();
    }

    size_t dimension() const override {
        return dimension_;
    }

    AnnIndexStats stats() const override {
        AnnIndexStats stats;
        stats.partitions = partitions_.size();
        for (const auto& [_, partition] : partitions_) {
            stats.indexed_vectors += partition.row_ids.size();
            stats.max_bucket_size =
                std::max(stats.max_bucket_size, config_.hnsw_m);
        }
        return stats;
    }

private:
    struct Partition {
        std::unique_ptr<hnswlib::L2Space> space;
        mutable std::unique_ptr<hnswlib::HierarchicalNSW<float>> index;
        std::vector<size_t> row_ids;
        std::vector<float> unit_vectors;
    };

    bool ensure_dimension_(size_t dim) {
        if (dim == 0 || dim > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        if (dimension_ != 0) return dimension_ == dim;
        dimension_ = dim;
        return true;
    }

    std::vector<float> normalize_(const std::vector<float>& embedding) const {
        double norm_sq = 0.0;
        for (const float value : embedding) {
            norm_sq += static_cast<double>(value) * static_cast<double>(value);
        }
        if (norm_sq <= 0.0 || !std::isfinite(norm_sq)) return {};
        const double inv_norm = 1.0 / std::sqrt(norm_sq);
        std::vector<float> out(embedding.size());
        for (size_t i = 0; i < embedding.size(); ++i) {
            out[i] = static_cast<float>(
                static_cast<double>(embedding[i]) * inv_norm);
        }
        return out;
    }

    double dot_(const std::vector<float>& a, const float* b) const {
        double out = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            out += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        }
        return out;
    }

    static constexpr size_t kHnswSeed = 100;

    AnnIndexConfig config_;
    size_t dimension_ = 0;
    std::unordered_map<std::string, Partition> partitions_;
};
#endif

SparseProjectionAnnIndex::SparseProjectionAnnIndex(AnnIndexConfig config)
    : config_(config) {
    config_.tables = std::clamp<size_t>(config_.tables, 1, kMaxTables);
    config_.bits_per_table = std::clamp<size_t>(config_.bits_per_table, 1, kMaxBits);
    config_.terms_per_bit = std::clamp<size_t>(config_.terms_per_bit, 1, kMaxTermsPerBit);
    config_.probe_radius = std::min<size_t>(config_.probe_radius, 2);
    config_.max_candidates = std::max<size_t>(config_.max_candidates, 1);
}

bool SparseProjectionAnnIndex::add(const std::string& partition_key,
                                   size_t row_id,
                                   const std::vector<float>& embedding) {
    if (embedding.empty() || !ensure_dimension_(embedding.size())) return false;
    auto unit = normalize_(embedding);
    if (unit.empty()) return false;

    auto& partition = partitions_[partition_key];
    if (partition.tables.empty()) {
        partition.tables.resize(config_.tables);
    }
    const size_t entry_index = partition.row_ids.size();
    partition.row_ids.push_back(row_id);
    partition.unit_vectors.insert(partition.unit_vectors.end(), unit.begin(), unit.end());

    const float* stored = partition.unit_vectors.data() + entry_index * dimension_;
    for (size_t table = 0; table < config_.tables; ++table) {
        const uint32_t sig = signature_(stored, table);
        partition.tables[table][sig].push_back(entry_index);
    }
    return true;
}

std::vector<AnnSearchResult> SparseProjectionAnnIndex::search(
    const std::string& partition_key,
    const std::vector<float>& query,
    size_t limit) const {
    if (limit == 0 || query.empty() || query.size() != dimension_) return {};
    const auto partition_it = partitions_.find(partition_key);
    if (partition_it == partitions_.end()) return {};

    const auto query_unit = normalize_(query);
    if (query_unit.empty()) return {};

    const auto& partition = partition_it->second;
    std::vector<size_t> candidates;
    candidates.reserve(std::min(config_.max_candidates, partition.row_ids.size()));
    std::vector<uint8_t> seen(partition.row_ids.size(), 0);
    std::vector<uint32_t> probes;

    for (size_t table = 0; table < config_.tables; ++table) {
        probes.clear();
        collect_probe_signatures_(signature_(query_unit, table), &probes);
        for (const uint32_t sig : probes) {
            const auto bucket = partition.tables[table].find(sig);
            if (bucket == partition.tables[table].end()) continue;
            for (const size_t entry_index : bucket->second) {
                if (seen[entry_index] != 0) continue;
                seen[entry_index] = 1;
                candidates.push_back(entry_index);
                if (candidates.size() >= config_.max_candidates) break;
            }
            if (candidates.size() >= config_.max_candidates) break;
        }
        if (candidates.size() >= config_.max_candidates) break;
    }

    auto worse = [](const ScoredEntry& a, const ScoredEntry& b) {
        return worse_score(a, b);
    };
    std::vector<ScoredEntry> heap;
    heap.reserve(std::min(limit, candidates.size()));
    for (const size_t entry_index : candidates) {
        const float* embedding = partition.unit_vectors.data() + entry_index * dimension_;
        const double sim = dot_(query_unit, embedding);
        ScoredEntry scored{entry_index, sim};
        if (heap.size() < limit) {
            heap.push_back(scored);
            std::push_heap(heap.begin(), heap.end(), worse);
        } else if (sim > heap.front().similarity ||
                   (sim == heap.front().similarity &&
                    entry_index < heap.front().entry_index)) {
            std::pop_heap(heap.begin(), heap.end(), worse);
            heap.back() = scored;
            std::push_heap(heap.begin(), heap.end(), worse);
        }
    }

    std::sort(heap.begin(), heap.end(), [](const auto& a, const auto& b) {
        if (a.similarity != b.similarity) return a.similarity > b.similarity;
        return a.entry_index < b.entry_index;
    });

    std::vector<AnnSearchResult> out;
    out.reserve(heap.size());
    for (const auto& scored : heap) {
        out.push_back({partition.row_ids[scored.entry_index],
                       scored.similarity});
    }
    return out;
}

void SparseProjectionAnnIndex::clear() {
    dimension_ = 0;
    projections_.clear();
    partitions_.clear();
}

bool SparseProjectionAnnIndex::empty() const {
    return partitions_.empty();
}

size_t SparseProjectionAnnIndex::dimension() const {
    return dimension_;
}

AnnIndexStats SparseProjectionAnnIndex::stats() const {
    AnnIndexStats stats;
    stats.partitions = partitions_.size();
    for (const auto& [_, partition] : partitions_) {
        stats.indexed_vectors += partition.row_ids.size();
        for (const auto& table : partition.tables) {
            stats.buckets += table.size();
            for (const auto& [__, rows] : table) {
                stats.max_bucket_size = std::max(stats.max_bucket_size, rows.size());
            }
        }
    }
    return stats;
}

bool SparseProjectionAnnIndex::ensure_dimension_(size_t dim) {
    if (dim == 0) return false;
    if (dimension_ != 0) return dimension_ == dim;
    dimension_ = dim;
    rebuild_projections_();
    return true;
}

std::vector<float> SparseProjectionAnnIndex::normalize_(
    const std::vector<float>& embedding) const {
    double norm_sq = 0.0;
    for (const float value : embedding) {
        norm_sq += static_cast<double>(value) * static_cast<double>(value);
    }
    if (norm_sq <= 0.0 || !std::isfinite(norm_sq)) return {};
    const double inv_norm = 1.0 / std::sqrt(norm_sq);
    std::vector<float> out(embedding.size());
    for (size_t i = 0; i < embedding.size(); ++i) {
        out[i] = static_cast<float>(static_cast<double>(embedding[i]) * inv_norm);
    }
    return out;
}

uint32_t SparseProjectionAnnIndex::signature_(
    const std::vector<float>& unit_embedding,
    size_t table) const {
    return signature_(unit_embedding.data(), table);
}

uint32_t SparseProjectionAnnIndex::signature_(
    const float* unit_embedding,
    size_t table) const {
    uint32_t signature = 0;
    for (size_t bit = 0; bit < config_.bits_per_table; ++bit) {
        double sum = 0.0;
        for (const auto& term : projections_[table][bit]) {
            sum += static_cast<double>(unit_embedding[term.dim]) *
                   static_cast<double>(term.sign);
        }
        if (sum >= 0.0) signature |= (1U << bit);
    }
    return signature;
}

void SparseProjectionAnnIndex::collect_probe_signatures_(
    uint32_t base,
    std::vector<uint32_t>* out) const {
    out->clear();
    out->push_back(base);
    if (config_.probe_radius == 0) return;
    for (size_t bit = 0; bit < config_.bits_per_table; ++bit) {
        out->push_back(base ^ (1U << bit));
    }
    if (config_.probe_radius < 2) return;
    for (size_t a = 0; a < config_.bits_per_table; ++a) {
        for (size_t b = a + 1; b < config_.bits_per_table; ++b) {
            out->push_back(base ^ (1U << a) ^ (1U << b));
        }
    }
}

double SparseProjectionAnnIndex::dot_(const std::vector<float>& a,
                                      const std::vector<float>& b) const {
    return dot_(a, b.data());
}

double SparseProjectionAnnIndex::dot_(const std::vector<float>& a,
                                      const float* b) const {
    double out = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        out += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return out;
}

void SparseProjectionAnnIndex::rebuild_projections_() {
    projections_.assign(config_.tables,
                        std::vector<std::vector<ProjectionTerm>>(config_.bits_per_table));
    for (size_t table = 0; table < config_.tables; ++table) {
        for (size_t bit = 0; bit < config_.bits_per_table; ++bit) {
            auto& terms = projections_[table][bit];
            terms.reserve(config_.terms_per_bit);
            for (size_t term = 0; term < config_.terms_per_bit; ++term) {
                const uint64_t h = mix_(
                    kProjectionSeed ^
                    (static_cast<uint64_t>(table) << 48U) ^
                    (static_cast<uint64_t>(bit) << 32U) ^
                    static_cast<uint64_t>(term));
                ProjectionTerm projection;
                projection.dim = static_cast<uint32_t>(h % dimension_);
                projection.sign = ((h >> 63U) == 0U) ? 1.0f : -1.0f;
                terms.push_back(projection);
            }
        }
    }
}

uint64_t SparseProjectionAnnIndex::mix_(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::unique_ptr<AgentAnnIndex> make_ann_index(AnnIndexKind kind,
                                              AnnIndexConfig config) {
    if (kind == AnnIndexKind::Hnsw) {
#ifdef ZEPTO_ENABLE_HNSWLIB
        return std::make_unique<HnswAnnIndex>(config);
#else
        return nullptr;
#endif
    }
    return std::make_unique<SparseProjectionAnnIndex>(config);
}

bool hnsw_ann_available() {
#ifdef ZEPTO_ENABLE_HNSWLIB
    return true;
#else
    return false;
#endif
}

} // namespace zeptodb::ai
