#pragma once
// ============================================================================
// ZeptoDB: Role-Based Access Control (RBAC) Definitions
// ============================================================================
// Roles, permissions, and helper utilities.
// Header-only — no external dependencies.
// ============================================================================

#include <cstdint>
#include <string_view>

namespace zeptodb::auth {

// ============================================================================
// Role — what type of user/service this identity is
// ============================================================================
enum class Role : uint8_t {
    ADMIN       = 0,  // Full access: DDL, queries, user management
    WRITER      = 1,  // Read + write (SQL + ingest API)
    READER      = 2,  // SELECT queries only
    ANALYST     = 3,  // SELECT, restricted to allowed_symbols whitelist
    METRICS     = 4,  // /metrics /health /stats endpoints only (Prometheus scraper)
    UNKNOWN     = 255,
};

// ============================================================================
// Permission — individual capability bits (bitmask)
// ============================================================================
enum class Permission : uint32_t {
    NONE    = 0,
    READ    = 1u << 0,  // Execute SELECT queries
    WRITE   = 1u << 1,  // INSERT / ingest API
    ADMIN   = 1u << 2,  // DDL, user management endpoints
    METRICS = 1u << 3,  // /metrics /stats /health endpoints
    ALL     = 0xFFFFFFFFu,
};

inline Permission operator|(Permission a, Permission b) {
    return static_cast<Permission>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(Permission a, Permission b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// ============================================================================
// role_permissions — canonical mapping from role to permission bitmask
// ============================================================================
inline Permission role_permissions(Role r) {
    switch (r) {
        case Role::ADMIN:
            return Permission::ALL;
        case Role::WRITER:
            return Permission::READ | Permission::WRITE | Permission::METRICS;
        case Role::READER:
            return Permission::READ;
        case Role::ANALYST:
            return Permission::READ;
        case Role::METRICS:
            return Permission::METRICS;
        default:
            return Permission::NONE;
    }
}

// ============================================================================
// Serialization helpers
// ============================================================================
inline std::string_view role_to_string(Role r) {
    switch (r) {
        case Role::ADMIN:   return "admin";
        case Role::WRITER:  return "writer";
        case Role::READER:  return "reader";
        case Role::ANALYST: return "analyst";
        case Role::METRICS: return "metrics";
        default:            return "unknown";
    }
}

inline Role role_from_string(std::string_view s) {
    if (s == "admin")   return Role::ADMIN;
    if (s == "writer")  return Role::WRITER;
    if (s == "reader")  return Role::READER;
    if (s == "analyst") return Role::ANALYST;
    if (s == "metrics") return Role::METRICS;
    return Role::UNKNOWN;
}

} // namespace zeptodb::auth
