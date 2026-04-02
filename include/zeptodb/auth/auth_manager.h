#pragma once
// ============================================================================
// ZeptoDB: AuthManager — Central Authentication & Authorization Gateway
// ============================================================================
// Integrates API key auth, JWT/SSO, RBAC, rate limiting, and audit logging.
//
// Usage in HttpServer:
//   auto decision = auth->check(method, path, auth_header, remote_addr);
//   if (decision.status != AuthStatus::OK) { return 401/403; }
//   if (!decision.context.has_permission(Permission::READ)) { return 403; }
//
// Auth priority:  JWT Bearer > API Key Bearer > unauthenticated
// Public paths:   /ping /health /ready  — always allowed (no auth required)
// ============================================================================

#include "zeptodb/auth/rbac.h"
#include "zeptodb/auth/api_key_store.h"
#include "zeptodb/auth/jwt_validator.h"
#include "zeptodb/auth/jwks_provider.h"
#include "zeptodb/auth/rate_limiter.h"
#include "zeptodb/auth/audit_buffer.h"
#include "zeptodb/auth/vault_key_backend.h"
#include "zeptodb/auth/sso_identity_provider.h"
#include "zeptodb/auth/oidc_discovery.h"
#include "zeptodb/auth/session_store.h"
#include "zeptodb/auth/oauth2_token_exchange.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace zeptodb::auth {

// ============================================================================
// AuthContext — resolved identity after successful authentication
// ============================================================================
struct AuthContext {
    std::string              subject;          // user id or key id
    std::string              name;             // display name
    Role                     role = Role::READER;
    std::string              source;           // "api_key" | "jwt" | "anonymous"
    std::vector<std::string> allowed_symbols;  // empty = unrestricted
    std::string              tenant_id;        // empty = no tenant (unrestricted)
    std::vector<std::string> allowed_tables;   // empty = unrestricted

    bool has_permission(Permission p) const {
        return role_permissions(role) & p;
    }

    // Returns true if the symbol is accessible to this identity.
    // When allowed_symbols is empty, all symbols are accessible.
    bool can_access_symbol(const std::string& sym) const {
        if (allowed_symbols.empty()) return true;
        for (const auto& s : allowed_symbols)
            if (s == sym) return true;
        return false;
    }

    bool can_access_table(const std::string& table) const {
        if (allowed_tables.empty()) return true;
        for (const auto& t : allowed_tables)
            if (t == table) return true;
        return false;
    }
};

// ============================================================================
// AuthStatus / AuthDecision
// ============================================================================
enum class AuthStatus {
    OK,            // Authenticated and authorized
    UNAUTHORIZED,  // No credentials or invalid credentials  → 401
    FORBIDDEN,     // Valid credentials but insufficient role → 403
};

struct AuthDecision {
    AuthStatus  status = AuthStatus::UNAUTHORIZED;
    AuthContext context;
    std::string reason;
};

// ============================================================================
// AuthManager
// ============================================================================
class AuthManager {
public:
    struct Config {
        bool enabled = true;   // Set false to bypass all auth (dev mode)

        // API Key settings
        std::string api_keys_file;  // path to key store file

        // Vault-backed API Key Store (optional, write-through sync)
        bool                     vault_keys_enabled = false;
        VaultKeyBackend::Config  vault_keys;

        // JWT / SSO settings
        bool                jwt_enabled = false;
        JwtValidator::Config jwt;
        std::string         jwks_url;   // JWKS endpoint for auto-fetch RS256 keys

        // Rate limiting
        bool                rate_limit_enabled = true;
        RateLimiter::Config rate_limit;

        // SSO Identity Provider (multi-IdP support)
        bool                          sso_enabled = false;
        SsoIdentityProvider::Config   sso;
        std::vector<IdpConfig>        sso_idps;  // IdPs to register on startup

        // OIDC Discovery: auto-configure IdP from issuer URL
        // When set, fetches /.well-known/openid-configuration to populate
        // jwks_uri, authorization_endpoint, token_endpoint automatically.
        std::string oidc_issuer;       // e.g. "https://accounts.google.com"
        std::string oidc_client_id;
        std::string oidc_client_secret;
        std::string oidc_redirect_uri; // e.g. "http://localhost:8123/auth/callback"
        std::string oidc_audience;     // optional audience override

        // Server-side session store
        bool                  sessions_enabled = false;
        SessionStore::Config  session_config;

        // Audit log
        bool        audit_enabled  = true;
        std::string audit_log_file;  // empty = log to stderr via spdlog

        // In-memory audit ring buffer (capacity 10,000 events)
        bool        audit_buffer_enabled = true;

        // Paths that never require authentication
        std::vector<std::string> public_paths = {
            "/ping", "/health", "/ready",
            "/auth/login", "/auth/callback", "/auth/logout"
        };
    };

    explicit AuthManager(Config config);

    // ---------------------------------------------------------------------------
    // check — primary entry point called by HttpServer pre-routing handler
    // ---------------------------------------------------------------------------
    // Extracts credentials from Authorization header, validates them, and
    // returns a decision. Public paths always return OK with anonymous context.
    AuthDecision check(const std::string& method,
                       const std::string& path,
                       const std::string& auth_header,
                       const std::string& remote_addr = "") const;

    // ---------------------------------------------------------------------------
    // Admin API: manage keys
    // ---------------------------------------------------------------------------
    // Returns the full plaintext key (shown once).
    std::string create_api_key(const std::string& name,
                                Role role,
                                const std::vector<std::string>& symbols = {},
                                const std::vector<std::string>& tables = {},
                                const std::string& tenant_id = "",
                                int64_t expires_at_ns = 0);

    bool revoke_api_key(const std::string& key_id);

    // Update mutable fields of an existing key.
    bool update_api_key(const std::string& key_id,
                        const std::optional<std::vector<std::string>>& symbols,
                        const std::optional<std::vector<std::string>>& tables,
                        const std::optional<bool>& enabled,
                        const std::optional<std::string>& tenant_id,
                        const std::optional<int64_t>& expires_at_ns);

    std::vector<ApiKeyEntry> list_api_keys() const;

    // ---------------------------------------------------------------------------
    // Audit
    // ---------------------------------------------------------------------------
    void audit(const AuthContext& ctx,
               const std::string& action,
               const std::string& detail,
               const std::string& remote_addr = "") const;

    // Access to the in-memory audit ring buffer (for /admin/audit endpoint)
    const AuditBuffer& audit_buffer() const { return audit_buffer_; }

    // ---------------------------------------------------------------------------
    // JWKS
    // ---------------------------------------------------------------------------
    /// Force refresh JWKS keys. Returns true if keys were loaded.
    bool refresh_jwks();

    /// Access JWKS provider (may be null if not configured).
    JwksProvider* jwks_provider() { return jwks_provider_.get(); }

    /// Access SSO identity provider (may be null if not configured).
    SsoIdentityProvider* sso_provider() { return sso_provider_.get(); }

    /// Access session store (may be null if not configured).
    SessionStore* session_store() { return session_store_.get(); }

    /// Access OIDC metadata (populated after discovery).
    const OidcMetadata* oidc_metadata() const { return oidc_meta_ ? &*oidc_meta_ : nullptr; }

    /// Get OIDC client_id (for building authorize URL in HTTP handler).
    const std::string& oidc_client_id() const { return config_.oidc_client_id; }
    const std::string& oidc_client_secret() const { return config_.oidc_client_secret; }
    const std::string& oidc_redirect_uri() const { return config_.oidc_redirect_uri; }

    /// Authenticate via session cookie. Returns nullopt if no valid session.
    std::optional<AuthContext> check_session(const std::string& cookie_header) const;

private:
    Config                         config_;
    std::unique_ptr<ApiKeyStore>   key_store_;
    std::unique_ptr<JwtValidator>  jwt_validator_;
    std::unique_ptr<JwksProvider>  jwks_provider_;
    std::unique_ptr<RateLimiter>   rate_limiter_;
    std::unique_ptr<SsoIdentityProvider> sso_provider_;
    std::unique_ptr<SessionStore>  session_store_;
    std::optional<OidcMetadata>    oidc_meta_;
    mutable AuditBuffer            audit_buffer_;

    AuthDecision check_api_key(const std::string& token) const;
    AuthDecision check_jwt(const std::string& token) const;
    AuthDecision check_sso(const std::string& token) const;

    static std::string extract_bearer_token(const std::string& auth_header);
    bool is_public_path(const std::string& path) const;
};

// ============================================================================
// TlsConfig — passed to HttpServer to enable HTTPS
// ============================================================================
struct TlsConfig {
    bool        enabled   = false;
    std::string cert_path;       // path to server certificate (PEM)
    std::string key_path;        // path to private key (PEM)
    std::string ca_cert_path;    // optional: CA cert for mTLS client verification
    uint16_t    https_port = 8443;
    bool        also_serve_http = true;  // keep HTTP on original port when true
};

} // namespace zeptodb::auth
