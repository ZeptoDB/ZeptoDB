#pragma once
// ============================================================================
// ZeptoDB: License Validator
// ============================================================================
// Edition-based feature gating via RS256-signed JWT license keys.
//
// Editions: Community (default, no key), Pro, Enterprise
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
enum class Edition : uint8_t { COMMUNITY = 0, PRO = 1, ENTERPRISE = 2 };

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
    /// Embedded public key — replace with actual signing key for production.
    static constexpr const char* DEFAULT_PUBLIC_KEY = "";

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

private:
    LicenseClaims claims_;
    std::string   public_key_pem_;
    bool          loaded_ = false;

    bool decode_and_verify(const std::string& jwt);
};

/// Global singleton accessor.
LicenseValidator& license();

} // namespace zeptodb::auth
