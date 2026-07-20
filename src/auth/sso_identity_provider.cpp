// ============================================================================
// ZeptoDB: SSO Identity Provider Implementation
// ============================================================================
#include "zeptodb/auth/sso_identity_provider.h"
#include "json_claims.h"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace zeptodb::auth {

// ============================================================================
// Constructor
// ============================================================================
SsoIdentityProvider::SsoIdentityProvider() : SsoIdentityProvider(Config{}) {}

SsoIdentityProvider::SsoIdentityProvider(Config config)
    : config_(std::move(config)) {
    if (config_.cache_ttl_s < 0) {
        throw std::invalid_argument("SSO cache TTL must not be negative");
    }
}

// ============================================================================
// add_idp
// ============================================================================
bool SsoIdentityProvider::add_idp(IdpConfig idp) {
    std::lock_guard lock(mu_);
    if (idps_.count(idp.id)) return false;

    IdpRuntime rt;
    rt.config = std::move(idp);

    // Build JwtValidator config
    JwtValidator::Config jwt_cfg;
    jwt_cfg.expected_issuer   = rt.config.issuer;
    jwt_cfg.expected_audience = rt.config.audience;
    jwt_cfg.verify_expiry     = true;
    jwt_cfg.default_role      = Role::UNKNOWN;
    jwt_cfg.role_claim        = "zepto_role";
    jwt_cfg.symbols_claim     = rt.config.symbols_claim;
    jwt_cfg.tenant_claim      = rt.config.tenant_claim;
    jwt_cfg.tables_claim      = rt.config.tables_claim;
    rt.validator = std::make_unique<JwtValidator>(jwt_cfg);

    // JWKS auto-fetch
    if (!rt.config.jwks_url.empty()) {
        rt.jwks = std::make_unique<JwksProvider>(rt.config.jwks_url);
        rt.jwks->refresh();
        rt.validator->set_key_resolver([jwks = rt.jwks.get()](const std::string& kid) {
            if (!kid.empty()) return jwks->get_pem(kid);
            return jwks->get_default_pem();
        });
        rt.jwks->start();
    }

    std::string id     = rt.config.id;
    std::string issuer = rt.config.issuer;
    idps_.emplace(id, std::move(rt));
    issuer_map_.emplace(std::move(issuer), std::move(id));
    return true;
}

// ============================================================================
// remove_idp
// ============================================================================
bool SsoIdentityProvider::remove_idp(const std::string& idp_id) {
    std::lock_guard lock(mu_);
    auto it = idps_.find(idp_id);
    if (it == idps_.end()) return false;

    issuer_map_.erase(it->second.config.issuer);
    idps_.erase(it);
    return true;
}

// ============================================================================
// list_idps
// ============================================================================
std::vector<std::string> SsoIdentityProvider::list_idps() const {
    std::lock_guard lock(mu_);
    std::vector<std::string> ids;
    ids.reserve(idps_.size());
    for (const auto& [id, _] : idps_) ids.push_back(id);
    return ids;
}

// ============================================================================
// resolve
// ============================================================================
std::optional<SsoIdentity> SsoIdentityProvider::resolve(const std::string& jwt_token) const {
    // Extract issuer from token payload (without full validation)
    std::string issuer = extract_issuer_from_jwt(jwt_token);
    if (issuer.empty()) return std::nullopt;

    std::lock_guard lock(mu_);
    auto iss_it = issuer_map_.find(issuer);
    if (iss_it == issuer_map_.end()) return std::nullopt;

    auto idp_it = idps_.find(iss_it->second);
    if (idp_it == idps_.end()) return std::nullopt;

    // Check cache
    // We need the subject for cache key, but we don't have it yet before validation.
    // So we validate first, then cache.
    return resolve_with_idp(idp_it->second, jwt_token);
}

// ============================================================================
// resolve_with_idp
// ============================================================================
std::optional<SsoIdentity> SsoIdentityProvider::resolve_with_idp(
    const IdpRuntime& idp, const std::string& token) const
{
    auto claims = idp.validator->validate(token);
    if (!claims) return std::nullopt;

    // Check cache
    std::string ck = cache_key(idp.config.id, token);
    auto cached = lookup_cache(ck);
    if (cached) return cached;

    SsoIdentity identity;
    identity.subject = claims->subject;
    identity.email   = claims->email;
    identity.idp_id  = idp.config.id;
    identity.allowed_symbols = claims->allowed_symbols;
    identity.tenant_id = claims->tenant_id;
    identity.allowed_tables = claims->allowed_tables;

    // Extract groups from the raw JWT payload for group-based role mapping
    // We re-decode the payload to get the group claim
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    if (dot1 != std::string::npos && dot2 != std::string::npos) {
        std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
        std::string payload_json = JwtValidator::base64url_decode(payload_b64);

        // Extract group claim
        if (!idp.config.group_claim.empty()) {
            identity.groups = extract_json_string_array(payload_json, idp.config.group_claim);
        }

    }

    // Resolve role: group mapping takes priority, then JWT claim, then default
    identity.role = resolve_role(idp.config, identity.groups, claims->role);

    store_cache(ck, identity, claims->expiry);
    return identity;
}

// ============================================================================
// resolve_role — pick highest-privilege matching group
// ============================================================================
std::optional<Role> SsoIdentityProvider::resolve_group_role(
    const IdpConfig& cfg, const std::vector<std::string>& groups) {
    // Role priority: ADMIN(0) > WRITER(1) > READER(2) > ANALYST(3) > METRICS(4)
    std::optional<Role> best;
    for (const auto& g : groups) {
        auto it = cfg.group_role_map.find(g);
        if (it != cfg.group_role_map.end()) {
            if (!best || static_cast<uint8_t>(it->second) <
                    static_cast<uint8_t>(*best)) {
                best = it->second;
            }
        }
    }
    return best;
}

Role SsoIdentityProvider::resolve_role(
    const IdpConfig& cfg, const std::vector<std::string>& groups,
    Role token_role) {
    const auto group_role = resolve_group_role(cfg, groups);
    if (group_role) return *group_role;
    return token_role != Role::UNKNOWN ? token_role : cfg.default_role;
}

// ============================================================================
// extract_issuer_from_jwt — quick iss extraction without full validation
// ============================================================================
std::string SsoIdentityProvider::extract_issuer_from_jwt(const std::string& token) {
    auto dot1 = token.find('.');
    if (dot1 == std::string::npos || dot1 == 0) return "";
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos || dot2 == dot1 + 1 ||
        dot2 + 1 >= token.size() ||
        token.find('.', dot2 + 1) != std::string::npos) {
        return "";
    }

    std::string payload = JwtValidator::base64url_decode(
        token.substr(dot1 + 1, dot2 - dot1 - 1));

    return extract_json_string(payload, "iss");
}

// ============================================================================
// Cache operations
// ============================================================================
std::string SsoIdentityProvider::cache_key(const std::string& idp_id,
                                            const std::string& token) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(token.data(), token.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key = idp_id + ":";
    key.reserve(key.size() + digest_size * 2);
    for (unsigned int i = 0; i < digest_size; ++i) {
        key.push_back(kHex[digest[i] >> 4U]);
        key.push_back(kHex[digest[i] & 0x0fU]);
    }
    return key;
}

std::optional<SsoIdentity> SsoIdentityProvider::lookup_cache(const std::string& key) const {
    std::lock_guard lock(cache_mu_);

    if (config_.cache_capacity == 0) return std::nullopt;
    auto it = cache_.find(key);
    if (it == cache_.end()) return std::nullopt;

    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ns > it->second.expires_at_ns) {
        cache_.erase(it);
        return std::nullopt;
    }
    return it->second.identity;
}

void SsoIdentityProvider::store_cache(const std::string& key,
                                       const SsoIdentity& identity,
                                       int64_t token_expiry_s) const {
    std::lock_guard lock(cache_mu_);

    if (config_.cache_capacity == 0 || config_.cache_ttl_s == 0 ||
        key.empty()) {
        return;
    }

    const auto system_now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t token_remaining_s = token_expiry_s - system_now_s;
    if (token_remaining_s <= 0) return;
    const int64_t ttl_s = std::min(config_.cache_ttl_s, token_remaining_s);

    // Evict if at capacity (simple: clear all expired, then oldest if still full)
    if (cache_.size() >= config_.cache_capacity) {
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (now_ns > it->second.expires_at_ns)
                it = cache_.erase(it);
            else
                ++it;
        }
        if (cache_.size() >= config_.cache_capacity) {
            const auto oldest = std::min_element(
                cache_.begin(), cache_.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second.expires_at_ns <
                           rhs.second.expires_at_ns;
                });
            if (oldest != cache_.end()) cache_.erase(oldest);
        }
    }

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t ttl_ns = ttl_s >
            std::numeric_limits<int64_t>::max() / 1'000'000'000LL
        ? std::numeric_limits<int64_t>::max()
        : ttl_s * 1'000'000'000LL;
    const int64_t expires = ttl_ns >
            std::numeric_limits<int64_t>::max() - now_ns
        ? std::numeric_limits<int64_t>::max()
        : now_ns + ttl_ns;

    cache_[key] = {identity, expires};
}

void SsoIdentityProvider::clear_cache() {
    std::lock_guard lock(cache_mu_);
    cache_.clear();
}

size_t SsoIdentityProvider::cache_size() const {
    std::lock_guard lock(cache_mu_);
    return cache_.size();
}

// ============================================================================
// Minimal JSON helpers (same approach as JwtValidator — no external dep)
// ============================================================================
std::string SsoIdentityProvider::extract_json_string(const std::string& json,
                                                      const std::string& key) {
    std::string value;
    if (detail::read_json_string(json, key, &value) !=
        detail::JsonFieldStatus::Valid) {
        return "";
    }
    return value;
}

std::vector<std::string> SsoIdentityProvider::extract_json_string_array(
    const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    if (detail::read_json_string_array(json, key, &result) !=
        detail::JsonFieldStatus::Valid) {
        result.clear();
    }
    return result;
}

} // namespace zeptodb::auth
