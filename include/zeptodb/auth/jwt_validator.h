#pragma once
// ============================================================================
// ZeptoDB: JWT Validator (SSO / OIDC)
// ============================================================================
// Validates JSON Web Tokens for Single Sign-On integration.
//
// Supported algorithms:
//   HS256 — HMAC-SHA256 with shared secret (internal / simple setups)
//   RS256 — RSA-SHA256 with PEM public key (Okta, Azure AD, Google Workspace)
//
// Expected JWT payload claims:
//   sub          — subject / user id
//   exp          — expiry (Unix timestamp, validated)
//   iss          — issuer (validated when expected_issuer configured)
//   aud          — audience (validated when expected_audience configured)
//   zepto_role    — role string ("admin"|"writer"|"reader"|"analyst"|"metrics")
//   zepto_symbols — comma-separated symbol whitelist (optional)
//
// Usage:
//   JwtValidator::Config cfg;
//   cfg.hs256_secret = "my-secret";
//   cfg.expected_issuer = "https://auth.example.com";
//   JwtValidator v(cfg);
//   auto claims = v.validate(token);
// ============================================================================

#include "zeptodb/auth/rbac.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace zeptodb::auth {

// ============================================================================
// JwtClaims — parsed + validated JWT payload
// ============================================================================
struct JwtClaims {
    std::string              subject;          // "sub"
    std::string              email;            // "email" (optional)
    std::string              issuer;           // "iss"
    int64_t                  expiry = 0;       // "exp" Unix seconds
    Role                     role = Role::READER;
    std::vector<std::string> allowed_symbols;  // from "zepto_symbols" claim
};

// ============================================================================
// JwtValidator
// ============================================================================
class JwtValidator {
public:
    struct Config {
        // --- Algorithm selection ---
        // Exactly one of hs256_secret or rs256_public_key_pem must be set.
        std::string hs256_secret;           // shared secret for HS256
        std::string rs256_public_key_pem;   // PEM public key for RS256

        // --- Claim validation ---
        std::string expected_issuer;    // validate "iss" if non-empty
        std::string expected_audience;  // validate "aud" if non-empty
        bool        verify_expiry = true;

        // --- Claim name mapping ---
        std::string role_claim    = "zepto_role";    // claim carrying the role
        std::string symbols_claim = "zepto_symbols"; // claim carrying symbol list
    };

    explicit JwtValidator(Config config);

    /// Set a dynamic key resolver for JWKS-based validation.
    /// Called with kid from JWT header; should return PEM public key.
    using KeyResolver = std::function<std::string(const std::string& kid)>;
    void set_key_resolver(KeyResolver resolver) { key_resolver_ = std::move(resolver); }

    // Validate token. Returns nullopt on any failure (expired, bad sig, etc).
    std::optional<JwtClaims> validate(const std::string& token) const;

    // Base64url decode (public for testing).
    static std::string base64url_decode(const std::string& input);

private:
    Config config_;
    KeyResolver key_resolver_;

    bool verify_hs256(const std::string& header_payload,
                      const std::string& b64sig) const;
    bool verify_rs256(const std::string& header_payload,
                      const std::string& b64sig) const;

    // Minimal JSON field extractors (no external dependency).
    static std::string get_json_string(const std::string& json,
                                       const std::string& key);
    static int64_t     get_json_int64(const std::string& json,
                                      const std::string& key);
    static std::vector<std::string> get_json_string_array(
        const std::string& json, const std::string& key);
};

} // namespace zeptodb::auth
