#pragma once
// ============================================================================
// ZeptoDB: Agent Memory RPC Wire Helpers
// ============================================================================
// Compact little-endian binary serializers for Agent Memory inter-node RPC.
// These helpers are intentionally independent of the cluster transport so the
// AI layer owns its payload format while TcpRpc only moves opaque bytes.
// ============================================================================

#include "zeptodb/ai/agent_memory.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zeptodb::ai {

struct MemoryGetRequest {
    std::string memory_id;
    std::string tenant_id;
};

struct MemoryDeleteRequest {
    std::string memory_id;
    std::string tenant_id;
};

struct CacheDeleteRequest {
    std::string tenant_id;
    std::string namespace_id = "default";
    std::string prompt;
};

struct MemoryGetResult {
    bool found = false;
    MemoryRecord record;
    std::string error;
};

struct MemorySearchRpcResult {
    std::vector<MemorySearchResult> matches;
    std::string error;
};

std::vector<uint8_t> serialize_memory_record(const MemoryRecord& record);
bool deserialize_memory_record(const uint8_t* data,
                               size_t len,
                               MemoryRecord* record,
                               std::string* error = nullptr);

std::vector<uint8_t> serialize_memory_query(const MemoryQuery& query);
bool deserialize_memory_query(const uint8_t* data,
                              size_t len,
                              MemoryQuery* query,
                              std::string* error = nullptr);

std::vector<uint8_t> serialize_memory_search_result(
    const MemorySearchRpcResult& result);
bool deserialize_memory_search_result(const uint8_t* data,
                                      size_t len,
                                      MemorySearchRpcResult* result,
                                      std::string* error = nullptr);

std::vector<uint8_t> serialize_memory_get_request(const MemoryGetRequest& request);
bool deserialize_memory_get_request(const uint8_t* data,
                                    size_t len,
                                    MemoryGetRequest* request,
                                    std::string* error = nullptr);

std::vector<uint8_t> serialize_memory_get_result(const MemoryGetResult& result);
bool deserialize_memory_get_result(const uint8_t* data,
                                   size_t len,
                                   MemoryGetResult* result,
                                   std::string* error = nullptr);

std::vector<uint8_t> serialize_memory_delete_request(
    const MemoryDeleteRequest& request);
bool deserialize_memory_delete_request(const uint8_t* data,
                                       size_t len,
                                       MemoryDeleteRequest* request,
                                       std::string* error = nullptr);

std::vector<uint8_t> serialize_cache_delete_request(
    const CacheDeleteRequest& request);
bool deserialize_cache_delete_request(const uint8_t* data,
                                      size_t len,
                                      CacheDeleteRequest* request,
                                      std::string* error = nullptr);

std::vector<uint8_t> serialize_cache_entry(const CacheEntry& entry);
bool deserialize_cache_entry(const uint8_t* data,
                             size_t len,
                             CacheEntry* entry,
                             std::string* error = nullptr);

std::vector<uint8_t> serialize_cache_lookup(const CacheLookup& lookup);
bool deserialize_cache_lookup(const uint8_t* data,
                              size_t len,
                              CacheLookup* lookup,
                              std::string* error = nullptr);

std::vector<uint8_t> serialize_cache_lookup_result(const CacheLookupResult& result);
bool deserialize_cache_lookup_result(const uint8_t* data,
                                     size_t len,
                                     CacheLookupResult* result,
                                     std::string* error = nullptr);

std::vector<uint8_t> serialize_store_result(const StoreResult& result);
bool deserialize_store_result(const uint8_t* data,
                              size_t len,
                              StoreResult* result,
                              std::string* error = nullptr);

std::vector<uint8_t> serialize_agent_memory_stats(
    const AgentMemoryStats& stats);
bool deserialize_agent_memory_stats(const uint8_t* data,
                                    size_t len,
                                    AgentMemoryStats* stats,
                                    std::string* error = nullptr);

} // namespace zeptodb::ai
