#pragma once
// ============================================================================
// ZeptoDB: License Validator
// ============================================================================
// Edition-based feature gating via RS256-signed JWT license keys.
//
// Editions: Community (default, no key), Enterprise
// Loading:  env ZEPTODB_LICENSE_KEY → file /etc/zeptodb/license.key → direct
// Expiry:   7-day warning, 30-day grace, then downgrade to Community
//
// Usage:
//   if (!license().hasFeature(Feature::CLUSTER)) { /* return 402 */ }
// ============================================================================

#include <cstdint>
#include <string>

namespace zeptodb::auth {

// ============================================================================
// Edition — product tier
// ============================================================================
enum class Edition : uint8_t { COMMUNITY = 0, ENTERPRISE = 1 };

// ============================================================================
// Feature — gated capability bitmask
// ============================================================================
enum class Feature : uint32_t {
    CLUSTER         = 1u << 0,
    SSO             = 1u << 1,
    AUDIT_EXPORT    = 1u << 2,
    ADVANCED_RBAC   = 1u << 3,
    KAFKA           = 1u << 4,
    MIGRATION       = 1u << 5,
    GEO_REPLICATION = 1u << 6,
    ROLLING_UPGRADE = 1u << 7,
    IOT_CONNECTORS  = 1u << 8,  // MQTT (now); future: OPC-UA, ROS2, Pulsar, Kinesis
};

// ============================================================================
// LicenseClaims — decoded JWT payload
// ============================================================================
struct LicenseClaims {
    Edition     edition    = Edition::COMMUNITY;
    uint32_t    features   = 0;        // bitmask of Feature
    int         max_nodes  = 1;
    std::string company;
    std::string tenant_id;             // SaaS: tenant isolation
    int64_t     expiry     = 0;        // Unix seconds
    int64_t     issued_at  = 0;
    int         grace_days = 30;
};

// ============================================================================
// LicenseValidator
// ============================================================================
class LicenseValidator {
public:
    /// Embedded public key for license verification.
    static constexpr const char* DEFAULT_PUBLIC_KEY =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAw7GwRM7KiilznM33R0lv\n"
        "IALADJAw/monlh/rR4sxFoL9q8mV+g8wxUL170NYlT80MGnJmNHyaaSdmxXuYfia\n"
        "F/acTxCpMKrO0O2/YpRjlRxo0JTmVSwX5LLIYmG5UPGqaxwiGNToRcLkE0vBQkZe\n"
        "lfZGw39LMKXdYPPGmS6nb7jQpx2ch3jmoKlnTCUm8j+KfQnjltkUYBDijZLPf7Dc\n"
        "+pL6obF0yvjD09dhejGITTViiWrNcRD5sAQH/zejQwWUtzb2Bb/kw8t0V5mYritL\n"
        "+WX0rbaAqSD6DhPvxJhvxxH9ib0ZR8IBNmr5HYINENlxjbVFlS6nXn3Z5PQv0P/u\n"
        "zQIDAQAB\n"
        "-----END PUBLIC KEY-----\n";

    explicit LicenseValidator(std::string public_key_pem = "");

    /// Load license: env ZEPTODB_LICENSE_KEY → /etc/zeptodb/license.key → key_or_path.
    /// Returns true if a valid license was loaded.
    bool load(const std::string& key_or_path = "");

    const LicenseClaims& claims() const { return claims_; }
    Edition edition() const {
        if (isExpired() && !inGracePeriod()) return Edition::COMMUNITY;
        return claims_.edition;
    }
    bool hasFeature(Feature f) const;
    bool isExpired() const;
    bool inGracePeriod() const;
    int  maxNodes() const {
        if (isExpired() && !inGracePeriod()) return 1;
        return claims_.max_nodes;
    }

    /// Load a JWT string without signature verification (test-only).
    bool load_from_jwt_string_for_testing(const std::string& jwt);

    /// Startup banner line, e.g. "ZeptoDB v0.1.0 (Enterprise — Acme Corp, 16 nodes, expires 2027-04-01)"
    std::string statusLine() const;

    /// Multi-line startup banner with optional upgrade hint for Community.
    std::string startupBanner() const;

    /// Generate a 30-day trial license JWT (unsigned, trial=true).
    static std::string generate_trial_key(const std::string& company = "Trial");

    /// Check if current license is a trial.
    bool isTrial() const { return trial_; }

private:
    LicenseClaims claims_;
    std::string   public_key_pem_;
    bool          loaded_ = false;
    bool          trial_  = false;

    bool decode_and_verify(const std::string& jwt);
};

/// Global singleton accessor.
LicenseValidator& license();

} // namespace zeptodb::auth
