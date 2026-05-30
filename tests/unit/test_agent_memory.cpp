// ============================================================================
// ZeptoDB: Agent Memory Layer tests
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "test_port_helper.h"
#include "zeptodb/ai/agent_memory.h"
#include "zeptodb/ai/agent_memory_router.h"
#include "zeptodb/ai/agent_memory_wire.h"
#include "zeptodb/cluster/k8s_lease.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"

using zeptodb::ai::AgentMemoryStore;
using zeptodb::ai::AgentMemoryAnnConfig;
using zeptodb::ai::AgentMemoryAnnMode;
using zeptodb::ai::AgentMemoryEntryKind;
using zeptodb::ai::AgentMemoryEvictionConfig;
using zeptodb::ai::AgentMemoryIdConfig;
using zeptodb::ai::AgentMemoryNodeId;
using zeptodb::ai::AgentMemoryRouter;
using zeptodb::ai::AgentMemoryRouterConfig;
using zeptodb::ai::AgentMemoryRoutingMode;
using zeptodb::ai::CacheEntry;
using zeptodb::ai::CacheLookup;
using zeptodb::ai::CacheLookupResult;
using zeptodb::ai::ContextRequest;
using zeptodb::ai::MemoryQuery;
using zeptodb::ai::MemoryRecord;
using zeptodb::ai::MemorySearchRpcResult;
using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::server::AgentMemoryReplicationMode;
using zeptodb::sql::QueryExecutor;
using namespace std::chrono_literals;

namespace {

struct HttpResp {
    int status = 0;
    std::string body;
};

HttpResp http_post(uint16_t port, const std::string& path,
                   const std::string& body,
                   const std::string& tenant = {}) {
    HttpResp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return r;
    }

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n";
    if (!tenant.empty()) req += "X-Zepto-Tenant-Id: " + tenant + "\r\n";
    req += "\r\n" + body;
    ::send(fd, req.data(), req.size(), 0);

    std::string raw;
    char buf[8192];
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    if (raw.size() > 12) r.status = std::atoi(raw.c_str() + 9);
    const auto pos = raw.find("\r\n\r\n");
    r.body = pos == std::string::npos ? raw : raw.substr(pos + 4);
    return r;
}

HttpResp http_get(uint16_t port, const std::string& path) {
    HttpResp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return r;
    }

    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string raw;
    char buf[8192];
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    if (raw.size() > 12) r.status = std::atoi(raw.c_str() + 9);
    const auto pos = raw.find("\r\n\r\n");
    r.body = pos == std::string::npos ? raw : raw.substr(pos + 4);
    return r;
}

HttpResp http_delete(uint16_t port, const std::string& path,
                     const std::string& tenant = {}) {
    HttpResp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return r;
    }

    std::string req = "DELETE " + path + " HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n";
    if (!tenant.empty()) req += "X-Zepto-Tenant-Id: " + tenant + "\r\n";
    req += "\r\n";
    ::send(fd, req.data(), req.size(), 0);

    std::string raw;
    char buf[8192];
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    if (raw.size() > 12) r.status = std::atoi(raw.c_str() + 9);
    const auto pos = raw.find("\r\n\r\n");
    r.body = pos == std::string::npos ? raw : raw.substr(pos + 4);
    return r;
}

std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

} // namespace

TEST(AgentMemoryStoreTest, PutSearchAndContextBudget) {
    AgentMemoryStore store;

    MemoryRecord pinned;
    pinned.memory_id = "m1";
    pinned.tenant_id = "tenant_a";
    pinned.namespace_id = "agent";
    pinned.user_id = "u1";
    pinned.content = "User prefers concise answers.";
    pinned.embedding = {1.0f, 0.0f};
    pinned.token_count = 5;
    pinned.pinned = true;
    ASSERT_TRUE(store.put_memory(pinned).ok);

    MemoryRecord large;
    large.memory_id = "m2";
    large.tenant_id = "tenant_a";
    large.namespace_id = "agent";
    large.user_id = "u1";
    large.content = "Very large tool output";
    large.embedding = {0.9f, 0.1f};
    large.token_count = 100;
    large.importance = 3.0;
    ASSERT_TRUE(store.put_memory(large).ok);

    ContextRequest req;
    req.tenant_id = "tenant_a";
    req.namespace_id = "agent";
    req.user_id = "u1";
    req.query_embedding = {1.0f, 0.0f};
    req.limit = 10;
    req.token_budget = 10;

    auto context = store.get_context(req);
    ASSERT_EQ(context.memories.size(), 1u);
    EXPECT_EQ(context.memories[0].record.memory_id, "m1");
    EXPECT_LE(context.token_count, 10);
}

TEST(AgentMemoryStoreTest, RejectsInvalidEmbeddingDimensionAndNan) {
    AgentMemoryStore store;
    MemoryRecord r;
    r.memory_id = "m1";
    r.embedding = {1.0f, 0.0f};
    ASSERT_TRUE(store.put_memory(r).ok);

    MemoryQuery wrong_query_dim;
    wrong_query_dim.query_embedding = {1.0f, 0.0f, 0.0f};
    wrong_query_dim.limit = 1;
    auto wrong_dim_matches = store.search(wrong_query_dim);
    ASSERT_EQ(wrong_dim_matches.size(), 1u);
    EXPECT_DOUBLE_EQ(wrong_dim_matches[0].similarity, 0.0);

    MemoryRecord wrong_dim;
    wrong_dim.memory_id = "m2";
    wrong_dim.embedding = {1.0f, 0.0f, 0.0f};
    auto bad_dim = store.put_memory(wrong_dim);
    EXPECT_FALSE(bad_dim.ok);
    EXPECT_NE(bad_dim.error.find("dimension mismatch"), std::string::npos);

    AgentMemoryStore store2;
    MemoryRecord nan_record;
    nan_record.memory_id = "bad";
    nan_record.embedding = {std::nanf("")};
    auto bad_nan = store2.put_memory(nan_record);
    EXPECT_FALSE(bad_nan.ok);
    EXPECT_NE(bad_nan.error.find("NaN"), std::string::npos);
}

TEST(AgentMemoryStoreTest, TtlAndTenantIsolation) {
    AgentMemoryStore store;
    const int64_t now = AgentMemoryStore::now_ns();

    MemoryRecord expired;
    expired.memory_id = "old";
    expired.tenant_id = "tenant_a";
    expired.namespace_id = "agent";
    expired.content = "expired";
    expired.embedding = {1.0f, 0.0f};
    expired.expires_at_ns = now - 1;
    ASSERT_TRUE(store.put_memory(expired).ok);

    MemoryRecord live;
    live.memory_id = "live";
    live.tenant_id = "tenant_b";
    live.namespace_id = "agent";
    live.content = "live";
    live.embedding = {1.0f, 0.0f};
    ASSERT_TRUE(store.put_memory(live).ok);

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.now_ns = now;
    EXPECT_TRUE(store.search(q).empty());

    q.tenant_id = "tenant_b";
    auto found = store.search(q);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].record.memory_id, "live");

    MemoryRecord hijack;
    hijack.memory_id = "live";
    hijack.namespace_id = "agent";
    hijack.content = "overwrite";
    hijack.embedding = {1.0f, 0.0f};
    auto rejected = store.put_memory(hijack);
    EXPECT_FALSE(rejected.ok);
    EXPECT_NE(rejected.error.find("different tenant"), std::string::npos);
}

TEST(AgentMemoryStoreTest, SearchReturnsTopKInScoreOrder) {
    AgentMemoryStore store;
    const int64_t now = AgentMemoryStore::now_ns();

    for (int i = 0; i < 20; ++i) {
        MemoryRecord r;
        r.memory_id = "m" + std::to_string(i);
        r.tenant_id = "tenant_a";
        r.namespace_id = "agent";
        r.content = "memory " + std::to_string(i);
        r.embedding = {1.0f, 0.0f};
        r.created_at_ns = now + i;
        r.importance = static_cast<double>(i);
        ASSERT_TRUE(store.put_memory(r).ok);
    }

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.now_ns = now + 20;
    q.limit = 3;

    auto matches = store.search(q);
    ASSERT_EQ(matches.size(), 3u);
    EXPECT_EQ(matches[0].record.memory_id, "m19");
    EXPECT_EQ(matches[1].record.memory_id, "m18");
    EXPECT_EQ(matches[2].record.memory_id, "m17");
    EXPECT_GE(matches[0].score, matches[1].score);
    EXPECT_GE(matches[1].score, matches[2].score);
}

TEST(AgentMemoryStoreTest, SearchCanSkipAccessCounterUpdates) {
    AgentMemoryStore store;
    const int64_t now = AgentMemoryStore::now_ns();

    MemoryRecord r;
    r.memory_id = "m1";
    r.tenant_id = "tenant_a";
    r.namespace_id = "agent";
    r.content = "memory";
    r.embedding = {1.0f, 0.0f};
    r.created_at_ns = now;
    ASSERT_TRUE(store.put_memory(r).ok);

    const auto before = store.get_memory("m1");
    ASSERT_TRUE(before.has_value());

    MemoryQuery read_only;
    read_only.tenant_id = "tenant_a";
    read_only.namespace_id = "agent";
    read_only.query_embedding = {1.0f, 0.0f};
    read_only.now_ns = now + 1;
    read_only.limit = 1;
    read_only.update_access = false;

    auto read_only_matches = store.search(read_only);
    ASSERT_EQ(read_only_matches.size(), 1u);
    EXPECT_EQ(read_only_matches[0].record.access_count, before->access_count);
    EXPECT_EQ(read_only_matches[0].record.last_accessed_ns,
              before->last_accessed_ns);

    const auto after_read_only = store.get_memory("m1");
    ASSERT_TRUE(after_read_only.has_value());
    EXPECT_EQ(after_read_only->access_count, before->access_count);
    EXPECT_EQ(after_read_only->last_accessed_ns, before->last_accessed_ns);

    read_only.update_access = true;
    read_only.now_ns = now + 2;
    auto updating_matches = store.search(read_only);
    ASSERT_EQ(updating_matches.size(), 1u);
    EXPECT_EQ(updating_matches[0].record.access_count,
              before->access_count + 1);
    EXPECT_EQ(updating_matches[0].record.last_accessed_ns, now + 2);

    const auto after_update = store.get_memory("m1");
    ASSERT_TRUE(after_update.has_value());
    EXPECT_EQ(after_update->access_count, before->access_count + 1);
    EXPECT_EQ(after_update->last_accessed_ns, now + 2);
}

TEST(AgentMemoryStoreTest, AnnSearchPreservesTenantPartitionAndRanking) {
    AgentMemoryStore store;
    AgentMemoryAnnConfig ann;
    ann.mode = AgentMemoryAnnMode::SparseProjection;
    ann.min_records = 1;
    ann.oversample = 32;
    ann.index.tables = 4;
    ann.index.bits_per_table = 4;
    ann.index.probe_radius = 2;
    ann.index.max_candidates = 10'000;
    store.set_ann_config(ann);

    for (int i = 0; i < 200; ++i) {
        MemoryRecord r;
        r.memory_id = "a_" + std::to_string(i);
        r.tenant_id = "tenant_a";
        r.namespace_id = "agent";
        r.content = "tenant a memory " + std::to_string(i);
        r.embedding = {1.0f - static_cast<float>(i) * 0.001f,
                       static_cast<float>(i) * 0.001f};
        r.importance = i == 0 ? 1.0 : 0.0;
        ASSERT_TRUE(store.put_memory(r).ok);
    }
    for (int i = 0; i < 20; ++i) {
        MemoryRecord r;
        r.memory_id = "b_" + std::to_string(i);
        r.tenant_id = "tenant_b";
        r.namespace_id = "agent";
        r.content = "tenant b memory " + std::to_string(i);
        r.embedding = {1.0f, 0.0f};
        r.importance = 10.0;
        ASSERT_TRUE(store.put_memory(r).ok);
    }

    ASSERT_TRUE(store.rebuild_ann_index().ok);

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.limit = 5;

    auto matches = store.search(q);
    ASSERT_EQ(matches.size(), 5u);
    EXPECT_EQ(matches[0].record.memory_id, "a_0");
    for (const auto& match : matches) {
        EXPECT_EQ(match.record.tenant_id, "tenant_a");
    }
    const auto stats = store.stats();
    EXPECT_GT(stats.ann_indexed_vectors, 0u);
    EXPECT_GT(stats.ann_search_count, 0u);
    EXPECT_EQ(stats.ann_fallback_count, 0u);
}

TEST(AgentMemoryStoreTest, AnnAppendMaintainsCleanIndexIncrementally) {
    AgentMemoryStore store;
    AgentMemoryAnnConfig ann;
    ann.mode = AgentMemoryAnnMode::SparseProjection;
    ann.min_records = 1;
    ann.oversample = 16;
    ann.index.tables = 1;
    ann.index.bits_per_table = 1;
    ann.index.probe_radius = 1;
    ann.index.max_candidates = 1'000;
    store.set_ann_config(ann);

    const int64_t now = AgentMemoryStore::now_ns();
    for (int i = 0; i < 2; ++i) {
        MemoryRecord r;
        r.memory_id = "base_" + std::to_string(i);
        r.tenant_id = "tenant_a";
        r.namespace_id = "agent";
        r.content = "base memory " + std::to_string(i);
        r.embedding = {0.0f, 1.0f};
        r.created_at_ns = now;
        ASSERT_TRUE(store.put_memory(r).ok);
    }

    ASSERT_TRUE(store.rebuild_ann_index().ok);
    auto stats = store.stats();
    EXPECT_EQ(stats.ann_rebuild_count, 1u);
    EXPECT_EQ(stats.ann_indexed_vectors, 2u);

    MemoryRecord appended;
    appended.memory_id = "appended";
    appended.tenant_id = "tenant_a";
    appended.namespace_id = "agent";
    appended.content = "appended memory";
    appended.embedding = {1.0f, 0.0f};
    appended.importance = 5.0;
    appended.created_at_ns = now;
    ASSERT_TRUE(store.put_memory(appended).ok);

    stats = store.stats();
    EXPECT_EQ(stats.ann_rebuild_count, 1u);
    EXPECT_EQ(stats.ann_indexed_vectors, 3u);

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.limit = 1;
    q.update_access = false;

    const auto matches = store.search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.memory_id, "appended");
    stats = store.stats();
    EXPECT_EQ(stats.ann_rebuild_count, 1u);
    EXPECT_GT(stats.ann_search_count, 0u);
}

TEST(AgentMemoryStoreTest, AnnSearchSchedulesBackgroundRebuild) {
    AgentMemoryStore store;
    AgentMemoryAnnConfig ann;
    ann.mode = AgentMemoryAnnMode::SparseProjection;
    ann.min_records = 1;
    ann.oversample = 32;
    ann.index.tables = 1;
    ann.index.bits_per_table = 1;
    ann.index.probe_radius = 1;
    ann.index.max_candidates = 1'000;
    store.set_ann_config(ann);

    const int64_t now = AgentMemoryStore::now_ns();
    for (int i = 0; i < 32; ++i) {
        MemoryRecord r;
        r.memory_id = "m_" + std::to_string(i);
        r.tenant_id = "tenant_a";
        r.namespace_id = "agent";
        r.content = "memory " + std::to_string(i);
        r.embedding = i == 0 ? std::vector<float>{1.0f, 0.0f}
                             : std::vector<float>{0.0f, 1.0f};
        r.created_at_ns = now - i;
        ASSERT_TRUE(store.put_memory(r).ok);
    }

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.limit = 1;
    q.update_access = false;

    auto matches = store.search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.memory_id, "m_0");
    EXPECT_EQ(store.stats().ann_fallback_count, 1u);

    bool rebuilt = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (store.stats().ann_rebuild_count >= 1) {
            rebuilt = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(rebuilt);

    matches = store.search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.memory_id, "m_0");
    const auto stats = store.stats();
    EXPECT_GE(stats.ann_search_count, 1u);
    EXPECT_EQ(stats.ann_rebuild_count, 1u);
}

TEST(AgentMemoryStoreTest, AnnMetadataUpdateDoesNotDirtyIndex) {
    AgentMemoryStore store;
    AgentMemoryAnnConfig ann;
    ann.mode = AgentMemoryAnnMode::SparseProjection;
    ann.min_records = 1;
    ann.oversample = 16;
    ann.index.tables = 1;
    ann.index.bits_per_table = 1;
    ann.index.probe_radius = 1;
    ann.index.max_candidates = 1'000;
    store.set_ann_config(ann);

    MemoryRecord original;
    original.memory_id = "m1";
    original.tenant_id = "tenant_a";
    original.namespace_id = "agent";
    original.content = "original";
    original.embedding = {1.0f, 0.0f};
    ASSERT_TRUE(store.put_memory(original).ok);
    ASSERT_TRUE(store.rebuild_ann_index().ok);
    EXPECT_EQ(store.stats().ann_rebuild_count, 1u);

    MemoryRecord updated = original;
    updated.content = "updated metadata";
    updated.importance = 3.0;
    ASSERT_TRUE(store.put_memory(updated).ok);

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.limit = 1;
    q.update_access = false;

    const auto matches = store.search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.content, "updated metadata");
    const auto stats = store.stats();
    EXPECT_EQ(stats.ann_rebuild_count, 1u);
    EXPECT_GE(stats.ann_search_count, 1u);
    EXPECT_EQ(stats.ann_fallback_count, 0u);
}

#ifdef ZEPTO_ENABLE_HNSWLIB
TEST(AgentMemoryStoreTest, HnswAnnSearchPreservesTenantPartition) {
    AgentMemoryStore store;
    AgentMemoryAnnConfig ann;
    ann.mode = AgentMemoryAnnMode::Hnsw;
    ann.min_records = 1;
    ann.oversample = 16;
    ann.index.hnsw_m = 8;
    ann.index.hnsw_ef_construction = 40;
    ann.index.hnsw_ef_search = 40;
    store.set_ann_config(ann);

    for (int i = 0; i < 64; ++i) {
        MemoryRecord r;
        r.memory_id = "a_" + std::to_string(i);
        r.tenant_id = "tenant_a";
        r.namespace_id = "agent";
        r.content = "tenant a memory " + std::to_string(i);
        r.embedding = {1.0f - static_cast<float>(i) * 0.001f,
                       static_cast<float>(i) * 0.001f};
        ASSERT_TRUE(store.put_memory(r).ok);
    }
    for (int i = 0; i < 16; ++i) {
        MemoryRecord r;
        r.memory_id = "b_" + std::to_string(i);
        r.tenant_id = "tenant_b";
        r.namespace_id = "agent";
        r.content = "tenant b memory " + std::to_string(i);
        r.embedding = {1.0f, 0.0f};
        ASSERT_TRUE(store.put_memory(r).ok);
    }

    ASSERT_TRUE(store.rebuild_ann_index().ok);

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    q.limit = 5;

    auto matches = store.search(q);
    ASSERT_EQ(matches.size(), 5u);
    for (const auto& match : matches) {
        EXPECT_EQ(match.record.tenant_id, "tenant_a");
    }
    const auto stats = store.stats();
    EXPECT_GT(stats.ann_indexed_vectors, 0u);
    EXPECT_GT(stats.ann_search_count, 0u);
    EXPECT_EQ(stats.ann_fallback_count, 0u);
}
#endif

TEST(AgentMemoryStoreTest, CapacityEvictionKeepsPinnedAndDropsLowestRetentionMemory) {
    AgentMemoryStore store;
    AgentMemoryEvictionConfig config;
    config.max_memories = 3;
    store.set_eviction_config(config);
    const int64_t now = AgentMemoryStore::now_ns();

    MemoryRecord pinned;
    pinned.memory_id = "pinned";
    pinned.tenant_id = "tenant_a";
    pinned.namespace_id = "agent";
    pinned.content = "pinned";
    pinned.embedding = {1.0f, 0.0f};
    pinned.created_at_ns = now - 10'000'000'000;
    pinned.pinned = true;
    ASSERT_TRUE(store.put_memory(pinned).ok);

    MemoryRecord stale;
    stale.memory_id = "stale";
    stale.tenant_id = "tenant_a";
    stale.namespace_id = "agent";
    stale.content = "stale";
    stale.embedding = {1.0f, 0.0f};
    stale.created_at_ns = now - 9'000'000'000;
    ASSERT_TRUE(store.put_memory(stale).ok);

    MemoryRecord important;
    important.memory_id = "important";
    important.tenant_id = "tenant_a";
    important.namespace_id = "agent";
    important.content = "important";
    important.embedding = {1.0f, 0.0f};
    important.created_at_ns = now - 8'000'000'000;
    important.importance = 5.0;
    ASSERT_TRUE(store.put_memory(important).ok);

    MemoryRecord recent;
    recent.memory_id = "recent";
    recent.tenant_id = "tenant_a";
    recent.namespace_id = "agent";
    recent.content = "recent";
    recent.embedding = {1.0f, 0.0f};
    recent.created_at_ns = now;
    ASSERT_TRUE(store.put_memory(recent).ok);

    EXPECT_EQ(store.stats().memory_count, 3u);
    EXPECT_EQ(store.stats().evicted_memory_count, 1u);
    EXPECT_TRUE(store.get_memory("pinned").has_value());
    EXPECT_TRUE(store.get_memory("important").has_value());
    EXPECT_TRUE(store.get_memory("recent").has_value());
    EXPECT_FALSE(store.get_memory("stale").has_value());
}

TEST(AgentMemoryStoreTest, CapacityEvictionAllowsPinnedOverflow) {
    AgentMemoryStore store;
    AgentMemoryEvictionConfig config;
    config.max_memories = 1;
    store.set_eviction_config(config);

    for (int i = 0; i < 2; ++i) {
        MemoryRecord r;
        r.memory_id = "pinned_" + std::to_string(i);
        r.tenant_id = "tenant_a";
        r.namespace_id = "agent";
        r.content = "pinned";
        r.embedding = {1.0f, 0.0f};
        r.pinned = true;
        ASSERT_TRUE(store.put_memory(r).ok);
    }

    EXPECT_EQ(store.stats().memory_count, 2u);
    EXPECT_EQ(store.stats().evicted_memory_count, 0u);
}

TEST(AgentMemoryStoreTest, ExpiredEvictionRemovesMemoryAndCacheOnWrite) {
    AgentMemoryStore store;
    const int64_t now = AgentMemoryStore::now_ns();

    MemoryRecord expired_memory;
    expired_memory.memory_id = "expired";
    expired_memory.tenant_id = "tenant_a";
    expired_memory.namespace_id = "agent";
    expired_memory.content = "expired";
    expired_memory.embedding = {1.0f, 0.0f};
    expired_memory.expires_at_ns = now - 1;
    expired_memory.pinned = true;
    ASSERT_TRUE(store.put_memory(expired_memory).ok);
    EXPECT_FALSE(store.get_memory("expired").has_value());

    MemoryRecord updated_memory;
    updated_memory.memory_id = "updated";
    updated_memory.tenant_id = "tenant_a";
    updated_memory.namespace_id = "agent";
    updated_memory.content = "updated";
    updated_memory.embedding = {1.0f, 0.0f};
    ASSERT_TRUE(store.put_memory(updated_memory).ok);
    updated_memory.expires_at_ns = now - 1;
    ASSERT_TRUE(store.put_memory(updated_memory).ok);
    EXPECT_FALSE(store.get_memory("updated").has_value());

    CacheEntry expired_cache;
    expired_cache.cache_id = "expired_cache";
    expired_cache.tenant_id = "tenant_a";
    expired_cache.namespace_id = "agent";
    expired_cache.prompt = "expired prompt";
    expired_cache.response = "expired response";
    expired_cache.embedding = {1.0f, 0.0f};
    expired_cache.expires_at_ns = now - 1;
    ASSERT_TRUE(store.store_cache(expired_cache).ok);

    CacheLookup lookup;
    lookup.tenant_id = "tenant_a";
    lookup.namespace_id = "agent";
    lookup.prompt = "expired prompt";
    EXPECT_FALSE(store.lookup_cache(lookup).hit);

    CacheEntry updated_cache;
    updated_cache.cache_id = "updated_cache";
    updated_cache.tenant_id = "tenant_a";
    updated_cache.namespace_id = "agent";
    updated_cache.prompt = "updated prompt";
    updated_cache.response = "updated response";
    updated_cache.embedding = {1.0f, 0.0f};
    ASSERT_TRUE(store.store_cache(updated_cache).ok);
    updated_cache.expires_at_ns = now - 1;
    ASSERT_TRUE(store.store_cache(updated_cache).ok);
    lookup.prompt = "updated prompt";
    EXPECT_FALSE(store.lookup_cache(lookup).hit);

    EXPECT_EQ(store.stats().evicted_memory_count, 2u);
    EXPECT_EQ(store.stats().evicted_cache_count, 2u);
}

TEST(AgentMemoryStoreTest, CacheCapacityEvictsLowestRetentionEntry) {
    AgentMemoryStore store;
    AgentMemoryEvictionConfig config;
    config.max_cache_entries = 2;
    store.set_eviction_config(config);
    const int64_t now = AgentMemoryStore::now_ns();

    CacheEntry old;
    old.cache_id = "old";
    old.tenant_id = "tenant_a";
    old.namespace_id = "agent";
    old.prompt = "old prompt";
    old.response = "old response";
    old.embedding = {1.0f, 0.0f};
    old.created_at_ns = now - 10'000'000'000;
    ASSERT_TRUE(store.store_cache(old).ok);

    CacheEntry hot;
    hot.cache_id = "hot";
    hot.tenant_id = "tenant_a";
    hot.namespace_id = "agent";
    hot.prompt = "hot prompt";
    hot.response = "hot response";
    hot.embedding = {1.0f, 0.0f};
    hot.created_at_ns = now - 9'000'000'000;
    hot.access_count = 10;
    ASSERT_TRUE(store.store_cache(hot).ok);

    CacheEntry fresh;
    fresh.cache_id = "fresh";
    fresh.tenant_id = "tenant_a";
    fresh.namespace_id = "agent";
    fresh.prompt = "fresh prompt";
    fresh.response = "fresh response";
    fresh.embedding = {1.0f, 0.0f};
    fresh.created_at_ns = now;
    ASSERT_TRUE(store.store_cache(fresh).ok);

    EXPECT_EQ(store.stats().cache_count, 2u);
    EXPECT_EQ(store.stats().evicted_cache_count, 1u);

    CacheLookup old_lookup;
    old_lookup.tenant_id = "tenant_a";
    old_lookup.namespace_id = "agent";
    old_lookup.prompt = "old prompt";
    EXPECT_FALSE(store.lookup_cache(old_lookup).hit);

    CacheLookup hot_lookup;
    hot_lookup.tenant_id = "tenant_a";
    hot_lookup.namespace_id = "agent";
    hot_lookup.prompt = "hot prompt";
    EXPECT_TRUE(store.lookup_cache(hot_lookup).hit);
}

TEST(AgentMemoryStoreTest, ExactAndSemanticCache) {
    AgentMemoryStore store;
    CacheEntry entry;
    entry.tenant_id = "tenant_a";
    entry.namespace_id = "agent";
    entry.prompt = "Summarize the task";
    entry.response = "Short summary";
    entry.embedding = {1.0f, 0.0f};
    ASSERT_TRUE(store.store_cache(entry).ok);

    CacheLookup exact;
    exact.tenant_id = "tenant_a";
    exact.namespace_id = "agent";
    exact.prompt = " summarize   THE task ";
    auto exact_hit = store.lookup_cache(exact);
    ASSERT_TRUE(exact_hit.hit);
    EXPECT_TRUE(exact_hit.exact);

    CacheLookup semantic;
    semantic.tenant_id = "tenant_a";
    semantic.namespace_id = "agent";
    semantic.prompt = "Different prompt";
    semantic.embedding = {0.95f, 0.05f};
    semantic.semantic_threshold = 0.9;
    auto semantic_hit = store.lookup_cache(semantic);
    ASSERT_TRUE(semantic_hit.hit);
    EXPECT_FALSE(semantic_hit.exact);
}

TEST(AgentMemoryStoreTest, PersistenceRoundTripRestoresMemoryAndCache) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_" + std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryStore store;
    MemoryRecord record;
    record.memory_id = "mem_persist";
    record.tenant_id = "tenant_a";
    record.namespace_id = "agent";
    record.user_id = "u1";
    record.content = "persistent preference";
    record.metadata_json = R"({"source":"test"})";
    record.embedding = {1.0f, 0.0f};
    record.token_count = 4;
    record.importance = 2.0;
    record.pinned = true;
    ASSERT_TRUE(store.put_memory(record).ok);

    CacheEntry cache;
    cache.cache_id = "cache_persist";
    cache.tenant_id = "tenant_a";
    cache.namespace_id = "agent";
    cache.prompt = "Summarize";
    cache.response = "Summary";
    cache.embedding = {0.9f, 0.1f};
    ASSERT_TRUE(store.store_cache(cache).ok);

    auto saved = store.save_to_directory(dir.string());
    ASSERT_TRUE(saved.ok) << saved.error;

    AgentMemoryStore loaded;
    auto load = loaded.load_from_directory(dir.string());
    ASSERT_TRUE(load.ok) << load.error;
    EXPECT_EQ(loaded.stats().memory_count, 1u);
    EXPECT_EQ(loaded.stats().cache_count, 1u);
    EXPECT_EQ(loaded.stats().embedding_dim, 2u);

    MemoryQuery q;
    q.tenant_id = "tenant_a";
    q.namespace_id = "agent";
    q.user_id = "u1";
    q.query_embedding = {1.0f, 0.0f};
    auto matches = loaded.search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.memory_id, "mem_persist");
    EXPECT_TRUE(matches[0].record.pinned);
    EXPECT_EQ(matches[0].record.metadata_json, R"({"source":"test"})");

    CacheLookup lookup;
    lookup.tenant_id = "tenant_a";
    lookup.namespace_id = "agent";
    lookup.prompt = " summarize ";
    auto hit = loaded.lookup_cache(lookup);
    ASSERT_TRUE(hit.hit);
    EXPECT_TRUE(hit.exact);
    EXPECT_EQ(hit.entry.cache_id, "cache_persist");

    std::filesystem::remove_all(dir);
}

TEST(AgentMemoryStoreTest, SurvivesConcurrentPuts) {
    AgentMemoryStore store;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                MemoryRecord r;
                r.memory_id = "m" + std::to_string(t) + "_" + std::to_string(i);
                r.tenant_id = "tenant";
                r.namespace_id = "agent";
                r.content = "memory";
                r.embedding = {1.0f, 0.0f};
                EXPECT_TRUE(store.put_memory(r).ok);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(store.stats().memory_count, kThreads * kPerThread);
}

TEST(AgentMemoryStoreTest, OwnerScopedAutoIdsIncludeNodeAndEpoch) {
    AgentMemoryStore legacy;
    MemoryRecord legacy_memory;
    legacy_memory.content = "legacy";
    legacy_memory.embedding = {1.0f, 0.0f};
    EXPECT_EQ(legacy.put_memory(legacy_memory).id, "mem_1");

    CacheEntry legacy_cache;
    legacy_cache.prompt = "legacy prompt";
    legacy_cache.response = "legacy response";
    legacy_cache.embedding = {1.0f, 0.0f};
    EXPECT_EQ(legacy.store_cache(legacy_cache).id, "cache_1");

    AgentMemoryStore routed;
    AgentMemoryIdConfig ids;
    ids.owner_scoped = true;
    ids.node_id = 7;
    ids.ring_epoch = 42;
    routed.set_id_config(ids);
    EXPECT_TRUE(routed.id_config().owner_scoped);
    EXPECT_EQ(routed.id_config().node_id, 7u);
    EXPECT_EQ(routed.id_config().ring_epoch, 42u);

    MemoryRecord memory;
    memory.content = "routed";
    memory.embedding = {1.0f, 0.0f};
    EXPECT_EQ(routed.put_memory(memory).id, "mem_7_42_1");

    MemoryRecord second_memory;
    second_memory.content = "routed again";
    second_memory.embedding = {1.0f, 0.0f};
    EXPECT_EQ(routed.put_memory(second_memory).id, "mem_7_42_2");

    CacheEntry cache;
    cache.prompt = "routed prompt";
    cache.response = "routed response";
    cache.embedding = {1.0f, 0.0f};
    EXPECT_EQ(routed.store_cache(cache).id, "cache_7_42_1");
}

TEST(AgentMemoryRouterTest, LocalModeAlwaysOwnsLocally) {
    AgentMemoryRouterConfig config;
    config.self_node_id = 7;
    config.ring_epoch = 3;
    config.mode = AgentMemoryRoutingMode::Local;
    AgentMemoryRouter router(config);
    router.add_node(1);
    router.add_node(2);

    const auto key = AgentMemoryRouter::memory_key(
        "tenant_a", "agent", "session_1", "agent_1", "user_1", "mem_1");
    const auto owner = router.route(key);
    EXPECT_EQ(owner.node_id, 7u);
    EXPECT_EQ(owner.ring_epoch, 3u);
    EXPECT_TRUE(owner.local);
    EXPECT_TRUE(router.is_local_owner(key));
}

TEST(AgentMemoryRouterTest, RoutedModeUsesLogicalSubjectPriorityAndStableOwners) {
    AgentMemoryRouterConfig config;
    config.self_node_id = 2;
    config.ring_epoch = 11;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 32;
    AgentMemoryRouter router(config);
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);

    const auto key = AgentMemoryRouter::memory_key(
        "tenant_a", "", "session_1", "agent_1", "user_1", "mem_1");
    EXPECT_EQ(key.namespace_id, "default");
    EXPECT_EQ(key.logical_subject, "session_1");
    EXPECT_EQ(key.entry_kind, AgentMemoryEntryKind::Memory);

    const auto owner = router.route(key);
    const auto repeated = router.route(key);
    EXPECT_EQ(repeated.node_id, owner.node_id);
    EXPECT_EQ(repeated.local, owner.local);
    EXPECT_EQ(owner.ring_epoch, 11u);
    const auto nodes = router.nodes();
    EXPECT_NE(std::find(nodes.begin(), nodes.end(), owner.node_id), nodes.end());

    const auto agent_key = AgentMemoryRouter::memory_key(
        "tenant_a", "agent", "", "agent_1", "user_1", "mem_1");
    EXPECT_EQ(agent_key.logical_subject, "agent_1");
    const auto user_key = AgentMemoryRouter::memory_key(
        "tenant_a", "agent", "", "", "user_1", "mem_1");
    EXPECT_EQ(user_key.logical_subject, "user_1");
    const auto fallback_key = AgentMemoryRouter::memory_key(
        "tenant_a", "agent", "", "", "", "mem_1");
    EXPECT_EQ(fallback_key.logical_subject, "mem_1");

    const auto cache_key = AgentMemoryRouter::cache_key(
        "tenant_a", "agent", "normalized_prompt_hash");
    EXPECT_EQ(cache_key.logical_subject, "normalized_prompt_hash");
    EXPECT_EQ(cache_key.entry_kind, AgentMemoryEntryKind::Cache);
    EXPECT_NE(AgentMemoryRouter::stable_key(cache_key),
              AgentMemoryRouter::stable_key(fallback_key));
}

TEST(AgentMemoryRouterTest, NodeRemovalReroutesOwnedKeys) {
    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 64;
    AgentMemoryRouter router(config);
    router.add_node(1);
    router.add_node(2);
    router.add_node(3);

    bool found = false;
    zeptodb::ai::AgentMemoryRoutingKey key;
    for (int i = 0; i < 10'000; ++i) {
        key = AgentMemoryRouter::memory_key(
            "tenant_a", "agent", "", "", "", "mem_" + std::to_string(i));
        if (router.route(key).node_id == 3u) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    router.remove_node(3);
    EXPECT_NE(router.route(key).node_id, 3u);
    const auto nodes = router.nodes();
    EXPECT_EQ(std::find(nodes.begin(), nodes.end(),
                        static_cast<AgentMemoryNodeId>(3)), nodes.end());
}

TEST(AgentMemoryWireTest, RoundTripsMemoryCacheAndStoreResult) {
    MemoryRecord memory;
    memory.memory_id = "mem_wire";
    memory.tenant_id = "tenant_a";
    memory.namespace_id = "agent";
    memory.user_id = "user_1";
    memory.session_id = "session_1";
    memory.agent_id = "agent_1";
    memory.type = "preference";
    memory.content = "prefers concise answers";
    memory.metadata_json = R"({"trusted":true})";
    memory.embedding = {1.0f, 0.5f};
    memory.token_count = 4;
    memory.importance = 2.5;
    memory.created_at_ns = 10;
    memory.last_accessed_ns = 11;
    memory.expires_at_ns = 12;
    memory.pinned = true;
    memory.access_count = 3;

    MemoryRecord decoded_memory;
    std::string error;
    const auto memory_payload = zeptodb::ai::serialize_memory_record(memory);
    ASSERT_TRUE(zeptodb::ai::deserialize_memory_record(
        memory_payload.data(), memory_payload.size(), &decoded_memory, &error))
        << error;
    EXPECT_EQ(decoded_memory.memory_id, memory.memory_id);
    EXPECT_EQ(decoded_memory.metadata_json, memory.metadata_json);
    EXPECT_EQ(decoded_memory.embedding, memory.embedding);
    EXPECT_TRUE(decoded_memory.pinned);
    EXPECT_EQ(decoded_memory.access_count, 3u);

    MemoryQuery query;
    query.tenant_id = "tenant_a";
    query.namespace_id = "agent";
    query.user_id = "user_1";
    query.session_id = "session_1";
    query.agent_id = "agent_1";
    query.type = "preference";
    query.query_embedding = {1.0f, 0.5f};
    query.limit = 7;
    query.now_ns = 1234;
    query.include_expired = true;
    query.force_scan = true;
    query.update_access = false;
    MemoryQuery decoded_query;
    const auto query_payload = zeptodb::ai::serialize_memory_query(query);
    ASSERT_TRUE(zeptodb::ai::deserialize_memory_query(
        query_payload.data(), query_payload.size(), &decoded_query, &error))
        << error;
    EXPECT_EQ(decoded_query.tenant_id, query.tenant_id);
    EXPECT_EQ(decoded_query.limit, 7u);
    EXPECT_TRUE(decoded_query.include_expired);
    EXPECT_TRUE(decoded_query.force_scan);
    EXPECT_FALSE(decoded_query.update_access);

    MemorySearchRpcResult search_result;
    zeptodb::ai::MemorySearchResult match;
    match.record = memory;
    match.score = 3.5;
    match.similarity = 0.9;
    search_result.matches.push_back(match);
    MemorySearchRpcResult decoded_search_result;
    const auto search_result_payload =
        zeptodb::ai::serialize_memory_search_result(search_result);
    ASSERT_TRUE(zeptodb::ai::deserialize_memory_search_result(
        search_result_payload.data(), search_result_payload.size(),
        &decoded_search_result, &error)) << error;
    ASSERT_EQ(decoded_search_result.matches.size(), 1u);
    EXPECT_EQ(decoded_search_result.matches[0].record.memory_id, "mem_wire");
    EXPECT_DOUBLE_EQ(decoded_search_result.matches[0].score, 3.5);

    CacheEntry cache;
    cache.cache_id = "cache_wire";
    cache.tenant_id = "tenant_a";
    cache.namespace_id = "agent";
    cache.prompt = "Hello";
    cache.response = "Hi";
    cache.metadata_json = R"({"model":"mock"})";
    cache.embedding = {0.1f, 0.2f};
    cache.token_count = 2;
    cache.created_at_ns = 20;
    cache.last_accessed_ns = 21;
    cache.expires_at_ns = 22;
    cache.access_count = 4;

    CacheEntry decoded_cache;
    const auto cache_payload = zeptodb::ai::serialize_cache_entry(cache);
    ASSERT_TRUE(zeptodb::ai::deserialize_cache_entry(
        cache_payload.data(), cache_payload.size(), &decoded_cache, &error))
        << error;
    EXPECT_EQ(decoded_cache.cache_id, cache.cache_id);
    EXPECT_EQ(decoded_cache.response, cache.response);
    EXPECT_EQ(decoded_cache.embedding, cache.embedding);
    EXPECT_EQ(decoded_cache.access_count, 4u);

    zeptodb::ai::StoreResult stored{true, "mem_wire", ""};
    zeptodb::ai::StoreResult decoded_result;
    const auto result_payload = zeptodb::ai::serialize_store_result(stored);
    ASSERT_TRUE(zeptodb::ai::deserialize_store_result(
        result_payload.data(), result_payload.size(), &decoded_result, &error))
        << error;
    EXPECT_TRUE(decoded_result.ok);
    EXPECT_EQ(decoded_result.id, "mem_wire");

    zeptodb::ai::MemoryGetRequest get_request{"mem_wire", "tenant_a"};
    zeptodb::ai::MemoryGetRequest decoded_get_request;
    const auto get_request_payload =
        zeptodb::ai::serialize_memory_get_request(get_request);
    ASSERT_TRUE(zeptodb::ai::deserialize_memory_get_request(
        get_request_payload.data(), get_request_payload.size(),
        &decoded_get_request, &error)) << error;
    EXPECT_EQ(decoded_get_request.memory_id, "mem_wire");
    EXPECT_EQ(decoded_get_request.tenant_id, "tenant_a");

    zeptodb::ai::MemoryDeleteRequest delete_request{"mem_wire", "tenant_a"};
    zeptodb::ai::MemoryDeleteRequest decoded_delete_request;
    const auto delete_request_payload =
        zeptodb::ai::serialize_memory_delete_request(delete_request);
    ASSERT_TRUE(zeptodb::ai::deserialize_memory_delete_request(
        delete_request_payload.data(), delete_request_payload.size(),
        &decoded_delete_request, &error)) << error;
    EXPECT_EQ(decoded_delete_request.memory_id, "mem_wire");
    EXPECT_EQ(decoded_delete_request.tenant_id, "tenant_a");

    zeptodb::ai::CacheDeleteRequest cache_delete_request{
        "tenant_a", "agent", "Hello"};
    zeptodb::ai::CacheDeleteRequest decoded_cache_delete_request;
    const auto cache_delete_payload =
        zeptodb::ai::serialize_cache_delete_request(cache_delete_request);
    ASSERT_TRUE(zeptodb::ai::deserialize_cache_delete_request(
        cache_delete_payload.data(), cache_delete_payload.size(),
        &decoded_cache_delete_request, &error)) << error;
    EXPECT_EQ(decoded_cache_delete_request.tenant_id, "tenant_a");
    EXPECT_EQ(decoded_cache_delete_request.namespace_id, "agent");
    EXPECT_EQ(decoded_cache_delete_request.prompt, "Hello");

    zeptodb::ai::MemoryGetResult get_result;
    get_result.found = true;
    get_result.record = memory;
    zeptodb::ai::MemoryGetResult decoded_get_result;
    const auto get_result_payload =
        zeptodb::ai::serialize_memory_get_result(get_result);
    ASSERT_TRUE(zeptodb::ai::deserialize_memory_get_result(
        get_result_payload.data(), get_result_payload.size(),
        &decoded_get_result, &error)) << error;
    ASSERT_TRUE(decoded_get_result.found);
    EXPECT_EQ(decoded_get_result.record.memory_id, "mem_wire");

    CacheLookup lookup;
    lookup.tenant_id = "tenant_a";
    lookup.namespace_id = "agent";
    lookup.prompt = "Hello";
    lookup.embedding = {0.1f, 0.2f};
    lookup.semantic_threshold = 0.8;
    lookup.now_ns = 123;
    CacheLookup decoded_lookup;
    const auto lookup_payload = zeptodb::ai::serialize_cache_lookup(lookup);
    ASSERT_TRUE(zeptodb::ai::deserialize_cache_lookup(
        lookup_payload.data(), lookup_payload.size(), &decoded_lookup, &error))
        << error;
    EXPECT_EQ(decoded_lookup.prompt, "Hello");
    EXPECT_EQ(decoded_lookup.embedding, lookup.embedding);

    CacheLookupResult lookup_result;
    lookup_result.hit = true;
    lookup_result.exact = true;
    lookup_result.score = 1.0;
    lookup_result.entry = cache;
    CacheLookupResult decoded_lookup_result;
    const auto lookup_result_payload =
        zeptodb::ai::serialize_cache_lookup_result(lookup_result);
    ASSERT_TRUE(zeptodb::ai::deserialize_cache_lookup_result(
        lookup_result_payload.data(), lookup_result_payload.size(),
        &decoded_lookup_result, &error)) << error;
    ASSERT_TRUE(decoded_lookup_result.hit);
    EXPECT_TRUE(decoded_lookup_result.exact);
    EXPECT_EQ(decoded_lookup_result.entry.cache_id, "cache_wire");
}

class AgentMemoryHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = zepto_test_util::pick_free_port();
        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
        server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
        server_->start_async();
        std::this_thread::sleep_for(80ms);
    }

    void TearDown() override {
        if (server_) server_->stop();
    }

    uint16_t port_ = 0;
    std::unique_ptr<ZeptoPipeline> pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
};

TEST_F(AgentMemoryHttpTest, MemorySearchContextAndCacheEndpoints) {
    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent","user_id":"u1",)"
        R"("content":"remember blue","embedding":[1,0],)"
        R"("token_count":3,"pinned":true})");
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_NE(put.body.find("memory_id"), std::string::npos);

    auto search = http_post(port_, "/api/ai/memories/search",
        R"({"tenant_id":"t1","namespace":"agent","user_id":"u1",)"
        R"("query_embedding":[1,0],"limit":5})");
    ASSERT_EQ(search.status, 200) << search.body;
    EXPECT_NE(search.body.find("remember blue"), std::string::npos);
    EXPECT_NE(search.body.find("\"rows\":1"), std::string::npos);

    auto context = http_post(port_, "/api/ai/context",
        R"({"tenant_id":"t1","namespace":"agent","query_embedding":[1,0],)"
        R"("token_budget":3,"limit":5})");
    ASSERT_EQ(context.status, 200) << context.body;
    EXPECT_NE(context.body.find("\"token_count\":3"), std::string::npos);

    auto cache = http_post(port_, "/api/ai/cache/store",
        R"({"tenant_id":"t1","namespace":"agent","prompt":"Hi",)"
        R"("response":"Hello","embedding":[1,0]})");
    ASSERT_EQ(cache.status, 200) << cache.body;

    auto lookup = http_post(port_, "/api/ai/cache/lookup",
        R"({"tenant_id":"t1","namespace":"agent","prompt":" hi "})");
    ASSERT_EQ(lookup.status, 200) << lookup.body;
    EXPECT_NE(lookup.body.find("\"hit\":true"), std::string::npos);
    EXPECT_NE(lookup.body.find("\"kind\":\"exact\""), std::string::npos);
}

TEST_F(AgentMemoryHttpTest, RoutedMemoryAndCacheWritesUseRemoteOwner) {
    auto remote_pipeline = make_pipeline();
    auto remote_executor = std::make_unique<QueryExecutor>(*remote_pipeline);
    zeptodb::server::HttpServer remote_server(
        *remote_executor, zepto_test_util::pick_free_port());

    zeptodb::cluster::TcpRpcServer remote_rpc;
    remote_rpc.set_agent_memory_put_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_memory_put_rpc(data, len);
        });
    remote_rpc.set_agent_cache_store_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_cache_store_rpc(data, len);
        });
    remote_rpc.set_agent_memory_delete_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_memory_delete_rpc(data, len);
        });
    remote_rpc.set_agent_cache_delete_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_cache_delete_rpc(data, len);
        });
    remote_rpc.set_agent_memory_get_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_memory_get_rpc(data, len);
        });
    remote_rpc.set_agent_memory_search_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_memory_search_rpc(data, len);
        });
    remote_rpc.set_agent_cache_lookup_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_cache_lookup_rpc(data, len);
        });
    const uint16_t rpc_port = zepto_test_util::pick_free_port();
    remote_rpc.start(rpc_port, [](const std::string&) {
        zeptodb::sql::QueryResultSet result;
        result.error = "SQL is not used by this test";
        return result;
    });

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 64;

    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", rpc_port, 2000));
    server_->set_agent_memory_routing(config, {1, 2}, std::move(remotes));

    AgentMemoryRouter probe(config);
    probe.add_node(1);
    probe.add_node(2);

    std::string remote_memory_id;
    std::string local_memory_id;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "remote_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id == 2u) {
            remote_memory_id = candidate;
            break;
        }
    }
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "local_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id == 1u) {
            local_memory_id = candidate;
            break;
        }
    }
    ASSERT_FALSE(remote_memory_id.empty());
    ASSERT_FALSE(local_memory_id.empty());

    auto local_put = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        local_memory_id +
        R"(","content":"local memory","embedding":[1,0],)"
        R"("token_count":3,"importance":0.0})");
    ASSERT_EQ(local_put.status, 200) << local_put.body;
    ASSERT_TRUE(server_->agent_memory_store().get_memory(local_memory_id).has_value());

    auto put = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        remote_memory_id +
        R"(","content":"remote memory","embedding":[1,0],)"
        R"("token_count":3,"importance":5.0})");
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_FALSE(server_->agent_memory_store().get_memory(remote_memory_id).has_value());
    auto remote_memory =
        remote_server.agent_memory_store().get_memory(remote_memory_id, "t1");
    ASSERT_TRUE(remote_memory.has_value());
    EXPECT_EQ(remote_memory->content, "remote memory");

    auto routed_get = http_get(port_, "/api/ai/memories/" + remote_memory_id +
                                      "?tenant_id=t1&namespace=agent");
    ASSERT_EQ(routed_get.status, 200) << routed_get.body;
    EXPECT_NE(routed_get.body.find("\"found\":true"), std::string::npos);
    EXPECT_NE(routed_get.body.find("remote memory"), std::string::npos);

    auto routed_search = http_post(port_, "/api/ai/memories/search",
        R"({"tenant_id":"t1","namespace":"agent","query_embedding":[1,0],)"
        R"("limit":2})");
    ASSERT_EQ(routed_search.status, 200) << routed_search.body;
    EXPECT_NE(routed_search.body.find("\"rows\":2"), std::string::npos);
    EXPECT_NE(routed_search.body.find("local memory"), std::string::npos);
    EXPECT_NE(routed_search.body.find("remote memory"), std::string::npos);

    auto routed_context = http_post(port_, "/api/ai/context",
        R"({"tenant_id":"t1","namespace":"agent","query_embedding":[1,0],)"
        R"("token_budget":3,"limit":2})");
    ASSERT_EQ(routed_context.status, 200) << routed_context.body;
    EXPECT_NE(routed_context.body.find("\"rows\":1"), std::string::npos);
    EXPECT_NE(routed_context.body.find("\"token_count\":3"), std::string::npos);
    EXPECT_NE(routed_context.body.find("remote memory"), std::string::npos);

    std::string remote_prompt;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "Remote_prompt_" + std::to_string(i);
        const auto key = AgentMemoryRouter::cache_key(
            "t1", "agent", AgentMemoryStore::normalize_prompt(candidate));
        if (probe.route(key).node_id == 2u) {
            remote_prompt = candidate;
            break;
        }
    }
    ASSERT_FALSE(remote_prompt.empty());

    auto cache = http_post(port_, "/api/ai/cache/store",
        std::string(R"({"tenant_id":"t1","namespace":"agent","prompt":")") +
        remote_prompt +
        R"(","response":"remote response","embedding":[1,0]})");
    ASSERT_EQ(cache.status, 200) << cache.body;

    CacheLookup local_lookup;
    local_lookup.tenant_id = "t1";
    local_lookup.namespace_id = "agent";
    local_lookup.prompt = remote_prompt;
    EXPECT_FALSE(server_->agent_memory_store().lookup_cache(local_lookup).hit);

    CacheLookup remote_lookup = local_lookup;
    auto remote_hit = remote_server.agent_memory_store().lookup_cache(remote_lookup);
    ASSERT_TRUE(remote_hit.hit);
    EXPECT_TRUE(remote_hit.exact);
    EXPECT_EQ(remote_hit.entry.response, "remote response");

    auto routed_lookup = http_post(port_, "/api/ai/cache/lookup",
        std::string(R"({"tenant_id":"t1","namespace":"agent","prompt":")") +
        remote_prompt + R"("})");
    ASSERT_EQ(routed_lookup.status, 200) << routed_lookup.body;
    EXPECT_NE(routed_lookup.body.find("\"hit\":true"), std::string::npos);
    EXPECT_NE(routed_lookup.body.find("\"kind\":\"exact\""), std::string::npos);
    EXPECT_NE(routed_lookup.body.find("remote response"), std::string::npos);

    auto routed_semantic_lookup = http_post(port_, "/api/ai/cache/lookup",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("prompt":"similar remote prompt","embedding":[1,0],)"
        R"("semantic_threshold":0.8})");
    ASSERT_EQ(routed_semantic_lookup.status, 200) << routed_semantic_lookup.body;
    EXPECT_NE(routed_semantic_lookup.body.find("\"hit\":true"), std::string::npos);
    EXPECT_NE(routed_semantic_lookup.body.find("\"kind\":\"semantic\""),
              std::string::npos);
    EXPECT_NE(routed_semantic_lookup.body.find("remote response"), std::string::npos);

    auto delete_remote_memory = http_delete(
        port_, "/api/ai/memories/" + remote_memory_id +
               "?tenant_id=t1&namespace=agent");
    ASSERT_EQ(delete_remote_memory.status, 200) << delete_remote_memory.body;
    EXPECT_FALSE(remote_server.agent_memory_store()
                     .get_memory(remote_memory_id, "t1")
                     .has_value());

    auto delete_remote_cache = http_delete(
        port_, "/api/ai/cache?tenant_id=t1&namespace=agent&prompt=" +
               remote_prompt);
    ASSERT_EQ(delete_remote_cache.status, 200) << delete_remote_cache.body;
    CacheLookup deleted_lookup = local_lookup;
    deleted_lookup.prompt = remote_prompt;
    EXPECT_FALSE(remote_server.agent_memory_store()
                     .lookup_cache(deleted_lookup)
                     .hit);

    remote_rpc.stop();
}

TEST_F(AgentMemoryHttpTest, RoutedRemoteOwnerWritePersistsOwnerWalBeforeAck) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_remote_owner_wal_" +
         std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    auto remote_pipeline = make_pipeline();
    auto remote_executor = std::make_unique<QueryExecutor>(*remote_pipeline);
    zeptodb::server::HttpServer remote_server(
        *remote_executor, zepto_test_util::pick_free_port());

    AgentMemoryRouterConfig remote_config;
    remote_config.self_node_id = 2;
    remote_config.ring_epoch = 7;
    remote_config.mode = AgentMemoryRoutingMode::Routed;
    remote_config.virtual_nodes_per_node = 64;

    std::string error;
    ASSERT_TRUE(remote_server.set_agent_memory_persistence(dir.string(), &error, 0))
        << error;
    ASSERT_TRUE(remote_server.set_agent_memory_routing(
        remote_config, {1, 2}, {}, &error)) << error;

    zeptodb::cluster::TcpRpcServer remote_rpc;
    remote_rpc.set_agent_memory_put_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_memory_put_rpc(data, len);
        });
    const uint16_t rpc_port = zepto_test_util::pick_free_port();
    remote_rpc.start(rpc_port, [](const std::string&) {
        zeptodb::sql::QueryResultSet result;
        result.error = "SQL is not used by this test";
        return result;
    });

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 64;

    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", rpc_port, 2000));
    ASSERT_TRUE(server_->set_agent_memory_routing(
        config, {1, 2}, std::move(remotes), &error)) << error;

    AgentMemoryRouter probe(config);
    probe.add_node(1);
    probe.add_node(2);
    std::string remote_memory_id;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "remote_wal_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id == 2u) {
            remote_memory_id = candidate;
            break;
        }
    }
    ASSERT_FALSE(remote_memory_id.empty());

    auto put = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        remote_memory_id +
        R"(","content":"remote owner wal","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;

    const auto shard_dir = dir / "node-2" / "shard-0";
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "manifest.json"));
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "wal.log"));
    EXPECT_FALSE(std::filesystem::exists(shard_dir / "records.bin"));
    EXPECT_FALSE(std::filesystem::exists(shard_dir / "vectors.bin"));

    auto replay_pipeline = make_pipeline();
    auto replay_executor = std::make_unique<QueryExecutor>(*replay_pipeline);
    zeptodb::server::HttpServer replay_server(
        *replay_executor, zepto_test_util::pick_free_port());
    ASSERT_TRUE(replay_server.set_agent_memory_persistence(dir.string(), &error))
        << error;
    ASSERT_TRUE(replay_server.set_agent_memory_routing(
        remote_config, {1, 2}, {}, &error)) << error;

    MemoryQuery q;
    q.tenant_id = "t1";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    const auto matches = replay_server.agent_memory_store().search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.memory_id, remote_memory_id);
    EXPECT_EQ(matches[0].record.content, "remote owner wal");

    remote_rpc.stop();
    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, SyncReplicationCopiesOwnerWalToReplicaShard) {
    const auto base = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_sync_replica_" + std::to_string(::getpid()) +
         "_" + std::to_string(AgentMemoryStore::now_ns()));
    const auto owner_dir = base / "owner";
    const auto replica_dir = base / "replica";

    auto replica_pipeline = make_pipeline();
    auto replica_executor = std::make_unique<QueryExecutor>(*replica_pipeline);
    zeptodb::server::HttpServer replica_server(
        *replica_executor, zepto_test_util::pick_free_port());

    AgentMemoryRouterConfig replica_config;
    replica_config.self_node_id = 2;
    replica_config.ring_epoch = 7;
    replica_config.mode = AgentMemoryRoutingMode::Routed;
    replica_config.virtual_nodes_per_node = 64;

    std::string error;
    ASSERT_TRUE(replica_server.set_agent_memory_persistence(
        replica_dir.string(), &error, 0)) << error;
    ASSERT_TRUE(replica_server.set_agent_memory_routing(
        replica_config, {1, 2}, {}, &error)) << error;

    zeptodb::cluster::TcpRpcServer replica_rpc;
    replica_rpc.set_agent_memory_replica_append_callback(
        [&replica_server](const uint8_t* data, size_t len) {
            return replica_server.handle_agent_memory_replica_append_rpc(data, len);
        });
    const uint16_t rpc_port = zepto_test_util::pick_free_port();
    replica_rpc.start(rpc_port, [](const std::string&) {
        zeptodb::sql::QueryResultSet result;
        result.error = "SQL is not used by this test";
        return result;
    });

    AgentMemoryRouterConfig owner_config;
    owner_config.self_node_id = 1;
    owner_config.ring_epoch = 7;
    owner_config.mode = AgentMemoryRoutingMode::Routed;
    owner_config.virtual_nodes_per_node = 64;

    ASSERT_TRUE(server_->set_agent_memory_persistence(
        owner_dir.string(), &error, 0)) << error;
    server_->set_agent_memory_replication_mode(AgentMemoryReplicationMode::Sync);
    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", rpc_port, 2000));
    ASSERT_TRUE(server_->set_agent_memory_routing(
        owner_config, {1, 2}, std::move(remotes), &error)) << error;

    AgentMemoryRouter probe(owner_config);
    probe.add_node(1);
    probe.add_node(2);
    std::string owner_memory_id;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "sync_replica_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id == 1u) {
            owner_memory_id = candidate;
            break;
        }
    }
    ASSERT_FALSE(owner_memory_id.empty());

    auto put = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        owner_memory_id +
        R"(","content":"sync replicated memory","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;

    EXPECT_TRUE(std::filesystem::exists(owner_dir / "node-1" / "shard-0" /
                                        "wal.log"));
    EXPECT_TRUE(std::filesystem::exists(replica_dir / "node-1" / "shard-0" /
                                        "wal.log"));
    EXPECT_FALSE(replica_server.agent_memory_store()
                     .get_memory(owner_memory_id, "t1")
                     .has_value());

    replica_rpc.stop();
    std::filesystem::remove_all(base);
}

TEST_F(AgentMemoryHttpTest, QuorumReplicationSucceedsWithMajorityAck) {
    const auto base = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_quorum_replica_" + std::to_string(::getpid()) +
         "_" + std::to_string(AgentMemoryStore::now_ns()));
    const auto owner_dir = base / "owner";
    const auto replica_dir = base / "replica";

    auto replica_pipeline = make_pipeline();
    auto replica_executor = std::make_unique<QueryExecutor>(*replica_pipeline);
    zeptodb::server::HttpServer replica_server(
        *replica_executor, zepto_test_util::pick_free_port());

    AgentMemoryRouterConfig replica_config;
    replica_config.self_node_id = 2;
    replica_config.ring_epoch = 7;
    replica_config.mode = AgentMemoryRoutingMode::Routed;
    replica_config.virtual_nodes_per_node = 64;

    std::string error;
    ASSERT_TRUE(replica_server.set_agent_memory_persistence(
        replica_dir.string(), &error, 0)) << error;
    ASSERT_TRUE(replica_server.set_agent_memory_routing(
        replica_config, {1, 2, 3}, {}, &error)) << error;

    zeptodb::cluster::TcpRpcServer replica_rpc;
    replica_rpc.set_agent_memory_replica_append_callback(
        [&replica_server](const uint8_t* data, size_t len) {
            return replica_server.handle_agent_memory_replica_append_rpc(data, len);
        });
    const uint16_t good_rpc_port = zepto_test_util::pick_free_port();
    replica_rpc.start(good_rpc_port, [](const std::string&) {
        zeptodb::sql::QueryResultSet result;
        result.error = "SQL is not used by this test";
        return result;
    });

    AgentMemoryRouterConfig owner_config;
    owner_config.self_node_id = 1;
    owner_config.ring_epoch = 7;
    owner_config.mode = AgentMemoryRoutingMode::Routed;
    owner_config.virtual_nodes_per_node = 64;

    ASSERT_TRUE(server_->set_agent_memory_persistence(
        owner_dir.string(), &error, 0)) << error;
    server_->set_agent_memory_replication_mode(AgentMemoryReplicationMode::Quorum);
    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", good_rpc_port, 2000));
    remotes.emplace(3, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", zepto_test_util::pick_free_port(), 100));
    ASSERT_TRUE(server_->set_agent_memory_routing(
        owner_config, {1, 2, 3}, std::move(remotes), &error)) << error;

    AgentMemoryRouter probe(owner_config);
    probe.add_node(1);
    probe.add_node(2);
    probe.add_node(3);
    std::string owner_memory_id;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "quorum_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id == 1u) {
            owner_memory_id = candidate;
            break;
        }
    }
    ASSERT_FALSE(owner_memory_id.empty());

    auto put = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        owner_memory_id +
        R"(","content":"quorum replicated memory","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_TRUE(std::filesystem::exists(replica_dir / "node-1" / "shard-0" /
                                        "wal.log"));

    replica_rpc.stop();
    std::filesystem::remove_all(base);
}

TEST_F(AgentMemoryHttpTest, SyncReplicationRejectsMissingReplicaAck) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_sync_missing_" + std::to_string(::getpid()) +
         "_" + std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 64;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0))
        << error;
    server_->set_agent_memory_replication_mode(AgentMemoryReplicationMode::Sync);
    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", zepto_test_util::pick_free_port(), 100));
    ASSERT_TRUE(server_->set_agent_memory_routing(
        config, {1, 2}, std::move(remotes), &error)) << error;

    AgentMemoryRouter probe(config);
    probe.add_node(1);
    probe.add_node(2);
    std::string owner_memory_id;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "missing_replica_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id == 1u) {
            owner_memory_id = candidate;
            break;
        }
    }
    ASSERT_FALSE(owner_memory_id.empty());

    auto put = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        owner_memory_id +
        R"(","content":"missing replica","embedding":[1,0]})");
    EXPECT_EQ(put.status, 500) << put.body;
    EXPECT_NE(put.body.find("below required"), std::string::npos);
    EXPECT_FALSE(server_->agent_memory_store()
                     .get_memory(owner_memory_id, "t1")
                     .has_value());

    auto replay_pipeline = make_pipeline();
    auto replay_executor = std::make_unique<QueryExecutor>(*replay_pipeline);
    zeptodb::server::HttpServer replay_server(
        *replay_executor, zepto_test_util::pick_free_port());
    ASSERT_TRUE(replay_server.set_agent_memory_persistence(dir.string(), &error))
        << error;
    ASSERT_TRUE(replay_server.set_agent_memory_routing(config, {1, 2}, {}, &error))
        << error;
    EXPECT_FALSE(replay_server.agent_memory_store()
                     .get_memory(owner_memory_id, "t1")
                     .has_value());

    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, RoutedRemoteWritesCarryRingEpochForFencing) {
    auto remote_pipeline = make_pipeline();
    auto remote_executor = std::make_unique<QueryExecutor>(*remote_pipeline);
    zeptodb::server::HttpServer remote_server(
        *remote_executor, zepto_test_util::pick_free_port());

    zeptodb::cluster::FencingToken fencing;
    zeptodb::cluster::TcpRpcServer remote_rpc;
    remote_rpc.set_fencing_token(&fencing);
    remote_rpc.set_agent_memory_put_callback(
        [&remote_server](const uint8_t* data, size_t len) {
            return remote_server.handle_agent_memory_put_rpc(data, len);
        });
    const uint16_t rpc_port = zepto_test_util::pick_free_port();
    remote_rpc.start(rpc_port, [](const std::string&) {
        zeptodb::sql::QueryResultSet result;
        result.error = "SQL is not used by this test";
        return result;
    });

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 64;

    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", rpc_port, 2000));
    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_routing(
        config, {1, 2}, std::move(remotes), &error)) << error;

    AgentMemoryRouter probe(config);
    probe.add_node(1);
    probe.add_node(2);
    std::string first_remote_id;
    std::string stale_remote_id;
    for (int i = 0; i < 10'000; ++i) {
        const auto candidate = "fenced_mem_" + std::to_string(i);
        const auto key = AgentMemoryRouter::memory_key(
            "t1", "agent", "", "", "", candidate);
        if (probe.route(key).node_id != 2u) continue;
        if (first_remote_id.empty()) {
            first_remote_id = candidate;
        } else {
            stale_remote_id = candidate;
            break;
        }
    }
    ASSERT_FALSE(first_remote_id.empty());
    ASSERT_FALSE(stale_remote_id.empty());

    auto first = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        first_remote_id +
        R"(","content":"current epoch","embedding":[1,0]})");
    ASSERT_EQ(first.status, 200) << first.body;
    EXPECT_EQ(fencing.last_seen(), 7u);

    config.ring_epoch = 6;
    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> stale_remotes;
    stale_remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", rpc_port, 2000));
    ASSERT_TRUE(server_->set_agent_memory_routing(
        config, {1, 2}, std::move(stale_remotes), &error)) << error;

    auto stale = http_post(port_, "/api/ai/memories",
        std::string(R"({"tenant_id":"t1","namespace":"agent","memory_id":")") +
        stale_remote_id +
        R"(","content":"stale epoch","embedding":[1,0]})");
    EXPECT_EQ(stale.status, 400) << stale.body;
    EXPECT_NE(stale.body.find("invalid store result payload"), std::string::npos);
    EXPECT_FALSE(remote_server.agent_memory_store()
                     .get_memory(stale_remote_id, "t1")
                     .has_value());
    EXPECT_EQ(fencing.last_seen(), 7u);

    remote_rpc.stop();
}

TEST_F(AgentMemoryHttpTest, RoutedSearchReturns502WhenRemoteShardUnavailable) {
    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 64;

    std::unordered_map<AgentMemoryNodeId,
                       std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes;
    remotes.emplace(2, std::make_shared<zeptodb::cluster::TcpRpcClient>(
        "127.0.0.1", zepto_test_util::pick_free_port(), 100));
    server_->set_agent_memory_routing(config, {1, 2}, std::move(remotes));

    auto search = http_post(port_, "/api/ai/memories/search",
        R"({"tenant_id":"t1","namespace":"agent","query_embedding":[1,0],)"
        R"("limit":1})");
    ASSERT_EQ(search.status, 502) << search.body;
    EXPECT_NE(search.body.find("agent memory search failed"), std::string::npos);
}

TEST_F(AgentMemoryHttpTest, RejectsMalformedEmbeddingAndTenantMismatch) {
    auto malformed = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","content":"bad","embedding":"oops"})");
    EXPECT_EQ(malformed.status, 400);

    auto mismatch = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"body_tenant","content":"bad","embedding":[1,0]})",
        "header_tenant");
    EXPECT_EQ(mismatch.status, 400);
    EXPECT_NE(mismatch.body.find("Tenant header"), std::string::npos);

    auto negative_limit = http_post(port_, "/api/ai/memories/search",
        R"({"tenant_id":"t1","query_embedding":[1,0],"limit":-1})");
    EXPECT_EQ(negative_limit.status, 400);
    EXPECT_NE(negative_limit.body.find("limit"), std::string::npos);
}

TEST_F(AgentMemoryHttpTest, PersistsHttpMutationAndReloadsSnapshot) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_http_" + std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 1)) << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent","user_id":"u1",)"
        R"("content":"survives restart","embedding":[1,0],"token_count":3})");
    ASSERT_EQ(put.status, 200) << put.body;
    ASSERT_TRUE(std::filesystem::exists(dir / "records.bin"));
    ASSERT_TRUE(std::filesystem::exists(dir / "vectors.bin"));
    EXPECT_FALSE(std::filesystem::exists(dir / "wal.log"));

    server_->stop();
    server_.reset();
    executor_.reset();
    pipeline_.reset();

    pipeline_ = make_pipeline();
    executor_ = std::make_unique<QueryExecutor>(*pipeline_);
    server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error)) << error;

    MemoryQuery q;
    q.tenant_id = "t1";
    q.namespace_id = "agent";
    q.user_id = "u1";
    q.query_embedding = {1.0f, 0.0f};
    auto matches = server_->agent_memory_store().search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.content, "survives restart");

    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, RoutedPersistenceUsesShardLocalPathAndReloadsAfterRouting) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_routed_" + std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 1))
        << error;
    ASSERT_TRUE(server_->set_agent_memory_routing(config, {1}, {}, &error))
        << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"routed shard survives","embedding":[1,0],)"
        R"("token_count":4})");
    ASSERT_EQ(put.status, 200) << put.body;

    const auto shard_dir = dir / "node-1" / "shard-0";
    EXPECT_FALSE(std::filesystem::exists(dir / "records.bin"));
    EXPECT_FALSE(std::filesystem::exists(dir / "vectors.bin"));
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "records.bin"));
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "vectors.bin"));
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "manifest.json"));
    EXPECT_FALSE(std::filesystem::exists(shard_dir / "wal.log"));

    server_->stop();
    server_.reset();
    executor_.reset();
    pipeline_.reset();

    pipeline_ = make_pipeline();
    executor_ = std::make_unique<QueryExecutor>(*pipeline_);
    server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error)) << error;
    ASSERT_TRUE(server_->set_agent_memory_routing(config, {1}, {}, &error)) << error;

    MemoryQuery q;
    q.tenant_id = "t1";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    auto matches = server_->agent_memory_store().search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.content, "routed shard survives");

    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, RoutedPersistenceReplaysWalWithoutSnapshot) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_routed_wal_" + std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0))
        << error;
    ASSERT_TRUE(server_->set_agent_memory_routing(config, {1}, {}, &error))
        << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"wal routed memory","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;

    const auto shard_dir = dir / "node-1" / "shard-0";
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "manifest.json"));
    EXPECT_TRUE(std::filesystem::exists(shard_dir / "wal.log"));
    EXPECT_FALSE(std::filesystem::exists(shard_dir / "records.bin"));
    EXPECT_FALSE(std::filesystem::exists(shard_dir / "vectors.bin"));

    auto replay_pipeline = make_pipeline();
    auto replay_executor = std::make_unique<QueryExecutor>(*replay_pipeline);
    zeptodb::server::HttpServer replay_server(
        *replay_executor, zepto_test_util::pick_free_port());
    ASSERT_TRUE(replay_server.set_agent_memory_persistence(dir.string(), &error))
        << error;
    ASSERT_TRUE(replay_server.set_agent_memory_routing(config, {1}, {}, &error))
        << error;

    MemoryQuery q;
    q.tenant_id = "t1";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    auto matches = replay_server.agent_memory_store().search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.content, "wal routed memory");

    server_->stop();
    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, RejectsRoutedWalWithoutShardManifest) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_missing_wal_manifest_" +
         std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig config;
    config.self_node_id = 1;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0))
        << error;
    ASSERT_TRUE(server_->set_agent_memory_routing(config, {1}, {}, &error))
        << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"wal needs manifest","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;

    const auto shard_dir = dir / "node-1" / "shard-0";
    ASSERT_TRUE(std::filesystem::exists(shard_dir / "wal.log"));
    ASSERT_TRUE(std::filesystem::remove(shard_dir / "manifest.json"));

    auto replay_pipeline = make_pipeline();
    auto replay_executor = std::make_unique<QueryExecutor>(*replay_pipeline);
    zeptodb::server::HttpServer replay_server(
        *replay_executor, zepto_test_util::pick_free_port());
    ASSERT_TRUE(replay_server.set_agent_memory_persistence(dir.string(), &error))
        << error;
    error.clear();
    EXPECT_FALSE(replay_server.set_agent_memory_routing(config, {1}, {}, &error));
    EXPECT_NE(error.find("missing agent memory shard manifest"), std::string::npos)
        << error;

    server_->stop();
    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, RejectsRoutedSnapshotWithWrongShardManifest) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_wrong_shard_" + std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig node1;
    node1.self_node_id = 1;
    node1.ring_epoch = 7;
    node1.mode = AgentMemoryRoutingMode::Routed;
    node1.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_routing(node1, {1}, {}, &error)) << error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 1))
        << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"wrong shard source","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;

    const auto node1_shard = dir / "node-1" / "shard-0";
    const auto node2_shard = dir / "node-2" / "shard-0";
    std::filesystem::create_directories(node2_shard.parent_path());
    std::filesystem::copy(node1_shard, node2_shard,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing);

    server_->stop();
    server_.reset();
    executor_.reset();
    pipeline_.reset();

    pipeline_ = make_pipeline();
    executor_ = std::make_unique<QueryExecutor>(*pipeline_);
    server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error))
        << error;

    AgentMemoryRouterConfig node2 = node1;
    node2.self_node_id = 2;
    error.clear();
    EXPECT_FALSE(server_->set_agent_memory_routing(node2, {2}, {}, &error));
    EXPECT_NE(error.find("manifest does not match"), std::string::npos)
        << error;

    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, AdoptsFailedOwnerShardAndServesOwnerScopedIds) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_adopt_owner_" + std::to_string(::getpid()) +
         "_" + std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig source_config;
    source_config.self_node_id = 1;
    source_config.ring_epoch = 7;
    source_config.mode = AgentMemoryRoutingMode::Routed;
    source_config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0))
        << error;
    ASSERT_TRUE(server_->set_agent_memory_routing(
        source_config, {1}, {}, &error)) << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"adopted owner memory","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;
    const auto id_start = put.body.find("mem_1_7_");
    ASSERT_NE(id_start, std::string::npos) << put.body;
    const auto id_end = put.body.find('"', id_start);
    ASSERT_NE(id_end, std::string::npos) << put.body;
    const std::string memory_id = put.body.substr(id_start, id_end - id_start);
    EXPECT_TRUE(std::filesystem::exists(dir / "node-1" / "shard-0" / "wal.log"));

    auto replacement_pipeline = make_pipeline();
    auto replacement_executor =
        std::make_unique<QueryExecutor>(*replacement_pipeline);
    const uint16_t replacement_port = zepto_test_util::pick_free_port();
    zeptodb::server::HttpServer replacement_server(
        *replacement_executor, replacement_port);

    AgentMemoryRouterConfig replacement_config;
    replacement_config.self_node_id = 2;
    replacement_config.ring_epoch = 8;
    replacement_config.mode = AgentMemoryRoutingMode::Routed;
    replacement_config.virtual_nodes_per_node = 8;

    ASSERT_TRUE(replacement_server.set_agent_memory_persistence(
        dir.string(), &error, 0)) << error;
    ASSERT_TRUE(replacement_server.set_agent_memory_routing(
        replacement_config, {2}, {}, &error)) << error;
    ASSERT_TRUE(replacement_server.adopt_agent_memory_owner_shard(1, 7, &error))
        << error;
    EXPECT_TRUE(std::filesystem::exists(dir / "node-2" / "shard-0" /
                                        "records.bin"));

    replacement_server.start_async();
    std::this_thread::sleep_for(80ms);
    auto get = http_get(replacement_port, "/api/ai/memories/" + memory_id +
                                      "?tenant_id=t1&namespace=agent");
    ASSERT_EQ(get.status, 200) << get.body;
    EXPECT_NE(get.body.find("\"found\":true"), std::string::npos);
    EXPECT_NE(get.body.find("adopted owner memory"), std::string::npos);
    replacement_server.stop();

    server_->stop();
    server_.reset();
    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, OwnerFailoverAdoptsShardOnDeterministicSuccessor) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_auto_failover_" + std::to_string(::getpid()) +
         "_" + std::to_string(AgentMemoryStore::now_ns()));

    AgentMemoryRouterConfig source_config;
    source_config.self_node_id = 1;
    source_config.ring_epoch = 7;
    source_config.mode = AgentMemoryRoutingMode::Routed;
    source_config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0))
        << error;
    ASSERT_TRUE(server_->set_agent_memory_routing(
        source_config, {1}, {}, &error)) << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"automatic failover memory","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;
    const auto id_start = put.body.find("mem_1_7_");
    ASSERT_NE(id_start, std::string::npos) << put.body;
    const auto id_end = put.body.find('"', id_start);
    ASSERT_NE(id_end, std::string::npos) << put.body;
    const std::string memory_id = put.body.substr(id_start, id_end - id_start);

    auto replacement_pipeline = make_pipeline();
    auto replacement_executor =
        std::make_unique<QueryExecutor>(*replacement_pipeline);
    const uint16_t replacement_port = zepto_test_util::pick_free_port();
    zeptodb::server::HttpServer replacement_server(
        *replacement_executor, replacement_port);

    AgentMemoryRouterConfig replacement_config;
    replacement_config.self_node_id = 2;
    replacement_config.ring_epoch = 7;
    replacement_config.mode = AgentMemoryRoutingMode::Routed;
    replacement_config.virtual_nodes_per_node = 8;

    ASSERT_TRUE(replacement_server.set_agent_memory_persistence(
        dir.string(), &error, 1)) << error;
    ASSERT_TRUE(replacement_server.set_agent_memory_routing(
        replacement_config, {2}, {}, &error)) << error;
    replacement_server.start_async();
    std::this_thread::sleep_for(80ms);

    auto warm = http_post(replacement_port, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"replacement warm memory","embedding":[0,1]})");
    ASSERT_EQ(warm.status, 200) << warm.body;
    EXPECT_TRUE(std::filesystem::exists(dir / "node-2" / "shard-0" /
                                        "manifest.json"));

    auto result = replacement_server.handle_agent_memory_owner_failover(
        1, 7, 8, {2});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.adopted);
    EXPECT_EQ(result.replacement_node_id, 2u);

    auto get = http_get(replacement_port, "/api/ai/memories/" + memory_id +
                                      "?tenant_id=t1&namespace=agent");
    ASSERT_EQ(get.status, 200) << get.body;
    EXPECT_NE(get.body.find("\"found\":true"), std::string::npos);
    EXPECT_NE(get.body.find("automatic failover memory"), std::string::npos);

    replacement_server.stop();
    server_->stop();
    server_.reset();
    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, OwnerFailoverSkipsAdoptionOnNonSuccessor) {
    AgentMemoryRouterConfig config;
    config.self_node_id = 3;
    config.ring_epoch = 10;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_routing(
        config, {2, 3}, {}, &error)) << error;

    auto result = server_->handle_agent_memory_owner_failover(
        1, 10, 11, {3, 2, 1, 2});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.adopted);
    EXPECT_EQ(result.replacement_node_id, 2u);
}

TEST_F(AgentMemoryHttpTest, OwnerFailoverRejectsWithoutSurvivors) {
    AgentMemoryRouterConfig config;
    config.self_node_id = 2;
    config.ring_epoch = 7;
    config.mode = AgentMemoryRoutingMode::Routed;
    config.virtual_nodes_per_node = 8;

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_routing(
        config, {2}, {}, &error)) << error;

    auto result = server_->handle_agent_memory_owner_failover(
        1, 7, 8, {1, 0});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("at least one live node"), std::string::npos)
        << result.error;
}

TEST_F(AgentMemoryHttpTest, DefersPersistenceUntilStopWhenFlushEveryZero) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_http_deferred_" + std::to_string(::getpid()) + "_" +
         std::to_string(AgentMemoryStore::now_ns()));

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0)) << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"flush on stop","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;
    auto cache = http_post(port_, "/api/ai/cache/store",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("prompt":"Flush cache prompt","response":"cached answer",)"
        R"("embedding":[1,0]})");
    ASSERT_EQ(cache.status, 200) << cache.body;
    EXPECT_FALSE(std::filesystem::exists(dir / "records.bin"));
    EXPECT_FALSE(std::filesystem::exists(dir / "vectors.bin"));
    EXPECT_TRUE(std::filesystem::exists(dir / "wal.log"));

    auto replay_pipeline = make_pipeline();
    auto replay_executor = std::make_unique<QueryExecutor>(*replay_pipeline);
    zeptodb::server::HttpServer replay_server(
        *replay_executor, zepto_test_util::pick_free_port());
    ASSERT_TRUE(replay_server.set_agent_memory_persistence(dir.string(), &error))
        << error;
    MemoryQuery q;
    q.tenant_id = "t1";
    q.namespace_id = "agent";
    q.query_embedding = {1.0f, 0.0f};
    auto matches = replay_server.agent_memory_store().search(q);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].record.content, "flush on stop");
    CacheLookup lookup;
    lookup.tenant_id = "t1";
    lookup.namespace_id = "agent";
    lookup.prompt = "flush cache prompt";
    const auto hit = replay_server.agent_memory_store().lookup_cache(lookup);
    ASSERT_TRUE(hit.hit);
    EXPECT_TRUE(hit.exact);
    EXPECT_EQ(hit.entry.response, "cached answer");

    server_->stop();
    EXPECT_TRUE(std::filesystem::exists(dir / "records.bin"));
    EXPECT_TRUE(std::filesystem::exists(dir / "vectors.bin"));
    EXPECT_FALSE(std::filesystem::exists(dir / "wal.log"));

    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, WalTombstonesRemoveMemoryAndCacheOnReplay) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("zeptodb_agent_memory_http_tombstone_" + std::to_string(::getpid()) +
         "_" + std::to_string(AgentMemoryStore::now_ns()));

    std::string error;
    ASSERT_TRUE(server_->set_agent_memory_persistence(dir.string(), &error, 0))
        << error;

    auto put = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent","memory_id":"delete_me",)"
        R"("content":"deleted by tombstone","embedding":[1,0]})");
    ASSERT_EQ(put.status, 200) << put.body;
    auto cache = http_post(port_, "/api/ai/cache/store",
        R"({"tenant_id":"t1","namespace":"agent","prompt":"delete_cache",)"
        R"("response":"deleted cache","embedding":[1,0]})");
    ASSERT_EQ(cache.status, 200) << cache.body;

    auto del_memory = http_delete(
        port_, "/api/ai/memories/delete_me?tenant_id=t1&namespace=agent");
    ASSERT_EQ(del_memory.status, 200) << del_memory.body;
    auto del_cache = http_delete(
        port_, "/api/ai/cache?tenant_id=t1&namespace=agent&prompt=delete_cache");
    ASSERT_EQ(del_cache.status, 200) << del_cache.body;
    EXPECT_FALSE(server_->agent_memory_store()
                     .get_memory("delete_me", "t1")
                     .has_value());
    EXPECT_FALSE(server_->agent_memory_store()
                     .get_cache("t1", "agent", "delete_cache")
                     .has_value());
    EXPECT_TRUE(std::filesystem::exists(dir / "wal.log"));

    auto replay_pipeline = make_pipeline();
    auto replay_executor = std::make_unique<QueryExecutor>(*replay_pipeline);
    zeptodb::server::HttpServer replay_server(
        *replay_executor, zepto_test_util::pick_free_port());
    ASSERT_TRUE(replay_server.set_agent_memory_persistence(dir.string(), &error))
        << error;
    EXPECT_FALSE(replay_server.agent_memory_store()
                     .get_memory("delete_me", "t1")
                     .has_value());
    EXPECT_FALSE(replay_server.agent_memory_store()
                     .get_cache("t1", "agent", "delete_cache")
                     .has_value());

    auto missing = http_delete(
        port_, "/api/ai/memories/delete_me?tenant_id=t1&namespace=agent");
    EXPECT_EQ(missing.status, 404) << missing.body;

    server_->stop();
    std::filesystem::remove_all(dir);
}

TEST_F(AgentMemoryHttpTest, ExposesStatsAndPrometheusMetrics) {
    AgentMemoryEvictionConfig config;
    config.max_memories = 1;
    config.max_cache_entries = 1;
    server_->agent_memory_store().set_eviction_config(config);

    auto first = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"first","embedding":[1,0]})");
    ASSERT_EQ(first.status, 200) << first.body;

    auto second = http_post(port_, "/api/ai/memories",
        R"({"tenant_id":"t1","namespace":"agent",)"
        R"("content":"second","embedding":[1,0]})");
    ASSERT_EQ(second.status, 200) << second.body;

    auto stats = http_get(port_, "/api/ai/stats");
    ASSERT_EQ(stats.status, 200) << stats.body;
    EXPECT_NE(stats.body.find("\"memory_count\":1"), std::string::npos);
    EXPECT_NE(stats.body.find("\"cache_count\":0"), std::string::npos);
    EXPECT_NE(stats.body.find("\"embedding_dim\":2"), std::string::npos);
    EXPECT_NE(stats.body.find("\"evicted_memory_count\":1"), std::string::npos);
    EXPECT_NE(stats.body.find("\"max_memories\":1"), std::string::npos);
    EXPECT_EQ(stats.body.find("first"), std::string::npos);
    EXPECT_EQ(stats.body.find("second"), std::string::npos);

    auto metrics = http_get(port_, "/metrics");
    ASSERT_EQ(metrics.status, 200) << metrics.body;
    EXPECT_NE(metrics.body.find("zepto_agent_memory_records 1"), std::string::npos);
    EXPECT_NE(metrics.body.find("zepto_agent_cache_entries 0"), std::string::npos);
    EXPECT_NE(metrics.body.find("zepto_agent_memory_embedding_dim 2"), std::string::npos);
    EXPECT_NE(metrics.body.find("zepto_agent_memory_evictions_total 1"), std::string::npos);
    EXPECT_NE(metrics.body.find("zepto_agent_memory_max_records 1"), std::string::npos);
    EXPECT_NE(metrics.body.find("zepto_agent_cache_max_entries 1"), std::string::npos);
    EXPECT_EQ(metrics.body.find("second"), std::string::npos);
}
