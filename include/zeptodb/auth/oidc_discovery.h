#pragma once
// ============================================================================
// ZeptoDB: OIDC Discovery — Auto-configure IdP from issuer URL
// ============================================================================
// Fetches /.well-known/openid-configuration and extracts:
//   - jwks_uri
//   - issuer
//   - authorization_endpoint
//   - token_endpoint
//
// Usage:
//   auto meta = OidcDiscovery::fetch("https://accounts.google.com");
//   if (meta) { /* use meta->jwks_uri, meta->authorization_endpoint, etc. */ }
// ============================================================================

#include <optional>
#include <string>

namespace zeptodb::auth {

struct OidcMetadata {
    std::string issuer;
    std::string jwks_uri;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string userinfo_endpoint;
};

class OidcDiscovery {
public:
    /// Fetch OIDC metadata from issuer_url/.well-known/openid-configuration.
    /// Returns nullopt on network error or missing required fields.
    static std::optional<OidcMetadata> fetch(const std::string& issuer_url);

    /// Build an IdpConfig from OIDC discovery + minimal user input.
    /// Caller still needs to set group_role_map and other policy fields.
    static std::string extract_json_string(const std::string& json,
                                           const std::string& key);
};

} // namespace zeptodb::auth
