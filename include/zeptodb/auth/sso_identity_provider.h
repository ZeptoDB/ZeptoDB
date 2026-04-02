#pragma once
// ============================================================================
// ZeptoDB: SSO Identity Provider — Multi-IdP Identity Resolution
// ============================================================================
// Resolves JWT tokens from multiple OIDC identity providers into a unified
// SsoIdentity. Supports issuer-based routing, group-to-role mapping, and
// an in-memory identity cache with TTL.
//
// Usage:
//   SsoIdentityProvider provider;
//   provider.add_idp(okta_config);
//   provider.add_idp(azure_config);
//   auto identity = provider.resolve(jwt_token);
//   if (identity) { /* use identity->role, identity->subject, etc. */ }
// ============================================================================

#include "zeptodb/auth/rbac.h"
#include "zeptodb/auth/jwt_validator.h"
#include "zeptodb/auth/jwks_provider.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::auth {

// ============================================================================
// IdpConfig — per-provider configuration
// ============================================================================
struct IdpConfig {
    std::string id;           // unique IdP identifier (e.g. "okta-prod")
    std::string issuer;       // expected "iss" claim
    std::string jwks_url;     // JWKS endpoint for RS256 key auto-fetch
    std::string audience;     // expected "aud" claim (optional)

    // Group-based role mapping
    std::string group_claim = "groups";  // JWT claim containing group list
    std::unordered_map<std::string, Role> group_role_map;  // group → role
    Role default_role = Role::READER;

    // Optional claim mappings
    std::string tenant_claim;                  // JWT claim for tenant_id
    std::string symbols_claim = "zepto_symbols"; // JWT claim for symbol whitelist
};

// ============================================================================
// SsoIdentity — resolved identity from an IdP token
// ============================================================================
struct SsoIdentity {
    std::string              subject;
    std::string              email;
    std::string              idp_id;
    std::vector<std::string> groups;
    Role                     role = Role::READER;
    std::vector<std::string> allowed_symbols;
    std::string              tenant_id;
};

// ============================================================================
// SsoIdentityProvider
// ============================================================================
class SsoIdentityProvider {
public:
    struct Config {
        int64_t cache_ttl_s     = 300;    // identity cache TTL (seconds)
        size_t  cache_capacity  = 10000;  // max cached identities
    };

    SsoIdentityProvider();
    explicit SsoIdentityProvider(Config config);

    /// Register an identity provider. Returns false if id already exists.
    bool add_idp(IdpConfig idp);

    /// Remove an identity provider by id.
    bool remove_idp(const std::string& idp_id);

    /// List registered IdP ids.
    std::vector<std::string> list_idps() const;

    /// Resolve a JWT token to an SsoIdentity.
    /// Returns nullopt if no registered IdP matches the token's issuer,
    /// or if validation fails.
    std::optional<SsoIdentity> resolve(const std::string& jwt_token) const;

    /// Clear the identity cache.
    void clear_cache();

    /// Number of cached identities.
    size_t cache_size() const;

    // JSON helpers (public for testing, same pattern as JwtValidator::base64url_decode)
    static std::string extract_json_string(const std::string& json,
                                           const std::string& key);
    static std::vector<std::string> extract_json_string_array(
        const std::string& json, const std::string& key);

private:
    Config config_;

    // Registered IdPs
    struct IdpRuntime {
        IdpConfig                       config;
        std::unique_ptr<JwtValidator>   validator;
        std::unique_ptr<JwksProvider>   jwks;
    };
    mutable std::mutex                                   mu_;
    std::unordered_map<std::string, IdpRuntime>          idps_;       // id → runtime
    std::unordered_map<std::string, std::string>         issuer_map_; // issuer → id

    // Identity cache
    struct CacheEntry {
        SsoIdentity identity;
        int64_t     expires_at_ns;
    };
    mutable std::mutex                                   cache_mu_;
    mutable std::unordered_map<std::string, CacheEntry>  cache_; // "idp_id:sub" → entry

    // Helpers
    std::optional<SsoIdentity> resolve_with_idp(const IdpRuntime& idp,
                                                 const std::string& token) const;
    Role resolve_role(const IdpConfig& cfg,
                      const std::vector<std::string>& groups) const;
    static std::string extract_issuer_from_jwt(const std::string& token);
    static std::string cache_key(const std::string& idp_id, const std::string& sub);
    std::optional<SsoIdentity> lookup_cache(const std::string& key) const;
    void store_cache(const std::string& key, const SsoIdentity& identity) const;
};

} // namespace zeptodb::auth
