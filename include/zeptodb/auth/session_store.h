#pragma once
// ============================================================================
// ZeptoDB: Server-Side Session Store
// ============================================================================
// Cookie-based session management for Web UI SSO login flow.
//
// After OAuth2 callback or JWT login, the server creates a session and
// returns a Set-Cookie header. Subsequent requests use the cookie instead
// of sending the raw token on every request.
//
// Sessions have a configurable TTL and can be refreshed on activity.
// ============================================================================

#include "zeptodb/auth/rbac.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::auth {

// Forward declare — full definition in auth_manager.h
struct AuthContext;

struct Session {
    std::string  session_id;
    std::string  subject;
    std::string  name;
    Role         role = Role::READER;
    std::string  source;
    std::vector<std::string> allowed_symbols;
    std::string  tenant_id;
    std::string  refresh_token;   // OAuth2 refresh token (if available)
    int64_t      created_at_ns  = 0;
    int64_t      expires_at_ns  = 0;
    int64_t      last_active_ns = 0;
};

class SessionStore {
public:
    struct Config {
        int64_t session_ttl_s    = 3600;
        int64_t refresh_window_s = 300;
        size_t  max_sessions     = 10000;
        std::string cookie_name  = "zepto_sid";
        bool    cookie_secure    = false;
        bool    cookie_httponly  = true;
    };

    SessionStore();
    explicit SessionStore(Config config);

    /// Create a new session. Returns the session ID (used as cookie value).
    std::string create(const std::string& subject, const std::string& name,
                       Role role, const std::string& source,
                       const std::vector<std::string>& allowed_symbols = {},
                       const std::string& tenant_id = "",
                       const std::string& refresh_token = "");

    /// Lookup session by ID. Returns nullopt if expired or not found.
    /// Extends session TTL if within refresh window.
    std::optional<Session> get(const std::string& session_id);

    /// Destroy a session (logout).
    bool destroy(const std::string& session_id);

    /// Update the refresh token for a session.
    bool update_refresh_token(const std::string& session_id,
                              const std::string& new_refresh_token);

    /// Number of active sessions.
    size_t size() const;

    /// Evict all expired sessions. Returns count evicted.
    size_t evict_expired();

    /// Build Set-Cookie header value for a session.
    std::string make_cookie(const std::string& session_id) const;

    /// Build Set-Cookie header to clear the cookie (logout).
    std::string make_clear_cookie() const;

    const Config& config() const { return config_; }

private:
    Config config_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Session> sessions_;

    static std::string generate_id();
    int64_t now_ns() const;
};

} // namespace zeptodb::auth
