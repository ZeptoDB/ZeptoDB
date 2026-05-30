#include "zeptodb/ai/agent_memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace zeptodb::ai {

namespace {

constexpr char kRecordsMagic[8] = {'Z', 'M', 'A', 'I', 'M', '0', '1', '\0'};
constexpr char kVectorsMagic[8] = {'Z', 'M', 'A', 'I', 'V', '0', '1', '\0'};
constexpr uint32_t kSnapshotVersion = 1;
constexpr size_t kParallelScanMinRows = 200'000;
constexpr size_t kParallelScanRowsPerThread = 50'000;

template <typename T>
bool write_pod(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return os.good();
}

template <typename T>
bool read_pod(std::istream& is, T* value) {
    is.read(reinterpret_cast<char*>(value), sizeof(T));
    return is.good();
}

bool write_string(std::ostream& os, const std::string& value) {
    const uint64_t size = static_cast<uint64_t>(value.size());
    if (!write_pod(os, size)) return false;
    os.write(value.data(), static_cast<std::streamsize>(value.size()));
    return os.good();
}

bool read_string(std::istream& is, std::string* value) {
    uint64_t size = 0;
    if (!read_pod(is, &size)) return false;
    if (size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    value->assign(static_cast<size_t>(size), '\0');
    if (size == 0) return true;
    is.read(value->data(), static_cast<std::streamsize>(size));
    return is.good();
}

bool write_magic(std::ostream& os, const char (&magic)[8]) {
    os.write(magic, sizeof(magic));
    return os.good();
}

bool read_magic(std::istream& is, const char (&expected)[8]) {
    char actual[8] = {};
    is.read(actual, sizeof(actual));
    return is.good() && std::equal(std::begin(actual), std::end(actual),
                                   std::begin(expected));
}

bool has_expiry(int64_t expires_at_ns) {
    return expires_at_ns > 0;
}

} // namespace

AgentMemoryStore::AgentMemoryStore()
    : ann_index_(make_ann_index(AnnIndexKind::SparseProjection,
                                ann_config_.index)) {
    ann_rebuild_worker_ = std::thread([this] {
        ann_rebuild_worker_loop_();
    });
}

AgentMemoryStore::~AgentMemoryStore() {
    stop_ann_rebuild_worker_();
}

void AgentMemoryStore::ann_rebuild_worker_loop_() {
    for (;;) {
        std::optional<AnnBuildSnapshot> snapshot;
        {
            std::unique_lock<std::mutex> lock(mu_);
            ann_rebuild_cv_.wait(lock, [&] {
                return ann_rebuild_stop_ || ann_rebuild_requested_;
            });
            if (ann_rebuild_stop_) return;
            ann_rebuild_requested_ = false;
            ann_rebuild_requested_generation_ = 0;
            snapshot = ann_build_snapshot_locked_();
            if (!snapshot.has_value()) continue;
            ann_rebuild_running_ = true;
            ann_rebuild_running_generation_ = snapshot->generation;
        }

        const auto result = build_and_swap_ann_index_(std::move(*snapshot));

        {
            std::lock_guard<std::mutex> lock(mu_);
            ann_rebuild_running_ = false;
            ann_rebuild_running_generation_ = 0;
            (void)result;
        }
        ann_rebuild_cv_.notify_one();
    }
}

void AgentMemoryStore::request_ann_rebuild_locked_() {
    if (ann_rebuild_stop_) return;
    if (ann_config_.mode == AgentMemoryAnnMode::Off) return;
    if (!ann_dirty_ && ann_index_) return;
    if (ann_config_.mode == AgentMemoryAnnMode::Hnsw && !hnsw_ann_available()) {
        return;
    }
    if (ann_rebuild_running_ &&
        ann_rebuild_running_generation_ == ann_generation_) {
        return;
    }
    if (ann_rebuild_requested_ &&
        ann_rebuild_requested_generation_ == ann_generation_) {
        return;
    }
    ann_rebuild_requested_ = true;
    ann_rebuild_requested_generation_ = ann_generation_;
    ann_rebuild_cv_.notify_one();
}

void AgentMemoryStore::stop_ann_rebuild_worker_() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ann_rebuild_stop_ = true;
        ann_rebuild_requested_ = false;
        ann_rebuild_requested_generation_ = 0;
    }
    ann_rebuild_cv_.notify_one();
    if (ann_rebuild_worker_.joinable()) {
        ann_rebuild_worker_.join();
    }
}

int64_t AgentMemoryStore::now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string AgentMemoryStore::normalize_prompt(const std::string& prompt) {
    std::string out;
    out.reserve(prompt.size());
    bool in_space = false;
    for (unsigned char ch : prompt) {
        if (std::isspace(ch)) {
            in_space = true;
            continue;
        }
        if (in_space && !out.empty()) out.push_back(' ');
        out.push_back(static_cast<char>(std::tolower(ch)));
        in_space = false;
    }
    return out;
}

double AgentMemoryStore::cosine_similarity(const std::vector<float>& a,
                                           const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0;
    double dot = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        aa += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        bb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    if (aa <= 0.0 || bb <= 0.0) return 0.0;
    return dot / (std::sqrt(aa) * std::sqrt(bb));
}

int64_t AgentMemoryStore::estimate_tokens(const std::string& content) {
    if (content.empty()) return 0;
    return static_cast<int64_t>((content.size() + 3) / 4);
}

StoreResult AgentMemoryStore::put_memory(MemoryRecord record) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string error;
    const size_t old_embedding_dim = embedding_dim_;
    if (!validate_embedding_(record.embedding, &error)) {
        return {false, {}, error};
    }
    if (old_embedding_dim == 0 && embedding_dim_ != 0 && !memories_.empty()) {
        memory_unit_embeddings_.assign(memories_.size() * embedding_dim_, 0.0f);
        memory_has_embedding_.assign(memories_.size(), 0);
    }
    if (old_embedding_dim == 0 && embedding_dim_ != 0) {
        memory_unit_embeddings_.reserve(memories_.capacity() * embedding_dim_);
        memory_has_embedding_.reserve(memories_.capacity());
    }
    if (record.memory_id.empty()) record.memory_id = next_id_("mem");
    if (record.namespace_id.empty()) record.namespace_id = "default";
    if (record.created_at_ns == 0) record.created_at_ns = now_ns();
    if (record.last_accessed_ns == 0) record.last_accessed_ns = record.created_at_ns;
    if (record.token_count < 0) {
        return {false, {}, "token_count must be non-negative"};
    }
    if (record.token_count == 0) record.token_count = estimate_tokens(record.content);

    const std::string id = record.memory_id;
    const bool ann_was_clean =
        ann_config_.mode != AgentMemoryAnnMode::Off && ann_index_ && !ann_dirty_;
    const size_t memory_count_before = memories_.size();
    bool appended_memory = false;
    size_t appended_row = 0;
    bool updated_memory = false;
    size_t updated_row = 0;
    bool update_preserves_ann_candidates = false;
    const auto it = memory_index_.find(record.memory_id);
    if (it != memory_index_.end()) {
        updated_memory = true;
        updated_row = it->second;
        auto& existing = memories_[it->second];
        if (!existing.tenant_id.empty() && existing.tenant_id != record.tenant_id) {
            return {false, {}, "memory_id belongs to a different tenant"};
        }
        update_preserves_ann_candidates =
            existing.tenant_id == record.tenant_id &&
            existing.namespace_id == record.namespace_id &&
            existing.embedding == record.embedding;
        const bool had_expiry = has_expiry(existing.expires_at_ns);
        const bool has_new_expiry = has_expiry(record.expires_at_ns);
        existing = std::move(record);
        set_unit_embedding_locked_(it->second, existing.embedding);
        if (had_expiry != has_new_expiry) {
            if (has_new_expiry) {
                ++expiring_memory_count_;
            } else if (expiring_memory_count_ > 0) {
                --expiring_memory_count_;
            }
        }
    } else {
        const bool has_new_expiry = has_expiry(record.expires_at_ns);
        appended_memory = true;
        appended_row = memories_.size();
        memory_index_[record.memory_id] = appended_row;
        memories_.push_back(std::move(record));
        append_unit_embedding_locked_(memories_.back().embedding);
        if (has_new_expiry) ++expiring_memory_count_;
    }
    const int64_t now = now_ns();
    if (eviction_config_.evict_expired_on_write) {
        evict_expired_locked_(now);
    }
    enforce_eviction_locked_(now);
    const auto appended_it = memory_index_.find(id);
    const bool append_still_valid =
        appended_memory &&
        memories_.size() == memory_count_before + 1 &&
        appended_it != memory_index_.end() &&
        appended_it->second == appended_row;
    const bool update_still_valid =
        updated_memory &&
        memories_.size() == memory_count_before &&
        appended_it != memory_index_.end() &&
        appended_it->second == updated_row;
    if (ann_was_clean && !ann_dirty_ && append_still_valid) {
        if (!try_add_ann_entry_locked_(appended_row, memories_[appended_row])) {
            mark_ann_dirty_locked_();
        }
    } else if (update_still_valid && update_preserves_ann_candidates) {
        // Metadata/ranking-only updates do not affect ANN candidate generation.
        return {true, id, {}};
    } else {
        mark_ann_dirty_locked_();
    }
    return {true, id, {}};
}

std::optional<MemoryRecord> AgentMemoryStore::get_memory(
    const std::string& memory_id,
    const std::string& tenant_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = memory_index_.find(memory_id);
    if (it == memory_index_.end()) return std::nullopt;
    const auto& record = memories_[it->second];
    if (!tenant_id.empty() && record.tenant_id != tenant_id) return std::nullopt;
    return record;
}

std::optional<CacheEntry> AgentMemoryStore::get_cache(
    const std::string& tenant_id,
    const std::string& namespace_id,
    const std::string& prompt) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string scope = namespace_id.empty() ? "default" : namespace_id;
    const std::string exact_key = tenant_id + "\n" + scope + "\n"
        + normalize_prompt(prompt);
    const auto it = cache_exact_index_.find(exact_key);
    if (it == cache_exact_index_.end()) return std::nullopt;
    return cache_entries_[it->second];
}

std::vector<MemoryRecord> AgentMemoryStore::memory_records_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return memories_;
}

std::vector<CacheEntry> AgentMemoryStore::cache_entries_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_entries_;
}

bool AgentMemoryStore::remove_memory(const std::string& memory_id,
                                     const std::string& tenant_id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = memory_index_.find(memory_id);
    if (it == memory_index_.end()) return false;
    const auto row = it->second;
    if (!tenant_id.empty() && memories_[row].tenant_id != tenant_id) {
        return false;
    }
    if (has_expiry(memories_[row].expires_at_ns) && expiring_memory_count_ > 0) {
        --expiring_memory_count_;
    }
    memories_.erase(memories_.begin() + static_cast<std::ptrdiff_t>(row));
    rebuild_memory_index_locked_();
    rebuild_unit_embeddings_locked_();
    mark_ann_dirty_locked_();
    return true;
}

std::vector<MemorySearchResult> AgentMemoryStore::search(MemoryQuery query) {
    if (query.limit == 0) return {};
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = query.now_ns == 0 ? now_ns() : query.now_ns;

    if (should_use_ann_locked_(query)) {
        if (ann_dirty_ || !ann_index_) {
            request_ann_rebuild_locked_();
        }

        const bool can_use_ann = ann_index_ && !ann_dirty_;
        if (can_use_ann) {
            const size_t candidate_limit = std::max(
                query.limit,
                query.limit * std::max<size_t>(ann_config_.oversample, 1));
            const auto ann_rows = ann_index_->search(
                ann_partition_key_(query.tenant_id, query.namespace_id),
                query.query_embedding,
                candidate_limit);
            std::vector<size_t> row_ids;
            row_ids.reserve(ann_rows.size());
            for (const auto& row : ann_rows) row_ids.push_back(row.row_id);
            auto matches = search_scan_locked_(query, now, &row_ids);
            if (matches.size() >= std::min(query.limit, memories_.size())) {
                ++ann_search_count_;
                return matches;
            }
        }
        ++ann_fallback_count_;
    }

    return search_scan_locked_(query, now, nullptr);
}

ContextResult AgentMemoryStore::get_context(ContextRequest request) {
    const int64_t budget = request.token_budget;
    if (budget < 0) return {};
    auto candidates = search(request);

    ContextResult out;
    std::unordered_set<std::string> seen_content;
    for (auto& candidate : candidates) {
        if (!candidate.record.content.empty() &&
            !seen_content.insert(candidate.record.content).second) {
            continue;
        }
        const int64_t tokens = candidate.record.token_count > 0
            ? candidate.record.token_count
            : estimate_tokens(candidate.record.content);
        if (budget > 0 && tokens > budget - out.token_count) continue;
        out.token_count += tokens;
        out.memories.push_back(std::move(candidate));
    }
    return out;
}

StoreResult AgentMemoryStore::store_cache(CacheEntry entry) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string error;
    if (!validate_embedding_(entry.embedding, &error)) {
        return {false, {}, error};
    }
    if (entry.prompt.empty()) return {false, {}, "prompt is required"};
    if (entry.response.empty()) return {false, {}, "response is required"};
    if (entry.cache_id.empty()) entry.cache_id = next_id_("cache");
    if (entry.namespace_id.empty()) entry.namespace_id = "default";
    if (entry.created_at_ns == 0) entry.created_at_ns = now_ns();
    if (entry.last_accessed_ns == 0) entry.last_accessed_ns = entry.created_at_ns;
    if (entry.token_count < 0) return {false, {}, "token_count must be non-negative"};

    const std::string exact_key = entry.tenant_id + "\n" + entry.namespace_id + "\n"
        + normalize_prompt(entry.prompt);
    const auto it = cache_exact_index_.find(exact_key);
    const std::string id = entry.cache_id;
    if (it != cache_exact_index_.end()) {
        auto& existing = cache_entries_[it->second];
        if (!existing.tenant_id.empty() && !entry.tenant_id.empty() &&
            existing.tenant_id != entry.tenant_id) {
            return {false, {}, "cache prompt belongs to a different tenant"};
        }
        const bool had_expiry = has_expiry(existing.expires_at_ns);
        const bool has_new_expiry = has_expiry(entry.expires_at_ns);
        existing = std::move(entry);
        if (had_expiry != has_new_expiry) {
            if (has_new_expiry) {
                ++expiring_cache_count_;
            } else if (expiring_cache_count_ > 0) {
                --expiring_cache_count_;
            }
        }
    } else {
        const bool has_new_expiry = has_expiry(entry.expires_at_ns);
        cache_exact_index_[exact_key] = cache_entries_.size();
        cache_entries_.push_back(std::move(entry));
        if (has_new_expiry) ++expiring_cache_count_;
    }
    const int64_t now = now_ns();
    if (eviction_config_.evict_expired_on_write) {
        evict_expired_locked_(now);
    }
    enforce_eviction_locked_(now);
    return {true, id, {}};
}

bool AgentMemoryStore::remove_cache(const std::string& tenant_id,
                                    const std::string& namespace_id,
                                    const std::string& prompt) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string scope = namespace_id.empty() ? "default" : namespace_id;
    const std::string exact_key = tenant_id + "\n" + scope + "\n"
        + normalize_prompt(prompt);
    const auto it = cache_exact_index_.find(exact_key);
    if (it == cache_exact_index_.end()) return false;
    const auto row = it->second;
    if (has_expiry(cache_entries_[row].expires_at_ns) &&
        expiring_cache_count_ > 0) {
        --expiring_cache_count_;
    }
    cache_entries_.erase(cache_entries_.begin() + static_cast<std::ptrdiff_t>(row));
    rebuild_cache_index_locked_();
    return true;
}

CacheLookupResult AgentMemoryStore::lookup_cache(CacheLookup lookup) {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = lookup.now_ns == 0 ? now_ns() : lookup.now_ns;
    if (lookup.namespace_id.empty()) lookup.namespace_id = "default";
    const std::string exact_key = lookup.tenant_id + "\n" + lookup.namespace_id + "\n"
        + normalize_prompt(lookup.prompt);

    const auto exact = cache_exact_index_.find(exact_key);
    if (exact != cache_exact_index_.end()) {
        auto& entry = cache_entries_[exact->second];
        if (!expired_(entry.expires_at_ns, now)) {
            entry.access_count += 1;
            entry.last_accessed_ns = now;
            return {true, true, 1.0, entry};
        }
    }

    CacheLookupResult best;
    for (auto& entry : cache_entries_) {
        if (entry.tenant_id != lookup.tenant_id ||
            entry.namespace_id != lookup.namespace_id ||
            expired_(entry.expires_at_ns, now)) {
            continue;
        }
        const double sim = cosine_similarity(lookup.embedding, entry.embedding);
        if (sim >= lookup.semantic_threshold && (!best.hit || sim > best.score)) {
            best.hit = true;
            best.exact = false;
            best.score = sim;
            best.entry = entry;
        }
    }
    if (best.hit) {
        auto it = std::find_if(cache_entries_.begin(), cache_entries_.end(),
            [&](const CacheEntry& entry) {
                return entry.cache_id == best.entry.cache_id;
            });
        if (it != cache_entries_.end()) {
            it->access_count += 1;
            it->last_accessed_ns = now;
            best.entry = *it;
        }
    }
    return best;
}

void AgentMemoryStore::set_eviction_config(AgentMemoryEvictionConfig config) {
    std::lock_guard<std::mutex> lock(mu_);
    eviction_config_ = config;
    const int64_t now = now_ns();
    if (eviction_config_.evict_expired_on_write) {
        evict_expired_locked_(now);
    }
    enforce_eviction_locked_(now);
}

AgentMemoryEvictionConfig AgentMemoryStore::eviction_config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return eviction_config_;
}

void AgentMemoryStore::set_ann_config(AgentMemoryAnnConfig config) {
    std::lock_guard<std::mutex> lock(mu_);
    config.oversample = std::max<size_t>(config.oversample, 1);
    ann_config_ = config;
    const AnnIndexKind kind = ann_config_.mode == AgentMemoryAnnMode::Hnsw
        ? AnnIndexKind::Hnsw
        : AnnIndexKind::SparseProjection;
    ann_index_ = make_ann_index(kind, ann_config_.index);
    mark_ann_dirty_locked_();
}

AgentMemoryAnnConfig AgentMemoryStore::ann_config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ann_config_;
}

StoreResult AgentMemoryStore::rebuild_ann_index() {
    std::optional<AnnBuildSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ann_dirty_ && ann_index_) return {true, {}, {}};
        snapshot = ann_build_snapshot_locked_();
    }
    if (!snapshot.has_value()) return {true, {}, {}};
    return build_and_swap_ann_index_(std::move(*snapshot));
}

void AgentMemoryStore::set_id_config(AgentMemoryIdConfig config) {
    std::lock_guard<std::mutex> lock(mu_);
    id_config_ = config;
}

AgentMemoryIdConfig AgentMemoryStore::id_config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return id_config_;
}

void AgentMemoryStore::reserve_memory_capacity(size_t memory_count,
                                               size_t cache_count) {
    std::lock_guard<std::mutex> lock(mu_);
    memories_.reserve(memory_count);
    memory_index_.reserve(memory_count);
    memory_has_embedding_.reserve(memory_count);
    if (embedding_dim_ != 0) {
        memory_unit_embeddings_.reserve(memory_count * embedding_dim_);
    }
    cache_entries_.reserve(cache_count);
    cache_exact_index_.reserve(cache_count);
}

size_t AgentMemoryStore::evict_expired(int64_t now_ns_arg) {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = now_ns_arg == 0 ? now_ns() : now_ns_arg;
    return evict_expired_locked_(now);
}

StoreResult AgentMemoryStore::save_to_directory(const std::string& directory) const {
    namespace fs = std::filesystem;
    if (directory.empty()) return {false, {}, "snapshot directory is required"};

    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) return {false, {}, "failed to create snapshot directory: " + ec.message()};

    const fs::path dir(directory);
    const fs::path records_tmp = dir / "records.bin.tmp";
    const fs::path vectors_tmp = dir / "vectors.bin.tmp";
    const fs::path records_path = dir / "records.bin";
    const fs::path vectors_path = dir / "vectors.bin";

    std::ofstream records(records_tmp, std::ios::binary | std::ios::trunc);
    std::ofstream vectors(vectors_tmp, std::ios::binary | std::ios::trunc);
    if (!records.is_open() || !vectors.is_open()) {
        return {false, {}, "failed to open snapshot files for writing"};
    }

    if (!write_magic(records, kRecordsMagic) ||
        !write_pod(records, kSnapshotVersion) ||
        !write_pod(records, static_cast<uint64_t>(embedding_dim_)) ||
        !write_pod(records, next_memory_id_) ||
        !write_pod(records, next_cache_id_) ||
        !write_pod(records, static_cast<uint64_t>(memories_.size())) ||
        !write_pod(records, static_cast<uint64_t>(cache_entries_.size())) ||
        !write_magic(vectors, kVectorsMagic) ||
        !write_pod(vectors, kSnapshotVersion)) {
        return {false, {}, "failed to write snapshot header"};
    }

    uint64_t vector_offset = 0;
    auto write_embedding = [&](const std::vector<float>& embedding,
                               uint64_t* offset,
                               uint64_t* count) {
        *offset = vector_offset;
        *count = static_cast<uint64_t>(embedding.size());
        if (!embedding.empty()) {
            vectors.write(reinterpret_cast<const char*>(embedding.data()),
                          static_cast<std::streamsize>(embedding.size() * sizeof(float)));
            vector_offset += *count;
        }
        return vectors.good();
    };

    for (const auto& r : memories_) {
        uint64_t offset = 0;
        uint64_t count = 0;
        if (!write_embedding(r.embedding, &offset, &count) ||
            !write_string(records, r.memory_id) ||
            !write_string(records, r.tenant_id) ||
            !write_string(records, r.namespace_id) ||
            !write_string(records, r.user_id) ||
            !write_string(records, r.session_id) ||
            !write_string(records, r.agent_id) ||
            !write_string(records, r.type) ||
            !write_string(records, r.content) ||
            !write_string(records, r.metadata_json) ||
            !write_pod(records, r.token_count) ||
            !write_pod(records, r.importance) ||
            !write_pod(records, r.created_at_ns) ||
            !write_pod(records, r.last_accessed_ns) ||
            !write_pod(records, r.expires_at_ns) ||
            !write_pod(records, static_cast<uint8_t>(r.pinned ? 1 : 0)) ||
            !write_pod(records, r.access_count) ||
            !write_pod(records, offset) ||
            !write_pod(records, count)) {
            return {false, {}, "failed to write memory snapshot"};
        }
    }

    for (const auto& c : cache_entries_) {
        uint64_t offset = 0;
        uint64_t count = 0;
        if (!write_embedding(c.embedding, &offset, &count) ||
            !write_string(records, c.cache_id) ||
            !write_string(records, c.tenant_id) ||
            !write_string(records, c.namespace_id) ||
            !write_string(records, c.prompt) ||
            !write_string(records, c.response) ||
            !write_string(records, c.metadata_json) ||
            !write_pod(records, c.token_count) ||
            !write_pod(records, c.created_at_ns) ||
            !write_pod(records, c.last_accessed_ns) ||
            !write_pod(records, c.expires_at_ns) ||
            !write_pod(records, c.access_count) ||
            !write_pod(records, offset) ||
            !write_pod(records, count)) {
            return {false, {}, "failed to write cache snapshot"};
        }
    }

    records.close();
    vectors.close();
    if (!records || !vectors) return {false, {}, "failed to flush snapshot files"};

    fs::remove(records_path, ec);
    ec.clear();
    fs::rename(records_tmp, records_path, ec);
    if (ec) return {false, {}, "failed to publish records snapshot: " + ec.message()};
    fs::remove(vectors_path, ec);
    ec.clear();
    fs::rename(vectors_tmp, vectors_path, ec);
    if (ec) return {false, {}, "failed to publish vectors snapshot: " + ec.message()};

    return {true, directory, {}};
}

StoreResult AgentMemoryStore::load_from_directory(const std::string& directory) {
    namespace fs = std::filesystem;
    if (directory.empty()) return {false, {}, "snapshot directory is required"};

    const fs::path dir(directory);
    const fs::path records_path = dir / "records.bin";
    const fs::path vectors_path = dir / "vectors.bin";
    if (!fs::exists(records_path) && !fs::exists(vectors_path)) {
        return {true, directory, {}};
    }
    if (!fs::exists(records_path) || !fs::exists(vectors_path)) {
        return {false, {}, "partial agent memory snapshot"};
    }

    std::ifstream records(records_path, std::ios::binary);
    std::ifstream vectors(vectors_path, std::ios::binary);
    if (!records.is_open() || !vectors.is_open()) {
        return {false, {}, "failed to open snapshot files for reading"};
    }

    uint32_t records_version = 0;
    uint32_t vectors_version = 0;
    uint64_t embedding_dim = 0;
    uint64_t next_memory_id = 1;
    uint64_t next_cache_id = 1;
    uint64_t memory_count = 0;
    uint64_t cache_count = 0;
    if (!read_magic(records, kRecordsMagic) ||
        !read_pod(records, &records_version) ||
        records_version != kSnapshotVersion ||
        !read_pod(records, &embedding_dim) ||
        !read_pod(records, &next_memory_id) ||
        !read_pod(records, &next_cache_id) ||
        !read_pod(records, &memory_count) ||
        !read_pod(records, &cache_count) ||
        !read_magic(vectors, kVectorsMagic) ||
        !read_pod(vectors, &vectors_version) ||
        vectors_version != kSnapshotVersion) {
        return {false, {}, "invalid agent memory snapshot header"};
    }

    std::vector<float> vector_arena;
    vectors.seekg(0, std::ios::end);
    const auto vector_bytes = vectors.tellg();
    if (vector_bytes < static_cast<std::streamoff>(sizeof(kVectorsMagic) + sizeof(uint32_t))) {
        return {false, {}, "invalid vector snapshot size"};
    }
    const auto payload_bytes =
        vector_bytes - static_cast<std::streamoff>(sizeof(kVectorsMagic) + sizeof(uint32_t));
    if (payload_bytes % static_cast<std::streamoff>(sizeof(float)) != 0) {
        return {false, {}, "vector snapshot payload is misaligned"};
    }
    vector_arena.resize(static_cast<size_t>(payload_bytes / sizeof(float)));
    vectors.seekg(static_cast<std::streamoff>(sizeof(kVectorsMagic) + sizeof(uint32_t)),
                  std::ios::beg);
    if (!vector_arena.empty()) {
        vectors.read(reinterpret_cast<char*>(vector_arena.data()), payload_bytes);
        if (!vectors.good()) return {false, {}, "failed to read vector snapshot"};
    }

    auto read_embedding = [&](uint64_t offset,
                              uint64_t count,
                              std::vector<float>* embedding) {
        if (offset > vector_arena.size() || count > vector_arena.size() - offset) {
            return false;
        }
        embedding->assign(vector_arena.begin() + static_cast<std::ptrdiff_t>(offset),
                          vector_arena.begin() + static_cast<std::ptrdiff_t>(offset + count));
        return true;
    };

    std::vector<MemoryRecord> loaded_memories;
    std::vector<CacheEntry> loaded_cache;
    loaded_memories.reserve(static_cast<size_t>(memory_count));
    loaded_cache.reserve(static_cast<size_t>(cache_count));

    for (uint64_t i = 0; i < memory_count; ++i) {
        MemoryRecord r;
        uint8_t pinned = 0;
        uint64_t offset = 0;
        uint64_t count = 0;
        if (!read_string(records, &r.memory_id) ||
            !read_string(records, &r.tenant_id) ||
            !read_string(records, &r.namespace_id) ||
            !read_string(records, &r.user_id) ||
            !read_string(records, &r.session_id) ||
            !read_string(records, &r.agent_id) ||
            !read_string(records, &r.type) ||
            !read_string(records, &r.content) ||
            !read_string(records, &r.metadata_json) ||
            !read_pod(records, &r.token_count) ||
            !read_pod(records, &r.importance) ||
            !read_pod(records, &r.created_at_ns) ||
            !read_pod(records, &r.last_accessed_ns) ||
            !read_pod(records, &r.expires_at_ns) ||
            !read_pod(records, &pinned) ||
            !read_pod(records, &r.access_count) ||
            !read_pod(records, &offset) ||
            !read_pod(records, &count) ||
            !read_embedding(offset, count, &r.embedding)) {
            return {false, {}, "failed to read memory snapshot"};
        }
        r.pinned = pinned != 0;
        loaded_memories.push_back(std::move(r));
    }

    for (uint64_t i = 0; i < cache_count; ++i) {
        CacheEntry c;
        uint64_t offset = 0;
        uint64_t count = 0;
        if (!read_string(records, &c.cache_id) ||
            !read_string(records, &c.tenant_id) ||
            !read_string(records, &c.namespace_id) ||
            !read_string(records, &c.prompt) ||
            !read_string(records, &c.response) ||
            !read_string(records, &c.metadata_json) ||
            !read_pod(records, &c.token_count) ||
            !read_pod(records, &c.created_at_ns) ||
            !read_pod(records, &c.last_accessed_ns) ||
            !read_pod(records, &c.expires_at_ns) ||
            !read_pod(records, &c.access_count) ||
            !read_pod(records, &offset) ||
            !read_pod(records, &count) ||
            !read_embedding(offset, count, &c.embedding)) {
            return {false, {}, "failed to read cache snapshot"};
        }
        loaded_cache.push_back(std::move(c));
    }

    std::unordered_map<std::string, size_t> loaded_memory_index;
    std::unordered_map<std::string, size_t> loaded_cache_exact_index;
    for (size_t i = 0; i < loaded_memories.size(); ++i) {
        const auto& r = loaded_memories[i];
        if (!r.embedding.empty() && r.embedding.size() != embedding_dim) {
            return {false, {}, "memory embedding dimension mismatch in snapshot"};
        }
        if (!loaded_memory_index.emplace(r.memory_id, i).second) {
            return {false, {}, "duplicate memory_id in snapshot"};
        }
    }
    for (size_t i = 0; i < loaded_cache.size(); ++i) {
        const auto& c = loaded_cache[i];
        if (!c.embedding.empty() && c.embedding.size() != embedding_dim) {
            return {false, {}, "cache embedding dimension mismatch in snapshot"};
        }
        const std::string exact_key = c.tenant_id + "\n" + c.namespace_id + "\n"
            + normalize_prompt(c.prompt);
        loaded_cache_exact_index[exact_key] = i;
    }

    std::lock_guard<std::mutex> lock(mu_);
    embedding_dim_ = static_cast<size_t>(embedding_dim);
    next_memory_id_ = next_memory_id;
    next_cache_id_ = next_cache_id;
    memories_ = std::move(loaded_memories);
    memory_index_ = std::move(loaded_memory_index);
    cache_entries_ = std::move(loaded_cache);
    cache_exact_index_ = std::move(loaded_cache_exact_index);
    rebuild_expiry_counts_locked_();
    rebuild_unit_embeddings_locked_();
    mark_ann_dirty_locked_();
    return {true, directory, {}};
}

AgentMemoryStats AgentMemoryStore::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    AgentMemoryStats stats;
    stats.memory_count = memories_.size();
    stats.cache_count = cache_entries_.size();
    stats.embedding_dim = embedding_dim_;
    stats.evicted_memory_count = evicted_memory_count_;
    stats.evicted_cache_count = evicted_cache_count_;
    stats.ann_enabled = ann_config_.mode != AgentMemoryAnnMode::Off;
    stats.ann_rebuild_count = ann_rebuild_count_;
    stats.ann_last_rebuild_ms = ann_last_rebuild_ms_;
    stats.ann_search_count = ann_search_count_;
    stats.ann_fallback_count = ann_fallback_count_;
    if (ann_index_) {
        const auto ann = ann_index_->stats();
        stats.ann_indexed_vectors = ann.indexed_vectors;
        stats.ann_partitions = ann.partitions;
        stats.ann_buckets = ann.buckets;
        stats.ann_max_bucket_size = ann.max_bucket_size;
    }
    return stats;
}

void AgentMemoryStore::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    embedding_dim_ = 0;
    next_memory_id_ = 1;
    next_cache_id_ = 1;
    evicted_memory_count_ = 0;
    evicted_cache_count_ = 0;
    expiring_memory_count_ = 0;
    expiring_cache_count_ = 0;
    id_config_ = AgentMemoryIdConfig{};
    eviction_config_ = AgentMemoryEvictionConfig{};
    ann_config_ = AgentMemoryAnnConfig{};
    ann_index_ = make_ann_index(AnnIndexKind::SparseProjection,
                                ann_config_.index);
    ann_dirty_ = true;
    ann_rebuild_requested_ = false;
    ann_rebuild_requested_generation_ = 0;
    ++ann_generation_;
    ann_rebuild_count_ = 0;
    ann_last_rebuild_ms_ = 0.0;
    ann_search_count_ = 0;
    ann_fallback_count_ = 0;
    memory_unit_embeddings_.clear();
    memory_has_embedding_.clear();
    memories_.clear();
    memory_index_.clear();
    cache_entries_.clear();
    cache_exact_index_.clear();
}

bool AgentMemoryStore::validate_embedding_(const std::vector<float>& embedding,
                                           std::string* error) {
    if (embedding.empty()) return true;
    for (float value : embedding) {
        if (!std::isfinite(value)) {
            if (error) *error = "embedding contains NaN or Inf";
            return false;
        }
    }
    if (embedding_dim_ != 0 && embedding.size() != embedding_dim_) {
        if (error) {
            *error = "embedding dimension mismatch: expected "
                + std::to_string(embedding_dim_) + ", got "
                + std::to_string(embedding.size());
        }
        return false;
    }
    if (embedding_dim_ == 0) embedding_dim_ = embedding.size();
    return true;
}

bool AgentMemoryStore::expired_(int64_t expires_at_ns, int64_t now) const {
    return expires_at_ns > 0 && expires_at_ns <= now;
}

bool AgentMemoryStore::matches_(const MemoryRecord& record,
                                const MemoryQuery& query,
                                int64_t now) const {
    if (!query.include_expired && expired_(record.expires_at_ns, now)) return false;
    if (!query.tenant_id.empty() && record.tenant_id != query.tenant_id) return false;
    if (!query.namespace_id.empty() && record.namespace_id != query.namespace_id) return false;
    if (!query.user_id.empty() && record.user_id != query.user_id) return false;
    if (!query.session_id.empty() && record.session_id != query.session_id) return false;
    if (!query.agent_id.empty() && record.agent_id != query.agent_id) return false;
    if (!query.type.empty() && record.type != query.type) return false;
    return true;
}

double AgentMemoryStore::score_(const MemoryRecord& record,
                                const MemoryQuery& query,
                                int64_t now,
                                double similarity) const {
    double score = query.query_embedding.empty() ? 0.0 : similarity * 10.0;
    score += record.importance;
    if (record.pinned) score += 5.0;
    if (record.created_at_ns > 0 && now > record.created_at_ns) {
        const double age_hours =
            static_cast<double>(now - record.created_at_ns) / 3'600'000'000'000.0;
        score += 1.0 / (1.0 + std::max(0.0, age_hours));
    }
    score += std::min<double>(1.0, static_cast<double>(record.access_count) * 0.01);
    return score;
}

std::vector<float> AgentMemoryStore::normalize_embedding_(
    const std::vector<float>& embedding) const {
    if (embedding.empty()) return {};
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

void AgentMemoryStore::append_unit_embedding_locked_(
    const std::vector<float>& embedding) {
    if (embedding_dim_ == 0) {
        memory_has_embedding_.push_back(0);
        return;
    }
    const auto unit = normalize_embedding_(embedding);
    memory_has_embedding_.push_back(unit.empty() ? 0 : 1);
    const size_t old_size = memory_unit_embeddings_.size();
    memory_unit_embeddings_.resize(old_size + embedding_dim_, 0.0f);
    if (!unit.empty()) {
        std::copy(unit.begin(), unit.end(), memory_unit_embeddings_.begin() +
                                             static_cast<std::ptrdiff_t>(old_size));
    }
}

void AgentMemoryStore::set_unit_embedding_locked_(
    size_t idx,
    const std::vector<float>& embedding) {
    if (idx >= memories_.size() || embedding_dim_ == 0) return;
    if (memory_has_embedding_.size() != memories_.size() ||
        memory_unit_embeddings_.size() != memories_.size() * embedding_dim_) {
        rebuild_unit_embeddings_locked_();
        return;
    }
    const auto unit = normalize_embedding_(embedding);
    memory_has_embedding_[idx] = unit.empty() ? 0 : 1;
    auto out = memory_unit_embeddings_.begin() +
        static_cast<std::ptrdiff_t>(idx * embedding_dim_);
    std::fill(out, out + static_cast<std::ptrdiff_t>(embedding_dim_), 0.0f);
    if (!unit.empty()) {
        std::copy(unit.begin(), unit.end(), out);
    }
}

void AgentMemoryStore::rebuild_unit_embeddings_locked_() {
    memory_has_embedding_.assign(memories_.size(), 0);
    memory_unit_embeddings_.clear();
    if (embedding_dim_ == 0) return;
    memory_unit_embeddings_.assign(memories_.size() * embedding_dim_, 0.0f);
    for (size_t i = 0; i < memories_.size(); ++i) {
        const auto unit = normalize_embedding_(memories_[i].embedding);
        if (unit.empty()) continue;
        memory_has_embedding_[i] = 1;
        std::copy(unit.begin(), unit.end(),
                  memory_unit_embeddings_.begin() +
                      static_cast<std::ptrdiff_t>(i * embedding_dim_));
    }
}

double AgentMemoryStore::memory_similarity_locked_(
    size_t idx,
    const std::vector<float>& query_unit) const {
    if (query_unit.empty() || idx >= memory_has_embedding_.size() ||
        memory_has_embedding_[idx] == 0 || embedding_dim_ == 0 ||
        query_unit.size() != embedding_dim_ ||
        memory_unit_embeddings_.size() < (idx + 1) * embedding_dim_) {
        return 0.0;
    }
    const float* row = memory_unit_embeddings_.data() + idx * embedding_dim_;
    double dot = 0.0;
    for (size_t i = 0; i < embedding_dim_; ++i) {
        dot += static_cast<double>(query_unit[i]) * static_cast<double>(row[i]);
    }
    return dot;
}

std::vector<MemorySearchResult> AgentMemoryStore::search_scan_locked_(
    const MemoryQuery& query,
    int64_t now,
    const std::vector<size_t>* candidate_rows) {
    const auto query_unit = normalize_embedding_(query.query_embedding);
    struct MatchCandidate {
        size_t idx = 0;
        double score = 0.0;
        double similarity = 0.0;
        int64_t created_at_ns = 0;
    };
    auto better_candidate = [](const MatchCandidate& a,
                               const MatchCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.created_at_ns > b.created_at_ns;
    };

    auto add_candidate = [&](std::vector<MatchCandidate>* heap,
                             MatchCandidate candidate) {
        if (heap->size() < query.limit) {
            heap->push_back(std::move(candidate));
            std::push_heap(heap->begin(), heap->end(), better_candidate);
        } else if (better_candidate(candidate, heap->front())) {
            std::pop_heap(heap->begin(), heap->end(), better_candidate);
            heap->back() = std::move(candidate);
            std::push_heap(heap->begin(), heap->end(), better_candidate);
        }
    };
    auto scan_range = [&](size_t begin, size_t end) {
        std::vector<MatchCandidate> local;
        local.reserve(std::min(query.limit, end - begin));
        for (size_t i = begin; i < end; ++i) {
            const auto& record = memories_[i];
            if (!matches_(record, query, now)) continue;
            const double sim = memory_similarity_locked_(i, query_unit);
            MatchCandidate candidate;
            candidate.idx = i;
            candidate.similarity = sim;
            candidate.score = score_(record, query, now, sim);
            candidate.created_at_ns = record.created_at_ns;
            add_candidate(&local, std::move(candidate));
        }
        return local;
    };
    auto scan_rows = [&](const std::vector<size_t>& rows) {
        std::vector<MatchCandidate> local;
        local.reserve(std::min(query.limit, rows.size()));
        for (const size_t i : rows) {
            if (i >= memories_.size()) continue;
            const auto& record = memories_[i];
            if (!matches_(record, query, now)) continue;
            const double sim = memory_similarity_locked_(i, query_unit);
            MatchCandidate candidate;
            candidate.idx = i;
            candidate.similarity = sim;
            candidate.score = score_(record, query, now, sim);
            candidate.created_at_ns = record.created_at_ns;
            add_candidate(&local, std::move(candidate));
        }
        return local;
    };

    std::vector<MatchCandidate> matches;
    matches.reserve(std::min(query.limit, memories_.size()));
    auto merge_matches = [&](std::vector<MatchCandidate>&& local) {
        for (auto& candidate : local) {
            add_candidate(&matches, std::move(candidate));
        }
    };

    if (candidate_rows == nullptr && memories_.size() >= kParallelScanMinRows) {
        const unsigned hardware_threads = std::thread::hardware_concurrency();
        const size_t max_threads = hardware_threads == 0
            ? 1
            : static_cast<size_t>(hardware_threads);
        const size_t thread_count = std::min(
            max_threads,
            std::max<size_t>(1, memories_.size() / kParallelScanRowsPerThread));
        if (thread_count > 1) {
            std::vector<std::vector<MatchCandidate>> partials(thread_count);
            std::vector<std::thread> workers;
            workers.reserve(thread_count);
            const size_t chunk = (memories_.size() + thread_count - 1) / thread_count;
            for (size_t t = 0; t < thread_count; ++t) {
                const size_t begin = t * chunk;
                const size_t end = std::min(memories_.size(), begin + chunk);
                workers.emplace_back([&, t, begin, end] {
                    partials[t] = scan_range(begin, end);
                });
            }
            for (auto& worker : workers) worker.join();
            for (auto& partial : partials) merge_matches(std::move(partial));
        } else {
            merge_matches(scan_range(0, memories_.size()));
        }
    } else if (candidate_rows == nullptr) {
        merge_matches(scan_range(0, memories_.size()));
    } else {
        merge_matches(scan_rows(*candidate_rows));
    }

    std::sort(matches.begin(), matches.end(),
        [](const auto& a, const auto& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.created_at_ns > b.created_at_ns;
        });

    std::vector<MemorySearchResult> out;
    out.reserve(matches.size());
    for (auto& match : matches) {
        const size_t idx = match.idx;
        auto& record = memories_[idx];
        if (query.update_access) {
            record.access_count += 1;
            record.last_accessed_ns = now;
        }
        MemorySearchResult result;
        result.record = record;
        result.score = match.score;
        result.similarity = match.similarity;
        result.record.access_count = record.access_count;
        result.record.last_accessed_ns = record.last_accessed_ns;
        out.push_back(std::move(result));
    }
    return out;
}

bool AgentMemoryStore::should_use_ann_locked_(const MemoryQuery& query) const {
    if (ann_config_.mode == AgentMemoryAnnMode::Off) return false;
    if (query.force_scan) return false;
    if (query.query_embedding.empty()) return false;
    if (query.tenant_id.empty() || query.namespace_id.empty()) return false;
    if (ann_config_.mode == AgentMemoryAnnMode::Auto &&
        memories_.size() < ann_config_.min_records) {
        return false;
    }
    return true;
}

AnnIndexKind AgentMemoryStore::ann_index_kind_locked_() const {
    return ann_config_.mode == AgentMemoryAnnMode::Hnsw
        ? AnnIndexKind::Hnsw
        : AnnIndexKind::SparseProjection;
}

std::optional<AgentMemoryStore::AnnBuildSnapshot>
AgentMemoryStore::ann_build_snapshot_locked_() const {
    if (ann_config_.mode == AgentMemoryAnnMode::Off) return std::nullopt;
    if (!ann_dirty_ && ann_index_) return std::nullopt;
    AnnBuildSnapshot snapshot;
    snapshot.kind = ann_index_kind_locked_();
    snapshot.config = ann_config_.index;
    snapshot.generation = ann_generation_;
    snapshot.entries.reserve(memories_.size());
    for (size_t i = 0; i < memories_.size(); ++i) {
        const auto& record = memories_[i];
        if (record.embedding.empty()) continue;
        AnnBuildEntry entry;
        entry.partition_key =
            ann_partition_key_(record.tenant_id, record.namespace_id);
        entry.row_id = i;
        entry.embedding = record.embedding;
        snapshot.entries.push_back(std::move(entry));
    }
    return snapshot;
}

StoreResult AgentMemoryStore::build_and_swap_ann_index_(
    AgentMemoryStore::AnnBuildSnapshot snapshot) {
    auto next_index = make_ann_index(snapshot.kind, snapshot.config);
    if (!next_index) {
        if (snapshot.kind == AnnIndexKind::Hnsw) {
            return {false, {}, "HNSW ANN backend is not enabled in this build"};
        }
        return {false, {}, "failed to create ANN index"};
    }

    const auto start = std::chrono::steady_clock::now();
    for (const auto& entry : snapshot.entries) {
        next_index->add(entry.partition_key, entry.row_id, entry.embedding);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    std::lock_guard<std::mutex> lock(mu_);
    if (snapshot.generation != ann_generation_ ||
        snapshot.kind != ann_index_kind_locked_()) {
        return {false, {}, "ANN index rebuild was superseded by a newer mutation"};
    }
    ann_index_ = std::move(next_index);
    ann_dirty_ = false;
    ann_last_rebuild_ms_ = elapsed;
    ++ann_rebuild_count_;
    return {true, {}, {}};
}

bool AgentMemoryStore::try_add_ann_entry_locked_(
    size_t row_id,
    const MemoryRecord& record) {
    if (!ann_index_ || ann_dirty_) return false;
    if (row_id >= memories_.size()) return false;
    if (record.embedding.empty()) return true;
    return ann_index_->add(
        ann_partition_key_(record.tenant_id, record.namespace_id),
        row_id,
        record.embedding);
}

void AgentMemoryStore::mark_ann_dirty_locked_() {
    ann_dirty_ = true;
    ++ann_generation_;
    if (ann_rebuild_running_) {
        request_ann_rebuild_locked_();
    }
}

std::string AgentMemoryStore::ann_partition_key_(
    const std::string& tenant_id,
    const std::string& namespace_id) const {
    return tenant_id + "\n" + namespace_id;
}

double AgentMemoryStore::memory_retention_score_(const MemoryRecord& record,
                                                 int64_t now) const {
    double score = record.importance * 100.0;
    score += std::min<double>(50.0, static_cast<double>(record.access_count));
    const int64_t last_seen = record.last_accessed_ns > 0
        ? record.last_accessed_ns
        : record.created_at_ns;
    if (last_seen > 0 && now > last_seen) {
        const double age_hours =
            static_cast<double>(now - last_seen) / 3'600'000'000'000.0;
        score += 1.0 / (1.0 + std::max(0.0, age_hours));
    } else {
        score += 1.0;
    }
    if (record.pinned) score += 1'000'000.0;
    return score;
}

double AgentMemoryStore::cache_retention_score_(const CacheEntry& entry,
                                                int64_t now) const {
    double score = std::min<double>(50.0, static_cast<double>(entry.access_count));
    const int64_t last_seen = entry.last_accessed_ns > 0
        ? entry.last_accessed_ns
        : entry.created_at_ns;
    if (last_seen > 0 && now > last_seen) {
        const double age_hours =
            static_cast<double>(now - last_seen) / 3'600'000'000'000.0;
        score += 1.0 / (1.0 + std::max(0.0, age_hours));
    } else {
        score += 1.0;
    }
    return score;
}

size_t AgentMemoryStore::evict_expired_locked_(int64_t now) {
    size_t memory_removed = 0;
    if (expiring_memory_count_ > 0) {
        const size_t memory_before = memories_.size();
        memories_.erase(std::remove_if(memories_.begin(), memories_.end(),
            [&](const MemoryRecord& record) {
                return expired_(record.expires_at_ns, now);
            }), memories_.end());
        memory_removed = memory_before - memories_.size();
    }
    if (memory_removed > 0) {
        evicted_memory_count_ += memory_removed;
        rebuild_memory_index_locked_();
        rebuild_unit_embeddings_locked_();
    }

    size_t cache_removed = 0;
    if (expiring_cache_count_ > 0) {
        const size_t cache_before = cache_entries_.size();
        cache_entries_.erase(std::remove_if(cache_entries_.begin(), cache_entries_.end(),
            [&](const CacheEntry& entry) {
                return expired_(entry.expires_at_ns, now);
            }), cache_entries_.end());
        cache_removed = cache_before - cache_entries_.size();
    }
    if (cache_removed > 0) {
        evicted_cache_count_ += cache_removed;
        rebuild_cache_index_locked_();
    }
    if (memory_removed > 0 || cache_removed > 0) {
        rebuild_expiry_counts_locked_();
    }
    return memory_removed + cache_removed;
}

void AgentMemoryStore::enforce_eviction_locked_(int64_t now) {
    while (eviction_config_.max_memories > 0 &&
           memories_.size() > eviction_config_.max_memories) {
        if (!evict_one_memory_for_capacity_locked_(now)) break;
    }
    while (eviction_config_.max_cache_entries > 0 &&
           cache_entries_.size() > eviction_config_.max_cache_entries) {
        if (!evict_one_cache_for_capacity_locked_(now)) break;
    }
}

bool AgentMemoryStore::evict_one_memory_for_capacity_locked_(int64_t now) {
    if (memories_.empty()) return false;
    auto candidate = memories_.end();
    double candidate_score = std::numeric_limits<double>::infinity();
    for (auto it = memories_.begin(); it != memories_.end(); ++it) {
        if (eviction_config_.protect_pinned && it->pinned) continue;
        const double score = memory_retention_score_(*it, now);
        if (candidate == memories_.end() || score < candidate_score ||
            (score == candidate_score && it->created_at_ns < candidate->created_at_ns)) {
            candidate = it;
            candidate_score = score;
        }
    }
    if (candidate == memories_.end()) return false;
    if (has_expiry(candidate->expires_at_ns) && expiring_memory_count_ > 0) {
        --expiring_memory_count_;
    }
    memories_.erase(candidate);
    ++evicted_memory_count_;
    rebuild_memory_index_locked_();
    rebuild_unit_embeddings_locked_();
    return true;
}

bool AgentMemoryStore::evict_one_cache_for_capacity_locked_(int64_t now) {
    if (cache_entries_.empty()) return false;
    auto candidate = cache_entries_.end();
    double candidate_score = std::numeric_limits<double>::infinity();
    for (auto it = cache_entries_.begin(); it != cache_entries_.end(); ++it) {
        const double score = cache_retention_score_(*it, now);
        if (candidate == cache_entries_.end() || score < candidate_score ||
            (score == candidate_score && it->created_at_ns < candidate->created_at_ns)) {
            candidate = it;
            candidate_score = score;
        }
    }
    if (has_expiry(candidate->expires_at_ns) && expiring_cache_count_ > 0) {
        --expiring_cache_count_;
    }
    cache_entries_.erase(candidate);
    ++evicted_cache_count_;
    rebuild_cache_index_locked_();
    return true;
}

void AgentMemoryStore::rebuild_expiry_counts_locked_() {
    expiring_memory_count_ = 0;
    for (const auto& record : memories_) {
        if (has_expiry(record.expires_at_ns)) ++expiring_memory_count_;
    }
    expiring_cache_count_ = 0;
    for (const auto& entry : cache_entries_) {
        if (has_expiry(entry.expires_at_ns)) ++expiring_cache_count_;
    }
}

void AgentMemoryStore::rebuild_memory_index_locked_() {
    memory_index_.clear();
    for (size_t i = 0; i < memories_.size(); ++i) {
        memory_index_[memories_[i].memory_id] = i;
    }
    mark_ann_dirty_locked_();
}

void AgentMemoryStore::rebuild_cache_index_locked_() {
    cache_exact_index_.clear();
    for (size_t i = 0; i < cache_entries_.size(); ++i) {
        const auto& entry = cache_entries_[i];
        const std::string exact_key = entry.tenant_id + "\n" + entry.namespace_id + "\n"
            + normalize_prompt(entry.prompt);
        cache_exact_index_[exact_key] = i;
    }
}

std::string AgentMemoryStore::next_id_(const char* prefix) {
    const bool cache_id = std::string(prefix) == "cache";
    const uint64_t counter = cache_id ? next_cache_id_++ : next_memory_id_++;
    std::ostringstream os;
    if (id_config_.owner_scoped) {
        os << prefix << "_" << id_config_.node_id << "_"
           << id_config_.ring_epoch << "_" << counter;
    } else {
        os << prefix << "_" << counter;
    }
    return os.str();
}

} // namespace zeptodb::ai
