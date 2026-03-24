#pragma once
// ============================================================================
// ZeptoDB: Multi-Tenancy — Tenant Manager
// ============================================================================
// Resource isolation per tenant: query concurrency, memory, rate limits.
// Integrates with AuthContext (tenant_id) and QueryExecutor.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::auth {

// ============================================================================
// TenantConfig: per-tenant resource quotas
// ============================================================================
struct TenantConfig {
    std::string tenant_id;
    std::string name;                       // display name

    // Resource quotas
    uint32_t    max_concurrent_queries = 10;  // 0 = unlimited
    uint64_t    max_memory_bytes       = 0;   // 0 = unlimited
    uint32_t    max_queries_per_minute = 0;   // 0 = use global rate limit
    uint32_t    max_ingestion_rate     = 0;   // ticks/sec, 0 = unlimited

    // Table namespace: if non-empty, tenant can only access tables
    // prefixed with this namespace (e.g. "tenant_a." → "tenant_a.trades")
    std::string table_namespace;

    // Priority: lower = higher priority for query scheduling
    uint8_t     priority = 5;               // 1 (highest) to 10 (lowest)
};

// ============================================================================
// TenantUsage: runtime usage tracking per tenant
// ============================================================================
struct TenantUsage {
    std::atomic<uint32_t> active_queries{0};
    std::atomic<uint64_t> total_queries{0};
    std::atomic<uint64_t> total_ticks_ingested{0};
    std::atomic<uint64_t> rejected_queries{0};  // over quota
};

// ============================================================================
// TenantManager: CRUD + quota enforcement
// ============================================================================
class TenantManager {
public:
    /// Create a tenant. Returns false if tenant_id already exists.
    bool create_tenant(TenantConfig config);

    /// Remove a tenant. Returns false if not found.
    bool drop_tenant(const std::string& tenant_id);

    /// Get tenant config. Returns nullopt if not found.
    std::optional<TenantConfig> get_tenant(const std::string& tenant_id) const;

    /// List all tenants.
    std::vector<TenantConfig> list_tenants() const;

    /// Check if a query can proceed (concurrency quota).
    /// Returns true and increments active_queries if allowed.
    bool acquire_query_slot(const std::string& tenant_id);

    /// Release a query slot when query completes.
    void release_query_slot(const std::string& tenant_id);

    /// Check if a table name is accessible by the tenant.
    /// If tenant has a namespace, table must be prefixed or be the namespace itself.
    bool can_access_table(const std::string& tenant_id,
                          const std::string& table_name) const;

    /// Get usage stats for a tenant.
    const TenantUsage* usage(const std::string& tenant_id) const;

    /// Check if any tenants are configured.
    bool has_tenants() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, TenantConfig> tenants_;
    std::unordered_map<std::string, TenantUsage>  usage_;
};

} // namespace zeptodb::auth
