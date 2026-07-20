// ============================================================================
// ZeptoDB: AuthManager Implementation
// ============================================================================
#include "zeptodb/auth/auth_manager.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <chrono>
#include <string_view>

namespace zeptodb::auth {

namespace {

constexpr size_t kMaxInternalAuthValueBytes = 4096;
constexpr size_t kMaxInternalAuthListItems = 256;
constexpr size_t kMaxInternalAuthListBytes = 8192;

bool is_safe_internal_auth_value(std::string_view value) {
    if (value.size() > kMaxInternalAuthValueBytes) return false;
    return std::none_of(value.begin(), value.end(), [](const char value_char) {
        const auto byte = static_cast<unsigned char>(value_char);
        return byte < 0x20U || byte == 0x7fU;
    });
}

bool is_safe_internal_auth_list(const std::vector<std::string>& values) {
    if (values.size() > kMaxInternalAuthListItems) return false;
    size_t serialized_bytes = values.empty() ? 0 : values.size() - 1;
    for (const auto& value : values) {
        // HTTP authorization middleware serializes these lists with commas.
        // Empty or comma-containing entries would change the restriction's
        // meaning after serialization and therefore must fail closed.
        if (value.empty() || value.find(',') != std::string::npos ||
            !is_safe_internal_auth_value(value) ||
            value.size() > kMaxInternalAuthListBytes -
                std::min(serialized_bytes, kMaxInternalAuthListBytes)) {
            return false;
        }
        serialized_bytes += value.size();
    }
    return serialized_bytes <= kMaxInternalAuthListBytes;
}

bool is_safe_internal_auth_context(const AuthContext& context) {
    return !context.subject.empty() &&
        is_safe_internal_auth_value(context.subject) &&
        is_safe_internal_auth_value(context.name) &&
        !context.source.empty() &&
        is_safe_internal_auth_value(context.source) &&
        is_safe_internal_auth_value(context.tenant_id) &&
        is_safe_internal_auth_list(context.allowed_symbols) &&
        is_safe_internal_auth_list(context.allowed_tables);
}

std::string audit_log_value(std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char byte : value) {
        switch (byte) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (byte < 0x20U || byte == 0x7fU) {
                    escaped += "\\x";
                    escaped.push_back(kHex[byte >> 4U]);
                    escaped.push_back(kHex[byte & 0x0fU]);
                } else {
                    escaped.push_back(static_cast<char>(byte));
                }
        }
    }
    return escaped;
}

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
            oidc_idp.tenant_claim = config_.jwt.tenant_claim;
            oidc_idp.symbols_claim = config_.jwt.symbols_claim;
            oidc_idp.tables_claim = config_.jwt.tables_claim;
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
                                 const std::string& remote_addr,
                                 const std::string& cookie_header) const
{
    if (!config_.enabled) {
        return authenticate(method, path, auth_header, remote_addr,
                            cookie_header);
    }

    // /auth/session always performs credential validation in its handler.
    // Treat it as pre-routing-public even when an operator replaces the
    // configurable public_paths list, avoiding duplicate rate/audit charges.
    if (path == "/auth/session" || is_public_path(path)) {
        // OIDC login/callback are public by protocol, but callback performs an
        // outbound token exchange. Charge the source-IP bucket before either
        // endpoint so attackers cannot mint states and drive unbounded IdP
        // requests without credentials.
        if (rate_limiter_ &&
            (path == "/auth/login" || path == "/auth/callback")) {
            const uint64_t check_number =
                rate_limit_checks_.fetch_add(1, std::memory_order_relaxed);
            if ((check_number & 0xfffU) == 0xfffU) {
                rate_limiter_->cleanup();
            }
            if (rate_limiter_->check_ip(remote_addr) ==
                RateDecision::RATE_LIMITED) {
                AuthContext context;
                context.subject = "anonymous";
                context.name = "anonymous";
                context.role = Role::METRICS;
                context.source = "oidc-public";
                if (config_.audit_enabled) {
                    audit(context, method + " " + path,
                          "oidc-public-rate-limit-forbidden", remote_addr);
                }
                return {AuthStatus::FORBIDDEN, std::move(context),
                        "Rate limit exceeded"};
            }
        }
        AuthContext ctx;
        ctx.subject = "anonymous";
        ctx.name    = "anonymous";
        ctx.role    = Role::METRICS;
        ctx.source  = "public";
        return {AuthStatus::OK, ctx, ""};
    }

    return authenticate(method, path, auth_header, remote_addr, cookie_header);
}

// ============================================================================
// authenticate
// ============================================================================
AuthDecision AuthManager::authenticate(const std::string& method,
                                        const std::string& path,
                                        const std::string& auth_header,
                                        const std::string& remote_addr,
                                        const std::string& cookie_header) const
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

    // Charge the source-IP bucket before JWT/JWKS/API-key verification so
    // invalid-token floods cannot bypass throttling while consuming crypto.
    if (rate_limiter_) {
        const uint64_t check_number =
            rate_limit_checks_.fetch_add(1, std::memory_order_relaxed);
        if ((check_number & 0xfffU) == 0xfffU) {
            rate_limiter_->cleanup();
        }
        if (rate_limiter_->check_ip(remote_addr) ==
            RateDecision::RATE_LIMITED) {
            AuthContext context;
            context.subject = "anonymous";
            context.name = "anonymous";
            context.role = Role::METRICS;
            context.source = "pre-auth";
            if (config_.audit_enabled) {
                audit(context, method + " " + path,
                      "pre-auth-rate-limit-forbidden", remote_addr);
            }
            return {AuthStatus::FORBIDDEN, std::move(context),
                    "Rate limit exceeded"};
        }
    }

    auto finish_authenticated = [this, &method, &path, &remote_addr](
        AuthDecision decision, const std::string& audit_detail) {
        if (decision.status != AuthStatus::OK) return decision;
        if (!is_safe_internal_auth_context(decision.context)) {
            return AuthDecision{
                AuthStatus::FORBIDDEN, {},
                "Authenticated identity contains an invalid authorization scope"};
        }
        if (rate_limiter_) {
            const auto identity_decision =
                rate_limiter_->check_identity(decision.context.subject);
            if (identity_decision == RateDecision::RATE_LIMITED) {
                if (config_.audit_enabled) {
                    audit(decision.context, method + " " + path,
                          "rate-limit-forbidden", remote_addr);
                }
                return AuthDecision{AuthStatus::FORBIDDEN,
                                    std::move(decision.context),
                                    "Rate limit exceeded"};
            }
        }
        if (config_.audit_enabled) {
            audit(decision.context, method + " " + path, audit_detail,
                  remote_addr);
        }
        return decision;
    };

    // No Bearer header: authenticate a server-side session cookie if present.
    if (auth_header.empty()) {
        if (!cookie_header.empty()) {
            auto context = check_session(cookie_header);
            if (context) {
                return finish_authenticated(
                    {AuthStatus::OK, std::move(*context), ""},
                    "session-auth");
            }
            return {AuthStatus::UNAUTHORIZED, {}, "Invalid or expired session"};
        }
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
            return finish_authenticated(std::move(decision), "sso-auth");
        }
    }

    // Try JWT first (Bearer tokens starting with "ey" are JWTs)
    if (config_.jwt_enabled && jwt_validator_ && token.size() > 2 &&
        token[0] == 'e' && token[1] == 'y') {
        auto decision = check_jwt(token);
        if (decision.status == AuthStatus::OK) {
            return finish_authenticated(std::move(decision), "jwt-auth");
        }
    }

    // Try API key
    if (key_store_) {
        auto decision = check_api_key(token);
        if (decision.status == AuthStatus::OK) {
            return finish_authenticated(std::move(decision), "apikey-auth");
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
    ctx.tenant_id       = entry->tenant_id;
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
    ctx.allowed_tables  = identity->allowed_tables;
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
    ctx.tenant_id       = claims->tenant_id;
    ctx.allowed_tables  = claims->allowed_tables;

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

    // Parse complete cookie pairs. A substring search can mistake another
    // cookie's value (for example `other=zepto_sid=...`) for the session.
    const auto& name = session_store_->config().cookie_name;
    std::string sid;
    size_t start = 0;
    while (start < cookie_header.size()) {
        while (start < cookie_header.size() &&
               (cookie_header[start] == ' ' || cookie_header[start] == ';')) {
            ++start;
        }
        const size_t end = cookie_header.find(';', start);
        const size_t equals = cookie_header.find('=', start);
        if (equals != std::string::npos &&
            (end == std::string::npos || equals < end) &&
            cookie_header.compare(start, equals - start, name) == 0) {
            sid = cookie_header.substr(
                equals + 1,
                end == std::string::npos ? std::string::npos
                                          : end - equals - 1);
            break;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (sid.empty()) return std::nullopt;

    auto session = session_store_->get(sid);
    if (!session) return std::nullopt;

    AuthContext ctx;
    ctx.subject         = session->subject;
    ctx.name            = session->name;
    ctx.role            = session->role;
    ctx.source          = session->source;
    ctx.allowed_symbols = session->allowed_symbols;
    ctx.tenant_id       = session->tenant_id;
    ctx.allowed_tables  = session->allowed_tables;
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
                 audit_log_value(ctx.subject),
                 role_to_string(ctx.role),
                 audit_log_value(action),
                 audit_log_value(detail),
                 audit_log_value(remote_addr.empty() ? "-" : remote_addr));

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
