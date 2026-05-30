#include "zeptodb/ai/agent_memory_wire.h"

#include <cstring>
#include <limits>
#include <utility>

namespace zeptodb::ai {

namespace {

constexpr uint8_t kEmptyPayload[1] = {0};

void write_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void write_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void write_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void write_i64(std::vector<uint8_t>& out, int64_t value) {
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    write_u64(out, raw);
}

void write_double(std::vector<uint8_t>& out, double value) {
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    write_u64(out, raw);
}

void write_f32(std::vector<uint8_t>& out, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    write_u32(out, raw);
}

void write_string(std::vector<uint8_t>& out, const std::string& value) {
    write_u32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void write_embedding(std::vector<uint8_t>& out,
                     const std::vector<float>& embedding) {
    write_u32(out, static_cast<uint32_t>(embedding.size()));
    for (float value : embedding) {
        write_f32(out, value);
    }
}

class Reader {
public:
    Reader(const uint8_t* data, size_t len)
        : p_(data ? data : kEmptyPayload),
          end_(p_ + len) {}

    bool read_u8(uint8_t* value) {
        if (!need_(1)) return false;
        *value = *p_++;
        return true;
    }

    bool read_u32(uint32_t* value) {
        if (!need_(4)) return false;
        *value = static_cast<uint32_t>(p_[0])
            | (static_cast<uint32_t>(p_[1]) << 8)
            | (static_cast<uint32_t>(p_[2]) << 16)
            | (static_cast<uint32_t>(p_[3]) << 24);
        p_ += 4;
        return true;
    }

    bool read_u64(uint64_t* value) {
        if (!need_(8)) return false;
        uint64_t out = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            out |= static_cast<uint64_t>(*p_++) << shift;
        }
        *value = out;
        return true;
    }

    bool read_i64(int64_t* value) {
        uint64_t raw = 0;
        if (!read_u64(&raw)) return false;
        std::memcpy(value, &raw, sizeof(raw));
        return true;
    }

    bool read_double(double* value) {
        uint64_t raw = 0;
        if (!read_u64(&raw)) return false;
        std::memcpy(value, &raw, sizeof(raw));
        return true;
    }

    bool read_f32(float* value) {
        uint32_t raw = 0;
        if (!read_u32(&raw)) return false;
        std::memcpy(value, &raw, sizeof(raw));
        return true;
    }

    bool read_string(std::string* value) {
        uint32_t size = 0;
        if (!read_u32(&size) || !need_(size)) return false;
        value->assign(reinterpret_cast<const char*>(p_), size);
        p_ += size;
        return true;
    }

    bool read_embedding(std::vector<float>* value) {
        uint32_t size = 0;
        if (!read_u32(&size)) return false;
        if (size > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            !need_(static_cast<size_t>(size) * sizeof(float))) {
            return false;
        }
        value->resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            if (!read_f32(&(*value)[i])) return false;
        }
        return true;
    }

    bool finished() const { return p_ == end_; }

private:
    bool need_(size_t n) const {
        return static_cast<size_t>(end_ - p_) >= n;
    }

    const uint8_t* p_;
    const uint8_t* end_;
};

bool finish_or_error(const Reader& reader,
                     const char* message,
                     std::string* error) {
    if (reader.finished()) return true;
    if (error) *error = message;
    return false;
}

} // namespace

std::vector<uint8_t> serialize_memory_record(const MemoryRecord& record) {
    std::vector<uint8_t> out;
    out.reserve(128 + record.content.size() + record.metadata_json.size() +
                record.embedding.size() * sizeof(float));
    write_string(out, record.memory_id);
    write_string(out, record.tenant_id);
    write_string(out, record.namespace_id);
    write_string(out, record.user_id);
    write_string(out, record.session_id);
    write_string(out, record.agent_id);
    write_string(out, record.type);
    write_string(out, record.content);
    write_string(out, record.metadata_json);
    write_embedding(out, record.embedding);
    write_i64(out, record.token_count);
    write_double(out, record.importance);
    write_i64(out, record.created_at_ns);
    write_i64(out, record.last_accessed_ns);
    write_i64(out, record.expires_at_ns);
    write_u8(out, record.pinned ? 1u : 0u);
    write_u64(out, record.access_count);
    return out;
}

bool deserialize_memory_record(const uint8_t* data,
                               size_t len,
                               MemoryRecord* record,
                               std::string* error) {
    if (!record) {
        if (error) *error = "memory record output is null";
        return false;
    }
    MemoryRecord out;
    Reader reader(data, len);
    uint8_t pinned = 0;
    if (!reader.read_string(&out.memory_id) ||
        !reader.read_string(&out.tenant_id) ||
        !reader.read_string(&out.namespace_id) ||
        !reader.read_string(&out.user_id) ||
        !reader.read_string(&out.session_id) ||
        !reader.read_string(&out.agent_id) ||
        !reader.read_string(&out.type) ||
        !reader.read_string(&out.content) ||
        !reader.read_string(&out.metadata_json) ||
        !reader.read_embedding(&out.embedding) ||
        !reader.read_i64(&out.token_count) ||
        !reader.read_double(&out.importance) ||
        !reader.read_i64(&out.created_at_ns) ||
        !reader.read_i64(&out.last_accessed_ns) ||
        !reader.read_i64(&out.expires_at_ns) ||
        !reader.read_u8(&pinned) ||
        !reader.read_u64(&out.access_count)) {
        if (error) *error = "invalid memory record payload";
        return false;
    }
    out.pinned = pinned != 0;
    if (!finish_or_error(reader, "trailing memory record payload bytes", error)) {
        return false;
    }
    *record = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_memory_query(const MemoryQuery& query) {
    std::vector<uint8_t> out;
    out.reserve(96 + query.query_embedding.size() * sizeof(float));
    write_string(out, query.tenant_id);
    write_string(out, query.namespace_id);
    write_string(out, query.user_id);
    write_string(out, query.session_id);
    write_string(out, query.agent_id);
    write_string(out, query.type);
    write_embedding(out, query.query_embedding);
    write_u64(out, static_cast<uint64_t>(query.limit));
    write_i64(out, query.now_ns);
    write_u8(out, query.include_expired ? 1u : 0u);
    write_u8(out, query.force_scan ? 1u : 0u);
    write_u8(out, query.update_access ? 1u : 0u);
    return out;
}

bool deserialize_memory_query(const uint8_t* data,
                              size_t len,
                              MemoryQuery* query,
                              std::string* error) {
    if (!query) {
        if (error) *error = "memory query output is null";
        return false;
    }
    MemoryQuery out;
    Reader reader(data, len);
    uint64_t limit = 0;
    uint8_t include_expired = 0;
    uint8_t force_scan = 0;
    uint8_t update_access = 0;
    if (!reader.read_string(&out.tenant_id) ||
        !reader.read_string(&out.namespace_id) ||
        !reader.read_string(&out.user_id) ||
        !reader.read_string(&out.session_id) ||
        !reader.read_string(&out.agent_id) ||
        !reader.read_string(&out.type) ||
        !reader.read_embedding(&out.query_embedding) ||
        !reader.read_u64(&limit) ||
        !reader.read_i64(&out.now_ns) ||
        !reader.read_u8(&include_expired) ||
        !reader.read_u8(&force_scan) ||
        !reader.read_u8(&update_access)) {
        if (error) *error = "invalid memory query payload";
        return false;
    }
    if (limit > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        if (error) *error = "memory query limit is too large";
        return false;
    }
    out.limit = static_cast<size_t>(limit);
    out.include_expired = include_expired != 0;
    out.force_scan = force_scan != 0;
    out.update_access = update_access != 0;
    if (!finish_or_error(reader, "trailing memory query payload bytes", error)) {
        return false;
    }
    *query = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_memory_search_result(
    const MemorySearchRpcResult& result) {
    std::vector<uint8_t> out;
    write_string(out, result.error);
    write_u32(out, static_cast<uint32_t>(result.matches.size()));
    for (const auto& match : result.matches) {
        const auto record = serialize_memory_record(match.record);
        write_u32(out, static_cast<uint32_t>(record.size()));
        out.insert(out.end(), record.begin(), record.end());
        write_double(out, match.score);
        write_double(out, match.similarity);
    }
    return out;
}

bool deserialize_memory_search_result(const uint8_t* data,
                                      size_t len,
                                      MemorySearchRpcResult* result,
                                      std::string* error) {
    if (!result) {
        if (error) *error = "memory search result output is null";
        return false;
    }
    MemorySearchRpcResult out;
    Reader reader(data, len);
    uint32_t count = 0;
    if (!reader.read_string(&out.error) || !reader.read_u32(&count)) {
        if (error) *error = "invalid memory search result payload";
        return false;
    }
    out.matches.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t record_size = 0;
        if (!reader.read_u32(&record_size)) {
            if (error) *error = "invalid memory search result record size";
            return false;
        }
        std::vector<uint8_t> record_payload(record_size);
        for (uint32_t j = 0; j < record_size; ++j) {
            if (!reader.read_u8(&record_payload[j])) {
                if (error) *error = "truncated memory search result record";
                return false;
            }
        }
        MemorySearchResult match;
        if (!deserialize_memory_record(record_payload.data(),
                                       record_payload.size(),
                                       &match.record,
                                       error) ||
            !reader.read_double(&match.score) ||
            !reader.read_double(&match.similarity)) {
            if (error && error->empty()) {
                *error = "invalid memory search result match";
            }
            return false;
        }
        out.matches.push_back(std::move(match));
    }
    if (!finish_or_error(reader, "trailing memory search result payload bytes", error)) {
        return false;
    }
    *result = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_memory_get_request(
    const MemoryGetRequest& request) {
    std::vector<uint8_t> out;
    out.reserve(16 + request.memory_id.size() + request.tenant_id.size());
    write_string(out, request.memory_id);
    write_string(out, request.tenant_id);
    return out;
}

bool deserialize_memory_get_request(const uint8_t* data,
                                    size_t len,
                                    MemoryGetRequest* request,
                                    std::string* error) {
    if (!request) {
        if (error) *error = "memory get request output is null";
        return false;
    }
    MemoryGetRequest out;
    Reader reader(data, len);
    if (!reader.read_string(&out.memory_id) ||
        !reader.read_string(&out.tenant_id)) {
        if (error) *error = "invalid memory get request payload";
        return false;
    }
    if (!finish_or_error(reader, "trailing memory get request payload bytes", error)) {
        return false;
    }
    *request = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_memory_get_result(
    const MemoryGetResult& result) {
    std::vector<uint8_t> out;
    write_u8(out, result.found ? 1u : 0u);
    write_string(out, result.error);
    const auto record = serialize_memory_record(result.record);
    write_u32(out, static_cast<uint32_t>(record.size()));
    out.insert(out.end(), record.begin(), record.end());
    return out;
}

bool deserialize_memory_get_result(const uint8_t* data,
                                   size_t len,
                                   MemoryGetResult* result,
                                   std::string* error) {
    if (!result) {
        if (error) *error = "memory get result output is null";
        return false;
    }
    MemoryGetResult out;
    Reader reader(data, len);
    uint8_t found = 0;
    uint32_t record_size = 0;
    if (!reader.read_u8(&found) ||
        !reader.read_string(&out.error) ||
        !reader.read_u32(&record_size)) {
        if (error) *error = "invalid memory get result payload";
        return false;
    }
    out.found = found != 0;
    std::vector<uint8_t> record_payload(record_size);
    for (uint32_t i = 0; i < record_size; ++i) {
        if (!reader.read_u8(&record_payload[i])) {
            if (error) *error = "truncated memory get result record";
            return false;
        }
    }
    if (out.found &&
        !deserialize_memory_record(record_payload.data(), record_payload.size(),
                                   &out.record, error)) {
        return false;
    }
    if (!finish_or_error(reader, "trailing memory get result payload bytes", error)) {
        return false;
    }
    *result = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_memory_delete_request(
    const MemoryDeleteRequest& request) {
    std::vector<uint8_t> out;
    out.reserve(16 + request.memory_id.size() + request.tenant_id.size());
    write_string(out, request.memory_id);
    write_string(out, request.tenant_id);
    return out;
}

bool deserialize_memory_delete_request(const uint8_t* data,
                                       size_t len,
                                       MemoryDeleteRequest* request,
                                       std::string* error) {
    if (!request) {
        if (error) *error = "memory delete request output is null";
        return false;
    }
    MemoryDeleteRequest out;
    Reader reader(data, len);
    if (!reader.read_string(&out.memory_id) ||
        !reader.read_string(&out.tenant_id)) {
        if (error) *error = "invalid memory delete request payload";
        return false;
    }
    if (!finish_or_error(reader, "trailing memory delete request payload bytes",
                         error)) {
        return false;
    }
    *request = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_cache_delete_request(
    const CacheDeleteRequest& request) {
    std::vector<uint8_t> out;
    out.reserve(24 + request.tenant_id.size() + request.namespace_id.size() +
                request.prompt.size());
    write_string(out, request.tenant_id);
    write_string(out, request.namespace_id);
    write_string(out, request.prompt);
    return out;
}

bool deserialize_cache_delete_request(const uint8_t* data,
                                      size_t len,
                                      CacheDeleteRequest* request,
                                      std::string* error) {
    if (!request) {
        if (error) *error = "cache delete request output is null";
        return false;
    }
    CacheDeleteRequest out;
    Reader reader(data, len);
    if (!reader.read_string(&out.tenant_id) ||
        !reader.read_string(&out.namespace_id) ||
        !reader.read_string(&out.prompt)) {
        if (error) *error = "invalid cache delete request payload";
        return false;
    }
    if (!finish_or_error(reader, "trailing cache delete request payload bytes",
                         error)) {
        return false;
    }
    if (out.namespace_id.empty()) out.namespace_id = "default";
    *request = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_cache_entry(const CacheEntry& entry) {
    std::vector<uint8_t> out;
    out.reserve(96 + entry.prompt.size() + entry.response.size() +
                entry.metadata_json.size() +
                entry.embedding.size() * sizeof(float));
    write_string(out, entry.cache_id);
    write_string(out, entry.tenant_id);
    write_string(out, entry.namespace_id);
    write_string(out, entry.prompt);
    write_string(out, entry.response);
    write_string(out, entry.metadata_json);
    write_embedding(out, entry.embedding);
    write_i64(out, entry.token_count);
    write_i64(out, entry.created_at_ns);
    write_i64(out, entry.last_accessed_ns);
    write_i64(out, entry.expires_at_ns);
    write_u64(out, entry.access_count);
    return out;
}

bool deserialize_cache_entry(const uint8_t* data,
                             size_t len,
                             CacheEntry* entry,
                             std::string* error) {
    if (!entry) {
        if (error) *error = "cache entry output is null";
        return false;
    }
    CacheEntry out;
    Reader reader(data, len);
    if (!reader.read_string(&out.cache_id) ||
        !reader.read_string(&out.tenant_id) ||
        !reader.read_string(&out.namespace_id) ||
        !reader.read_string(&out.prompt) ||
        !reader.read_string(&out.response) ||
        !reader.read_string(&out.metadata_json) ||
        !reader.read_embedding(&out.embedding) ||
        !reader.read_i64(&out.token_count) ||
        !reader.read_i64(&out.created_at_ns) ||
        !reader.read_i64(&out.last_accessed_ns) ||
        !reader.read_i64(&out.expires_at_ns) ||
        !reader.read_u64(&out.access_count)) {
        if (error) *error = "invalid cache entry payload";
        return false;
    }
    if (!finish_or_error(reader, "trailing cache entry payload bytes", error)) {
        return false;
    }
    *entry = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_cache_lookup(const CacheLookup& lookup) {
    std::vector<uint8_t> out;
    out.reserve(64 + lookup.prompt.size() +
                lookup.embedding.size() * sizeof(float));
    write_string(out, lookup.tenant_id);
    write_string(out, lookup.namespace_id);
    write_string(out, lookup.prompt);
    write_embedding(out, lookup.embedding);
    write_double(out, lookup.semantic_threshold);
    write_i64(out, lookup.now_ns);
    return out;
}

bool deserialize_cache_lookup(const uint8_t* data,
                              size_t len,
                              CacheLookup* lookup,
                              std::string* error) {
    if (!lookup) {
        if (error) *error = "cache lookup output is null";
        return false;
    }
    CacheLookup out;
    Reader reader(data, len);
    if (!reader.read_string(&out.tenant_id) ||
        !reader.read_string(&out.namespace_id) ||
        !reader.read_string(&out.prompt) ||
        !reader.read_embedding(&out.embedding) ||
        !reader.read_double(&out.semantic_threshold) ||
        !reader.read_i64(&out.now_ns)) {
        if (error) *error = "invalid cache lookup payload";
        return false;
    }
    if (!finish_or_error(reader, "trailing cache lookup payload bytes", error)) {
        return false;
    }
    *lookup = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_cache_lookup_result(
    const CacheLookupResult& result) {
    std::vector<uint8_t> out;
    write_u8(out, result.hit ? 1u : 0u);
    write_u8(out, result.exact ? 1u : 0u);
    write_double(out, result.score);
    const auto entry = serialize_cache_entry(result.entry);
    write_u32(out, static_cast<uint32_t>(entry.size()));
    out.insert(out.end(), entry.begin(), entry.end());
    return out;
}

bool deserialize_cache_lookup_result(const uint8_t* data,
                                     size_t len,
                                     CacheLookupResult* result,
                                     std::string* error) {
    if (!result) {
        if (error) *error = "cache lookup result output is null";
        return false;
    }
    CacheLookupResult out;
    Reader reader(data, len);
    uint8_t hit = 0;
    uint8_t exact = 0;
    uint32_t entry_size = 0;
    if (!reader.read_u8(&hit) ||
        !reader.read_u8(&exact) ||
        !reader.read_double(&out.score) ||
        !reader.read_u32(&entry_size)) {
        if (error) *error = "invalid cache lookup result payload";
        return false;
    }
    out.hit = hit != 0;
    out.exact = exact != 0;
    std::vector<uint8_t> entry_payload(entry_size);
    for (uint32_t i = 0; i < entry_size; ++i) {
        if (!reader.read_u8(&entry_payload[i])) {
            if (error) *error = "truncated cache lookup result entry";
            return false;
        }
    }
    if (out.hit &&
        !deserialize_cache_entry(entry_payload.data(), entry_payload.size(),
                                 &out.entry, error)) {
        return false;
    }
    if (!finish_or_error(reader, "trailing cache lookup result payload bytes", error)) {
        return false;
    }
    *result = std::move(out);
    return true;
}

std::vector<uint8_t> serialize_store_result(const StoreResult& result) {
    std::vector<uint8_t> out;
    out.reserve(16 + result.id.size() + result.error.size());
    write_u8(out, result.ok ? 1u : 0u);
    write_string(out, result.id);
    write_string(out, result.error);
    return out;
}

bool deserialize_store_result(const uint8_t* data,
                              size_t len,
                              StoreResult* result,
                              std::string* error) {
    if (!result) {
        if (error) *error = "store result output is null";
        return false;
    }
    StoreResult out;
    Reader reader(data, len);
    uint8_t ok = 0;
    if (!reader.read_u8(&ok) ||
        !reader.read_string(&out.id) ||
        !reader.read_string(&out.error)) {
        if (error) *error = "invalid store result payload";
        return false;
    }
    out.ok = ok != 0;
    if (!finish_or_error(reader, "trailing store result payload bytes", error)) {
        return false;
    }
    *result = std::move(out);
    return true;
}

} // namespace zeptodb::ai
