// ============================================================================
// ZeptoDB: Multi-Tenancy — Tenant Manager Implementation
// ============================================================================

#include "zeptodb/auth/tenant_manager.h"

namespace zeptodb::auth {

bool TenantManager::create_tenant(TenantConfig config) {
    std::lock_guard lk(mu_);
    if (tenants_.count(config.tenant_id)) return false;
    const std::string id = config.tenant_id;
    tenants_[id] = std::move(config);
    usage_[id];  // default-construct TenantUsage
    return true;
}

bool TenantManager::drop_tenant(const std::string& tenant_id) {
    std::lock_guard lk(mu_);
    usage_.erase(tenant_id);
    return tenants_.erase(tenant_id) > 0;
}

std::optional<TenantConfig> TenantManager::get_tenant(const std::string& tenant_id) const {
    std::lock_guard lk(mu_);
    auto it = tenants_.find(tenant_id);
    if (it == tenants_.end()) return std::nullopt;
    return it->second;
}

std::vector<TenantConfig> TenantManager::list_tenants() const {
    std::lock_guard lk(mu_);
    std::vector<TenantConfig> result;
    result.reserve(tenants_.size());
    for (auto& [_, t] : tenants_) result.push_back(t);
    return result;
}

bool TenantManager::acquire_query_slot(const std::string& tenant_id) {
    std::lock_guard lk(mu_);
    auto tc = tenants_.find(tenant_id);
    if (tc == tenants_.end()) return true;  // unknown tenant = no quota

    auto& u = usage_[tenant_id];
    if (tc->second.max_concurrent_queries > 0 &&
        u.active_queries.load() >= tc->second.max_concurrent_queries) {
        u.rejected_queries.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    u.active_queries.fetch_add(1, std::memory_order_relaxed);
    u.total_queries.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TenantManager::release_query_slot(const std::string& tenant_id) {
    std::lock_guard lk(mu_);
    auto it = usage_.find(tenant_id);
    if (it != usage_.end()) {
        auto cur = it->second.active_queries.load();
        if (cur > 0) it->second.active_queries.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool TenantManager::can_access_table(const std::string& tenant_id,
                                      const std::string& table_name) const {
    std::lock_guard lk(mu_);
    auto it = tenants_.find(tenant_id);
    if (it == tenants_.end()) return true;  // unknown tenant = unrestricted

    const auto& ns = it->second.table_namespace;
    if (ns.empty()) return true;  // no namespace = unrestricted

    // Table must start with namespace prefix
    return table_name.rfind(ns, 0) == 0;
}

const TenantUsage* TenantManager::usage(const std::string& tenant_id) const {
    std::lock_guard lk(mu_);
    auto it = usage_.find(tenant_id);
    return (it != usage_.end()) ? &it->second : nullptr;
}

bool TenantManager::has_tenants() const {
    std::lock_guard lk(mu_);
    return !tenants_.empty();
}

} // namespace zeptodb::auth
