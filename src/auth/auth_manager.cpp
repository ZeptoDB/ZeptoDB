// ============================================================================
// ZeptoDB: AuthManager Implementation
// ============================================================================
#include "zeptodb/auth/auth_manager.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <chrono>

namespace zeptodb::auth {

namespace {

// Shared audit logger (initialized once)
std::shared_ptr<spdlog::logger> get_audit_logger(const std::string& log_file) {
    static std::shared_ptr<spdlog::logger> logger;
    static std::once_flag flag;
    std::call_once(flag, [&]() {
        if (!log_file.empty()) {
            logger = spdlog::basic_logger_mt("zepto_audit", log_file, true);
        } else {
            logger = spdlog::default_logger();
        }
        logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [AUDIT] %v");
    });
    return logger;
}

} // anonymous namespace

// ============================================================================
// Constructor
// ============================================================================
AuthManager::AuthManager(Config config)
    : config_(std::move(config))
{
    if (!config_.api_keys_file.empty()) {
        if (config_.vault_keys_enabled) {
            auto vault = std::make_unique<VaultKeyBackend>(config_.vault_keys);
            key_store_ = std::make_unique<ApiKeyStore>(
                config_.api_keys_file, std::move(vault));
        } else {
            key_store_ = std::make_unique<ApiKeyStore>(config_.api_keys_file);
        }
    }

    if (config_.jwt_enabled) {
        jwt_validator_ = std::make_unique<JwtValidator>(config_.jwt);

        // JWKS auto-fetch: create provider and wire key resolver
        if (!config_.jwks_url.empty()) {
            jwks_provider_ = std::make_unique<JwksProvider>(config_.jwks_url);
            jwks_provider_->refresh();  // initial synchronous fetch
            jwt_validator_->set_key_resolver([this](const std::string& kid) -> std::string {
                if (!jwks_provider_) return "";
                if (!kid.empty()) return jwks_provider_->get_pem(kid);
                return jwks_provider_->get_default_pem();
            });
            jwks_provider_->start();  // background refresh
        }
    }

    if (config_.rate_limit_enabled) {
        rate_limiter_ = std::make_unique<RateLimiter>(config_.rate_limit);
    }

    if (config_.sso_enabled) {
        sso_provider_ = std::make_unique<SsoIdentityProvider>(config_.sso);
        for (auto& idp : config_.sso_idps)
            sso_provider_->add_idp(std::move(idp));
    }

    // OIDC Discovery: auto-configure from issuer URL
    if (!config_.oidc_issuer.empty()) {
        oidc_meta_ = OidcDiscovery::fetch(config_.oidc_issuer);
        if (oidc_meta_) {
            // Auto-register as SSO IdP if SSO provider exists
            if (!sso_provider_) {
                config_.sso_enabled = true;
                sso_provider_ = std::make_unique<SsoIdentityProvider>(config_.sso);
            }
            IdpConfig oidc_idp;
            oidc_idp.id       = "oidc-auto";
            oidc_idp.issuer   = oidc_meta_->issuer;
            oidc_idp.jwks_url = oidc_meta_->jwks_uri;
            oidc_idp.audience = config_.oidc_audience;
            sso_provider_->add_idp(std::move(oidc_idp));

            // Also configure JWT validator with JWKS if not already set
            if (!jwt_validator_ && !oidc_meta_->jwks_uri.empty()) {
                JwtValidator::Config jwt_cfg;
                jwt_cfg.expected_issuer   = oidc_meta_->issuer;
                jwt_cfg.expected_audience = config_.oidc_audience;
                jwt_validator_ = std::make_unique<JwtValidator>(jwt_cfg);
                jwks_provider_ = std::make_unique<JwksProvider>(oidc_meta_->jwks_uri);
                jwks_provider_->refresh();
                jwt_validator_->set_key_resolver([this](const std::string& kid) -> std::string {
                    if (!jwks_provider_) return "";
                    if (!kid.empty()) return jwks_provider_->get_pem(kid);
                    return jwks_provider_->get_default_pem();
                });
                jwks_provider_->start();
                config_.jwt_enabled = true;
            }
        }
    }

    // Server-side session store
    if (config_.sessions_enabled) {
        session_store_ = std::make_unique<SessionStore>(config_.session_config);
    }
}

// ============================================================================
// check
// ============================================================================
AuthDecision AuthManager::check(const std::string& method,
                                 const std::string& path,
                                 const std::string& auth_header,
                                 const std::string& remote_addr) const
{
    // Auth disabled → allow everything as anonymous admin
    if (!config_.enabled) {
        AuthContext ctx;
        ctx.subject = "anonymous";
        ctx.name    = "anonymous";
        ctx.role    = Role::ADMIN;
        ctx.source  = "disabled";
        return {AuthStatus::OK, ctx, ""};
    }

    // Public paths → allow without credentials
    if (is_public_path(path)) {
        AuthContext ctx;
        ctx.subject = "anonymous";
        ctx.name    = "anonymous";
        ctx.role    = Role::METRICS;
        ctx.source  = "public";
        return {AuthStatus::OK, ctx, ""};
    }

    // No auth header → check session cookie before rejecting
    if (auth_header.empty()) {
        return {AuthStatus::UNAUTHORIZED, {}, "No Authorization header"};
    }

    std::string token = extract_bearer_token(auth_header);
    if (token.empty()) {
        return {AuthStatus::UNAUTHORIZED, {}, "Invalid Authorization header format"};
    }

    // Try SSO identity provider first (multi-IdP, issuer-routed)
    if (sso_provider_ && token.size() > 2 &&
        token[0] == 'e' && token[1] == 'y') {
        auto decision = check_sso(token);
        if (decision.status == AuthStatus::OK) {
            if (rate_limiter_) {
                auto rd = rate_limiter_->check_identity(decision.context.subject);
                if (rd == RateDecision::RATE_LIMITED)
                    return {AuthStatus::FORBIDDEN, {}, "Rate limit exceeded"};
                rate_limiter_->check_ip(remote_addr);
            }
            if (config_.audit_enabled)
                audit(decision.context, method + " " + path, "sso-auth", remote_addr);
            return decision;
        }
    }

    // Try JWT first (Bearer tokens starting with "ey" are JWTs)
    if (config_.jwt_enabled && jwt_validator_ && token.size() > 2 &&
        token[0] == 'e' && token[1] == 'y') {
        auto decision = check_jwt(token);
        if (decision.status == AuthStatus::OK) {
            // Rate limit check
            if (rate_limiter_) {
                auto rd = rate_limiter_->check_identity(decision.context.subject);
                if (rd == RateDecision::RATE_LIMITED) {
                    return {AuthStatus::FORBIDDEN, {}, "Rate limit exceeded"};
                }
                rate_limiter_->check_ip(remote_addr);
            }
            if (config_.audit_enabled)
                audit(decision.context, method + " " + path, "jwt-auth", remote_addr);
            return decision;
        }
    }

    // Try API key
    if (key_store_) {
        auto decision = check_api_key(token);
        if (decision.status == AuthStatus::OK) {
            // Rate limit check
            if (rate_limiter_) {
                auto rd = rate_limiter_->check_identity(decision.context.subject);
                if (rd == RateDecision::RATE_LIMITED) {
                    return {AuthStatus::FORBIDDEN, {}, "Rate limit exceeded"};
                }
                rate_limiter_->check_ip(remote_addr);
            }
            if (config_.audit_enabled)
                audit(decision.context, method + " " + path, "apikey-auth", remote_addr);
            return decision;
        }
        return decision;  // propagate the failure reason
    }

    return {AuthStatus::UNAUTHORIZED, {}, "No authentication method configured"};
}

// ============================================================================
// check_api_key
// ============================================================================
AuthDecision AuthManager::check_api_key(const std::string& token) const {
    auto entry = key_store_->validate(token);
    if (!entry) {
        return {AuthStatus::UNAUTHORIZED, {}, "Invalid API key"};
    }

    AuthContext ctx;
    ctx.subject         = entry->id;
    ctx.name            = entry->name;
    ctx.role            = entry->role;
    ctx.source          = "api_key";
    ctx.allowed_symbols = entry->allowed_symbols;
    ctx.allowed_tables  = entry->allowed_tables;

    return {AuthStatus::OK, ctx, ""};
}

// ============================================================================
// check_sso
// ============================================================================
AuthDecision AuthManager::check_sso(const std::string& token) const {
    auto identity = sso_provider_->resolve(token);
    if (!identity) return {AuthStatus::UNAUTHORIZED, {}, "SSO: no matching IdP or invalid token"};

    AuthContext ctx;
    ctx.subject         = identity->subject;
    ctx.name            = identity->email.empty() ? identity->subject : identity->email;
    ctx.role            = identity->role;
    ctx.source          = "sso:" + identity->idp_id;
    ctx.allowed_symbols = identity->allowed_symbols;
    ctx.tenant_id       = identity->tenant_id;
    return {AuthStatus::OK, ctx, ""};
}

// ============================================================================
// check_jwt
// ============================================================================
AuthDecision AuthManager::check_jwt(const std::string& token) const {
    auto claims = jwt_validator_->validate(token);
    if (!claims) {
        return {AuthStatus::UNAUTHORIZED, {}, "Invalid or expired JWT"};
    }

    AuthContext ctx;
    ctx.subject         = claims->subject;
    ctx.name            = claims->email.empty() ? claims->subject : claims->email;
    ctx.role            = claims->role;
    ctx.source          = "jwt";
    ctx.allowed_symbols = claims->allowed_symbols;

    return {AuthStatus::OK, ctx, ""};
}

// ============================================================================
// Admin: API key management
// ============================================================================
std::string AuthManager::create_api_key(const std::string& name,
                                          Role role,
                                          const std::vector<std::string>& symbols,
                                          const std::vector<std::string>& tables,
                                          const std::string& tenant_id,
                                          int64_t expires_at_ns)
{
    if (!key_store_) throw std::runtime_error("No API key store configured");
    return key_store_->create_key(name, role, symbols, tables, tenant_id, expires_at_ns);
}

bool AuthManager::revoke_api_key(const std::string& key_id) {
    if (!key_store_) return false;
    return key_store_->revoke(key_id);
}

bool AuthManager::update_api_key(const std::string& key_id,
                                  const std::optional<std::vector<std::string>>& symbols,
                                  const std::optional<std::vector<std::string>>& tables,
                                  const std::optional<bool>& enabled,
                                  const std::optional<std::string>& tenant_id,
                                  const std::optional<int64_t>& expires_at_ns)
{
    if (!key_store_) return false;
    return key_store_->update_key(key_id, symbols, tables, enabled, tenant_id, expires_at_ns);
}

std::vector<ApiKeyEntry> AuthManager::list_api_keys() const {
    if (!key_store_) return {};
    return key_store_->list();
}

// ============================================================================
// JWKS refresh
// ============================================================================
bool AuthManager::refresh_jwks() {
    if (!jwks_provider_) return false;
    return jwks_provider_->refresh();
}

// ============================================================================
// check_session — resolve session cookie to AuthContext
// ============================================================================
std::optional<AuthContext> AuthManager::check_session(
    const std::string& cookie_header) const
{
    if (!session_store_ || cookie_header.empty()) return std::nullopt;

    // Parse cookie header for session cookie
    const auto& name = session_store_->config().cookie_name;
    std::string search = name + "=";
    auto pos = cookie_header.find(search);
    if (pos == std::string::npos) return std::nullopt;

    auto val_start = pos + search.size();
    auto val_end = cookie_header.find(';', val_start);
    std::string sid = (val_end != std::string::npos)
        ? cookie_header.substr(val_start, val_end - val_start)
        : cookie_header.substr(val_start);

    // Trim whitespace
    while (!sid.empty() && sid.back() == ' ') sid.pop_back();

    auto session = session_store_->get(sid);
    if (!session) return std::nullopt;

    AuthContext ctx;
    ctx.subject         = session->subject;
    ctx.name            = session->name;
    ctx.role            = session->role;
    ctx.source          = session->source;
    ctx.allowed_symbols = session->allowed_symbols;
    ctx.tenant_id       = session->tenant_id;
    return ctx;
}

// ============================================================================
// audit
// ============================================================================
void AuthManager::audit(const AuthContext& ctx,
                         const std::string& action,
                         const std::string& detail,
                         const std::string& remote_addr) const
{
    if (!config_.audit_enabled) return;
    auto logger = get_audit_logger(config_.audit_log_file);
    logger->info("subject={} role={} action=\"{}\" detail=\"{}\" from={}",
                 ctx.subject,
                 role_to_string(ctx.role),
                 action,
                 detail,
                 remote_addr.empty() ? "-" : remote_addr);

    // Push to in-memory ring buffer for /admin/audit endpoint
    if (config_.audit_buffer_enabled) {
        using namespace std::chrono;
        AuditEvent ev;
        ev.timestamp_ns = duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count();
        ev.subject     = ctx.subject;
        ev.role_str    = role_to_string(ctx.role);
        ev.action      = action;
        ev.detail      = detail;
        ev.remote_addr = remote_addr.empty() ? "-" : remote_addr;
        audit_buffer_.push(std::move(ev));
    }
}

// ============================================================================
// extract_bearer_token
// ============================================================================
std::string AuthManager::extract_bearer_token(const std::string& auth_header) {
    static const std::string bearer = "Bearer ";
    if (auth_header.size() <= bearer.size()) return "";

    // Case-insensitive prefix check
    for (size_t i = 0; i < bearer.size(); ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(auth_header[i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(bearer[i])));
        if (a != b) return "";
    }
    return auth_header.substr(bearer.size());
}

// ============================================================================
// is_public_path
// ============================================================================
bool AuthManager::is_public_path(const std::string& path) const {
    for (const auto& p : config_.public_paths)
        if (p == path) return true;
    return false;
}

} // namespace zeptodb::auth
