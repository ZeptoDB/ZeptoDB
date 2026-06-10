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

        if (partition.row_to_entry.find(row_id) != partition.row_to_entry.end()) {
            return false;
        }

        const size_t entry_index = partition.row_ids.size();
        partition.row_ids.push_back(row_id);
        partition.active.push_back(1);
        partition.row_to_entry[row_id] = entry_index;
        partition.unit_vectors.insert(partition.unit_vectors.end(),
                                      unit.begin(), unit.end());
        partition.index->addPoint(unit.data(), entry_index);
        partition.index->setEf(std::max<size_t>(config_.hnsw_ef_search, 1));
        ++partition.active_count;
        return true;
    }

    bool remove(const std::string& partition_key,
                size_t row_id) override {
        const auto partition_it = partitions_.find(partition_key);
        if (partition_it == partitions_.end()) return false;
        auto& partition = partition_it->second;
        const auto entry_it = partition.row_to_entry.find(row_id);
        if (entry_it == partition.row_to_entry.end()) return false;
        const size_t entry_index = entry_it->second;
        if (entry_index >= partition.active.size() ||
            partition.active[entry_index] == 0) {
            return false;
        }
        try {
            if (partition.index) {
                partition.index->markDelete(entry_index);
            }
        } catch (const std::runtime_error&) {
            return false;
        }
        partition.active[entry_index] = 0;
        partition.row_to_entry.erase(entry_it);
        if (partition.active_count > 0) --partition.active_count;
        if (partition.active_count == 0) {
            partitions_.erase(partition_it);
        }
        return true;
    }

    bool update_row_id(const std::string& partition_key,
                       size_t old_row_id,
                       size_t new_row_id) override {
        if (old_row_id == new_row_id) return true;
        const auto partition_it = partitions_.find(partition_key);
        if (partition_it == partitions_.end()) return false;
        auto& partition = partition_it->second;
        const auto entry_it = partition.row_to_entry.find(old_row_id);
        if (entry_it == partition.row_to_entry.end()) return false;
        if (partition.row_to_entry.find(new_row_id) != partition.row_to_entry.end()) {
            return false;
        }
        const size_t entry_index = entry_it->second;
        if (entry_index >= partition.active.size() ||
            partition.active[entry_index] == 0) {
            return false;
        }
        partition.row_ids[entry_index] = new_row_id;
        partition.row_to_entry.erase(entry_it);
        partition.row_to_entry[new_row_id] = entry_index;
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
            if (entry_index >= partition.active.size() ||
                partition.active[entry_index] == 0) {
                continue;
            }
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
            stats.indexed_vectors += partition.active_count;
            stats.tombstone_entries +=
                partition.row_ids.size() > partition.active_count
                    ? partition.row_ids.size() - partition.active_count
                    : 0;
            stats.max_bucket_size =
                std::max(stats.max_bucket_size, partition.active_count);
            stats.memory_bytes += partition.row_ids.capacity() * sizeof(size_t);
            stats.memory_bytes += partition.active.capacity() * sizeof(uint8_t);
            stats.memory_bytes +=
                partition.unit_vectors.capacity() * sizeof(float);
            stats.memory_bytes +=
                partition.row_to_entry.size() *
                (sizeof(size_t) * 2 + sizeof(void*) * 2);
        }
        return stats;
    }

private:
    struct Partition {
        std::unique_ptr<hnswlib::L2Space> space;
        mutable std::unique_ptr<hnswlib::HierarchicalNSW<float>> index;
        std::vector<size_t> row_ids;
        std::vector<uint8_t> active;
        std::unordered_map<size_t, size_t> row_to_entry;
        std::vector<float> unit_vectors;
        size_t active_count = 0;
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
    if (partition.row_to_entry.find(row_id) != partition.row_to_entry.end()) {
        return false;
    }
    const size_t entry_index = partition.row_ids.size();
    partition.row_ids.push_back(row_id);
    partition.active.push_back(1);
    partition.row_to_entry[row_id] = entry_index;
    partition.unit_vectors.insert(partition.unit_vectors.end(), unit.begin(), unit.end());

    const float* stored = partition.unit_vectors.data() + entry_index * dimension_;
    for (size_t table = 0; table < config_.tables; ++table) {
        const uint32_t sig = signature_(stored, table);
        partition.tables[table][sig].push_back(entry_index);
    }
    ++partition.active_count;
    return true;
}

bool SparseProjectionAnnIndex::remove(const std::string& partition_key,
                                      size_t row_id) {
    const auto partition_it = partitions_.find(partition_key);
    if (partition_it == partitions_.end()) return false;
    auto& partition = partition_it->second;
    const auto entry_it = partition.row_to_entry.find(row_id);
    if (entry_it == partition.row_to_entry.end()) return false;
    const size_t entry_index = entry_it->second;
    if (entry_index >= partition.active.size() ||
        partition.active[entry_index] == 0) {
        return false;
    }
    partition.active[entry_index] = 0;
    partition.row_to_entry.erase(entry_it);
    if (partition.active_count > 0) --partition.active_count;
    if (partition.active_count == 0) {
        partitions_.erase(partition_it);
    }
    return true;
}

bool SparseProjectionAnnIndex::update_row_id(const std::string& partition_key,
                                             size_t old_row_id,
                                             size_t new_row_id) {
    if (old_row_id == new_row_id) return true;
    const auto partition_it = partitions_.find(partition_key);
    if (partition_it == partitions_.end()) return false;
    auto& partition = partition_it->second;
    const auto entry_it = partition.row_to_entry.find(old_row_id);
    if (entry_it == partition.row_to_entry.end()) return false;
    if (partition.row_to_entry.find(new_row_id) != partition.row_to_entry.end()) {
        return false;
    }
    const size_t entry_index = entry_it->second;
    if (entry_index >= partition.active.size() ||
        partition.active[entry_index] == 0) {
        return false;
    }
    partition.row_ids[entry_index] = new_row_id;
    partition.row_to_entry.erase(entry_it);
    partition.row_to_entry[new_row_id] = entry_index;
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
                if (entry_index >= partition.active.size() ||
                    partition.active[entry_index] == 0) {
                    continue;
                }
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
        stats.indexed_vectors += partition.active_count;
        stats.tombstone_entries +=
            partition.row_ids.size() > partition.active_count
                ? partition.row_ids.size() - partition.active_count
                : 0;
        stats.memory_bytes += partition.row_ids.capacity() * sizeof(size_t);
        stats.memory_bytes += partition.active.capacity() * sizeof(uint8_t);
        stats.memory_bytes += partition.unit_vectors.capacity() * sizeof(float);
        stats.memory_bytes +=
            partition.row_to_entry.size() *
            (sizeof(size_t) * 2 + sizeof(void*) * 2);
        for (const auto& table : partition.tables) {
            stats.buckets += table.size();
            stats.memory_bytes +=
                table.size() * (sizeof(uint32_t) + sizeof(std::vector<size_t>) +
                                sizeof(void*) * 2);
            for (const auto& [__, rows] : table) {
                stats.memory_bytes += rows.capacity() * sizeof(size_t);
                size_t active_rows = 0;
                for (const size_t entry_index : rows) {
                    if (entry_index < partition.active.size() &&
                        partition.active[entry_index] != 0) {
                        ++active_rows;
                    }
                }
                stats.max_bucket_size =
                    std::max(stats.max_bucket_size, active_rows);
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

IvfAnnIndex::IvfAnnIndex(AnnIndexConfig config)
    : config_(config) {
    config_.ivf_centroids = std::clamp<size_t>(config_.ivf_centroids, 1, 65'536);
    config_.ivf_probe = std::max<size_t>(1, config_.ivf_probe);
    config_.max_candidates = std::max<size_t>(1, config_.max_candidates);
}

bool IvfAnnIndex::add(const std::string& partition_key,
                      size_t row_id,
                      const std::vector<float>& embedding) {
    if (embedding.empty() || !ensure_dimension_(embedding.size())) return false;
    auto unit = normalize_(embedding);
    if (unit.empty()) return false;

    auto& partition = partitions_[partition_key];
    if (partition.row_to_entry.find(row_id) != partition.row_to_entry.end()) {
        return false;
    }

    size_t list = 0;
    if (partition.lists.size() < config_.ivf_centroids) {
        list = partition.lists.size();
        partition.centroids.insert(partition.centroids.end(),
                                   unit.begin(), unit.end());
        partition.lists.emplace_back();
    } else {
        list = nearest_centroid_(partition, unit);
    }

    const size_t entry_index = partition.row_ids.size();
    partition.row_ids.push_back(row_id);
    partition.active.push_back(1);
    partition.row_to_entry[row_id] = entry_index;
    partition.unit_vectors.insert(partition.unit_vectors.end(),
                                  unit.begin(), unit.end());
    partition.lists[list].push_back(entry_index);
    ++partition.active_count;
    return true;
}

bool IvfAnnIndex::remove(const std::string& partition_key,
                         size_t row_id) {
    const auto partition_it = partitions_.find(partition_key);
    if (partition_it == partitions_.end()) return false;
    auto& partition = partition_it->second;
    const auto entry_it = partition.row_to_entry.find(row_id);
    if (entry_it == partition.row_to_entry.end()) return false;
    const size_t entry_index = entry_it->second;
    if (entry_index >= partition.active.size() ||
        partition.active[entry_index] == 0) {
        return false;
    }
    partition.active[entry_index] = 0;
    partition.row_to_entry.erase(entry_it);
    if (partition.active_count > 0) --partition.active_count;
    if (partition.active_count == 0) {
        partitions_.erase(partition_it);
    }
    return true;
}

bool IvfAnnIndex::update_row_id(const std::string& partition_key,
                                size_t old_row_id,
                                size_t new_row_id) {
    if (old_row_id == new_row_id) return true;
    const auto partition_it = partitions_.find(partition_key);
    if (partition_it == partitions_.end()) return false;
    auto& partition = partition_it->second;
    const auto entry_it = partition.row_to_entry.find(old_row_id);
    if (entry_it == partition.row_to_entry.end()) return false;
    if (partition.row_to_entry.find(new_row_id) != partition.row_to_entry.end()) {
        return false;
    }
    const size_t entry_index = entry_it->second;
    if (entry_index >= partition.active.size() ||
        partition.active[entry_index] == 0) {
        return false;
    }
    partition.row_ids[entry_index] = new_row_id;
    partition.row_to_entry.erase(entry_it);
    partition.row_to_entry[new_row_id] = entry_index;
    return true;
}

std::vector<AnnSearchResult> IvfAnnIndex::search(
    const std::string& partition_key,
    const std::vector<float>& query,
    size_t limit) const {
    if (limit == 0 || query.empty() || query.size() != dimension_) return {};
    const auto partition_it = partitions_.find(partition_key);
    if (partition_it == partitions_.end()) return {};
    const auto query_unit = normalize_(query);
    if (query_unit.empty()) return {};

    const auto& partition = partition_it->second;
    if (partition.active_count == 0 || partition.lists.empty()) return {};

    std::vector<ScoredEntry> centroid_scores;
    centroid_scores.reserve(partition.lists.size());
    for (size_t list = 0; list < partition.lists.size(); ++list) {
        const float* centroid = partition.centroids.data() + list * dimension_;
        centroid_scores.push_back({list, dot_(query_unit, centroid)});
    }
    const size_t probes = std::min(config_.ivf_probe, centroid_scores.size());
    std::partial_sort(centroid_scores.begin(),
                      centroid_scores.begin() + static_cast<std::ptrdiff_t>(probes),
                      centroid_scores.end(),
                      [](const auto& a, const auto& b) {
                          if (a.similarity != b.similarity) {
                              return a.similarity > b.similarity;
                          }
                          return a.entry_index < b.entry_index;
                      });

    auto worse = [](const ScoredEntry& a, const ScoredEntry& b) {
        return worse_score(a, b);
    };
    std::vector<ScoredEntry> heap;
    heap.reserve(std::min(limit, partition.active_count));
    size_t candidates = 0;
    for (size_t i = 0; i < probes && candidates < config_.max_candidates; ++i) {
        const auto& list = partition.lists[centroid_scores[i].entry_index];
        for (const size_t entry_index : list) {
            if (entry_index >= partition.active.size() ||
                partition.active[entry_index] == 0) {
                continue;
            }
            const float* stored =
                partition.unit_vectors.data() + entry_index * dimension_;
            const double sim = dot_(query_unit, stored);
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
            ++candidates;
            if (candidates >= config_.max_candidates) break;
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

void IvfAnnIndex::clear() {
    dimension_ = 0;
    partitions_.clear();
}

bool IvfAnnIndex::empty() const {
    return partitions_.empty();
}

size_t IvfAnnIndex::dimension() const {
    return dimension_;
}

AnnIndexStats IvfAnnIndex::stats() const {
    AnnIndexStats stats;
    stats.partitions = partitions_.size();
    for (const auto& [_, partition] : partitions_) {
        stats.indexed_vectors += partition.active_count;
        stats.tombstone_entries +=
            partition.row_ids.size() > partition.active_count
                ? partition.row_ids.size() - partition.active_count
                : 0;
        stats.buckets += partition.lists.size();
        stats.memory_bytes += partition.row_ids.capacity() * sizeof(size_t);
        stats.memory_bytes += partition.active.capacity() * sizeof(uint8_t);
        stats.memory_bytes += partition.unit_vectors.capacity() * sizeof(float);
        stats.memory_bytes += partition.centroids.capacity() * sizeof(float);
        stats.memory_bytes +=
            partition.row_to_entry.size() *
            (sizeof(size_t) * 2 + sizeof(void*) * 2);
        for (const auto& list : partition.lists) {
            size_t active_rows = 0;
            for (const size_t entry_index : list) {
                if (entry_index < partition.active.size() &&
                    partition.active[entry_index] != 0) {
                    ++active_rows;
                }
            }
            stats.max_bucket_size = std::max(stats.max_bucket_size, active_rows);
            stats.memory_bytes += list.capacity() * sizeof(size_t);
        }
    }
    return stats;
}

bool IvfAnnIndex::ensure_dimension_(size_t dim) {
    if (dim == 0) return false;
    if (dimension_ != 0) return dimension_ == dim;
    dimension_ = dim;
    return true;
}

std::vector<float> IvfAnnIndex::normalize_(
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

double IvfAnnIndex::dot_(const std::vector<float>& a,
                         const float* b) const {
    double out = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        out += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return out;
}

size_t IvfAnnIndex::nearest_centroid_(
    const Partition& partition,
    const std::vector<float>& unit) const {
    size_t best = 0;
    double best_score = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < partition.lists.size(); ++i) {
        const float* centroid = partition.centroids.data() + i * dimension_;
        const double score = dot_(unit, centroid);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
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
    if (kind == AnnIndexKind::Ivf) {
        return std::make_unique<IvfAnnIndex>(config);
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
