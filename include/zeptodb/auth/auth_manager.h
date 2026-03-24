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
#include "zeptodb/auth/rate_limiter.h"
#include "zeptodb/auth/audit_buffer.h"
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

        // JWT / SSO settings
        bool                jwt_enabled = false;
        JwtValidator::Config jwt;

        // Rate limiting
        bool                rate_limit_enabled = true;
        RateLimiter::Config rate_limit;

        // Audit log
        bool        audit_enabled  = true;
        std::string audit_log_file;  // empty = log to stderr via spdlog

        // In-memory audit ring buffer (capacity 10,000 events)
        bool        audit_buffer_enabled = true;

        // Paths that never require authentication
        std::vector<std::string> public_paths = {"/ping", "/health", "/ready"};
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
                                const std::vector<std::string>& symbols = {});

    bool revoke_api_key(const std::string& key_id);

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

private:
    Config                         config_;
    std::unique_ptr<ApiKeyStore>   key_store_;
    std::unique_ptr<JwtValidator>  jwt_validator_;
    std::unique_ptr<RateLimiter>   rate_limiter_;
    mutable AuditBuffer            audit_buffer_;

    AuthDecision check_api_key(const std::string& token) const;
    AuthDecision check_jwt(const std::string& token) const;

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
