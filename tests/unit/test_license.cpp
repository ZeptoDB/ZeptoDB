// ============================================================================
// ZeptoDB: License Validator Tests
// ============================================================================
#include <gtest/gtest.h>
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/auth/jwt_validator.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <chrono>
#include <cstdlib>
#include <string>

using namespace zeptodb::auth;

// ============================================================================
// Helpers (same base64url as test_auth.cpp)
// ============================================================================
static std::string b64url_encode(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i+1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i+2]);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[n & 63]         : '=';
    }
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

static std::string b64url_str(const std::string& s) {
    return b64url_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

static int64_t unix_now(int64_t offset = 0) {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count() + offset;
}

// Build an unsigned JWT (header.payload.empty_sig) — works when public_key is empty
static std::string make_unsigned_jwt(const std::string& payload_json) {
    std::string header  = b64url_str(R"({"alg":"RS256","typ":"JWT"})");
    std::string payload = b64url_str(payload_json);
    return header + "." + payload + ".fakesig";
}

// ============================================================================
// RSA key pair for signature tests
// ============================================================================
class LicenseRsaFixture : public ::testing::Test {
protected:
    std::string private_pem_;
    std::string public_pem_;

    void SetUp() override {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
        EVP_PKEY* pkey = nullptr;
        EVP_PKEY_keygen(ctx, &pkey);
        EVP_PKEY_CTX_free(ctx);

        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        char* d; long l = BIO_get_mem_data(bio, &d);
        private_pem_ = std::string(d, l);
        BIO_free(bio);

        bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(bio, pkey);
        l = BIO_get_mem_data(bio, &d);
        public_pem_ = std::string(d, l);
        BIO_free(bio);

        EVP_PKEY_free(pkey);
    }

    std::string sign_jwt(const std::string& payload_json) const {
        std::string header  = b64url_str(R"({"alg":"RS256","typ":"JWT"})");
        std::string payload = b64url_str(payload_json);
        std::string hp = header + "." + payload;

        BIO* bio = BIO_new_mem_buf(private_pem_.data(), static_cast<int>(private_pem_.size()));
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        EVP_MD_CTX* md = EVP_MD_CTX_new();
        EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, pkey);
        EVP_DigestSignUpdate(md, reinterpret_cast<const unsigned char*>(hp.data()), hp.size());
        size_t sig_len = 0;
        EVP_DigestSignFinal(md, nullptr, &sig_len);
        std::vector<unsigned char> sig(sig_len);
        EVP_DigestSignFinal(md, sig.data(), &sig_len);
        EVP_MD_CTX_free(md);
        EVP_PKEY_free(pkey);

        return hp + "." + b64url_encode(sig.data(), sig_len);
    }
};

// ============================================================================
// 1. Default (no key) → Community edition
// ============================================================================
TEST(LicenseTest, DefaultNoCommunity) {
    // Ensure env var is not set
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v;
    bool ok = v.load("");
    EXPECT_FALSE(ok);
    EXPECT_EQ(v.edition(), Edition::COMMUNITY);
    EXPECT_EQ(v.maxNodes(), 1);
    EXPECT_EQ(v.claims().features, 0u);
    EXPECT_FALSE(v.hasFeature(Feature::CLUSTER));
    EXPECT_FALSE(v.hasFeature(Feature::SSO));
    EXPECT_FALSE(v.hasFeature(Feature::KAFKA));
    EXPECT_FALSE(v.isExpired());
    EXPECT_FALSE(v.inGracePeriod());
}

// ============================================================================
// 2. Valid license JWT → correct edition, features, max_nodes
// ============================================================================
TEST(LicenseTest, ValidEnterpriseLicense) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER) |
                     static_cast<uint32_t>(Feature::SSO) |
                     static_cast<uint32_t>(Feature::KAFKA);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":16,"company":"TestCorp","exp":)" + std::to_string(unix_now(86400)) +
        R"(,"iat":)" + std::to_string(unix_now()) +
        R"(,"grace_days":14})";
    std::string jwt = make_unsigned_jwt(payload);

    EXPECT_TRUE(v.load_from_jwt_string_for_testing(jwt));
    EXPECT_EQ(v.edition(), Edition::ENTERPRISE);
    EXPECT_EQ(v.maxNodes(), 16);
    EXPECT_EQ(v.claims().company, "TestCorp");
    EXPECT_EQ(v.claims().grace_days, 14);
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
    EXPECT_TRUE(v.hasFeature(Feature::SSO));
    EXPECT_TRUE(v.hasFeature(Feature::KAFKA));
    EXPECT_FALSE(v.hasFeature(Feature::GEO_REPLICATION));
    EXPECT_FALSE(v.isExpired());
}

TEST(LicenseTest, ValidEnterpriseLicenseSmall) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::KAFKA) |
                     static_cast<uint32_t>(Feature::MIGRATION);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":4,"company":"SmallCo","exp":)" + std::to_string(unix_now(86400)) + "}";

    EXPECT_TRUE(v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload)));
    EXPECT_EQ(v.edition(), Edition::ENTERPRISE);
    EXPECT_EQ(v.maxNodes(), 4);
    EXPECT_TRUE(v.hasFeature(Feature::KAFKA));
    EXPECT_TRUE(v.hasFeature(Feature::MIGRATION));
    EXPECT_FALSE(v.hasFeature(Feature::CLUSTER));
}

// ============================================================================
// 3. Expired license → isExpired() true
// ============================================================================
TEST(LicenseTest, ExpiredLicense) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER);
    // Expired 2 days ago, grace_days=30 → still in grace
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":8,"exp":)" + std::to_string(unix_now(-2 * 86400)) +
        R"(,"grace_days":30})";

    EXPECT_TRUE(v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload)));
    EXPECT_TRUE(v.isExpired());
}

// ============================================================================
// 4. Grace period (expired within grace_days) → inGracePeriod() true, features active
// ============================================================================
TEST(LicenseTest, GracePeriodActive) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER) |
                     static_cast<uint32_t>(Feature::SSO);
    // Expired 5 days ago, grace_days=30
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":8,"exp":)" + std::to_string(unix_now(-5 * 86400)) +
        R"(,"grace_days":30})";

    EXPECT_TRUE(v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload)));
    EXPECT_TRUE(v.isExpired());
    EXPECT_TRUE(v.inGracePeriod());
    // Features still active during grace
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
    EXPECT_TRUE(v.hasFeature(Feature::SSO));
}

// ============================================================================
// 5. Grace period exceeded → features disabled, Community fallback
// ============================================================================
TEST(LicenseTest, GracePeriodExceeded) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER) |
                     static_cast<uint32_t>(Feature::SSO);
    // Expired 60 days ago, grace_days=30 → grace ended
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":8,"exp":)" + std::to_string(unix_now(-60 * 86400)) +
        R"(,"grace_days":30})";

    EXPECT_TRUE(v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload)));
    EXPECT_TRUE(v.isExpired());
    EXPECT_FALSE(v.inGracePeriod());
    // Features disabled
    EXPECT_FALSE(v.hasFeature(Feature::CLUSTER));
    EXPECT_FALSE(v.hasFeature(Feature::SSO));
}

// ============================================================================
// 6. Invalid signature → falls back to Community
// ============================================================================
TEST_F(LicenseRsaFixture, InvalidSignatureFallback) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    // Validator with a real public key — signature must match
    LicenseValidator v(public_pem_);
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":8,"exp":)" + std::to_string(unix_now(86400)) + "}";

    // Use an unsigned JWT — signature won't verify against our RSA key
    std::string bad_jwt = make_unsigned_jwt(payload);
    EXPECT_FALSE(v.load(bad_jwt));
    EXPECT_EQ(v.edition(), Edition::COMMUNITY);
    EXPECT_EQ(v.maxNodes(), 1);
    EXPECT_FALSE(v.hasFeature(Feature::CLUSTER));
}

TEST_F(LicenseRsaFixture, ValidSignatureAccepted) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v(public_pem_);
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER) |
                     static_cast<uint32_t>(Feature::SSO);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":16,"company":"Verified","exp":)" + std::to_string(unix_now(86400)) + "}";

    EXPECT_TRUE(v.load(sign_jwt(payload)));
    EXPECT_EQ(v.edition(), Edition::ENTERPRISE);
    EXPECT_EQ(v.maxNodes(), 16);
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
}

// ============================================================================
// 7. hasFeature() bitmask checks
// ============================================================================
TEST(LicenseTest, FeatureBitmaskIndividual) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    // Set every feature bit
    uint32_t all = static_cast<uint32_t>(Feature::CLUSTER) |
                   static_cast<uint32_t>(Feature::SSO) |
                   static_cast<uint32_t>(Feature::AUDIT_EXPORT) |
                   static_cast<uint32_t>(Feature::ADVANCED_RBAC) |
                   static_cast<uint32_t>(Feature::KAFKA) |
                   static_cast<uint32_t>(Feature::MIGRATION) |
                   static_cast<uint32_t>(Feature::GEO_REPLICATION) |
                   static_cast<uint32_t>(Feature::ROLLING_UPGRADE);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(all) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";

    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
    EXPECT_TRUE(v.hasFeature(Feature::SSO));
    EXPECT_TRUE(v.hasFeature(Feature::AUDIT_EXPORT));
    EXPECT_TRUE(v.hasFeature(Feature::ADVANCED_RBAC));
    EXPECT_TRUE(v.hasFeature(Feature::KAFKA));
    EXPECT_TRUE(v.hasFeature(Feature::MIGRATION));
    EXPECT_TRUE(v.hasFeature(Feature::GEO_REPLICATION));
    EXPECT_TRUE(v.hasFeature(Feature::ROLLING_UPGRADE));
}

TEST(LicenseTest, FeatureBitmaskSingleBit) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    // Only KAFKA enabled
    uint32_t feats = static_cast<uint32_t>(Feature::KAFKA);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";

    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v.hasFeature(Feature::KAFKA));
    EXPECT_FALSE(v.hasFeature(Feature::CLUSTER));
    EXPECT_FALSE(v.hasFeature(Feature::SSO));
    EXPECT_FALSE(v.hasFeature(Feature::GEO_REPLICATION));
}

TEST(LicenseTest, FeatureZeroBitmask) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    std::string payload = R"({"edition":"enterprise","features":0,"max_nodes":2,"exp":)" +
        std::to_string(unix_now(86400)) + "}";

    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_FALSE(v.hasFeature(Feature::CLUSTER));
    EXPECT_FALSE(v.hasFeature(Feature::SSO));
    EXPECT_FALSE(v.hasFeature(Feature::KAFKA));
}

// ============================================================================
// 8. statusLine() format
// ============================================================================
TEST(LicenseTest, StatusLineCommunity) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v;
    v.load("");
    std::string line = v.statusLine();
    EXPECT_NE(line.find("Community"), std::string::npos);
    EXPECT_NE(line.find("ZeptoDB"), std::string::npos);
}

TEST(LicenseTest, StatusLineEnterprise) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":16,"company":"Acme Corp","exp":)" + std::to_string(unix_now(86400)) + "}";

    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    std::string line = v.statusLine();
    EXPECT_NE(line.find("Enterprise"), std::string::npos);
    EXPECT_NE(line.find("Acme Corp"), std::string::npos);
    EXPECT_NE(line.find("16 nodes"), std::string::npos);
    EXPECT_NE(line.find("expires"), std::string::npos);
}

TEST(LicenseTest, StatusLineSingleNodeNoNodeCount) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    std::string payload = R"({"edition":"enterprise","features":0,"max_nodes":1,"company":"Solo","exp":)" +
        std::to_string(unix_now(86400)) + "}";

    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    std::string line = v.statusLine();
    EXPECT_NE(line.find("Enterprise"), std::string::npos);
    // max_nodes=1 should NOT show "nodes" in status
    EXPECT_EQ(line.find("nodes"), std::string::npos);
}

// ============================================================================
// Edge cases
// ============================================================================
TEST(LicenseTest, NoExpiryNeverExpires) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    // exp=0 means no expiry
    std::string payload = R"({"edition":"enterprise","features":1,"max_nodes":4,"exp":0})";
    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_FALSE(v.isExpired());
    EXPECT_FALSE(v.inGracePeriod());
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
}

TEST(LicenseTest, DefaultGraceDays30) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    // No grace_days in payload → default 30
    std::string payload = R"({"edition":"enterprise","features":1,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(-10 * 86400)) + "}";
    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_EQ(v.claims().grace_days, 30);
    EXPECT_TRUE(v.inGracePeriod());  // 10 days < 30 day grace
}

TEST(LicenseTest, MaxNodesDefaultsToOne) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    // No max_nodes in payload
    std::string payload = R"({"edition":"enterprise","features":0,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_EQ(v.maxNodes(), 1);
}

TEST(LicenseTest, MalformedJwtFallback) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    EXPECT_FALSE(v.load("not-a-jwt"));
    EXPECT_EQ(v.edition(), Edition::COMMUNITY);
}

TEST(LicenseTest, EnvVarLoading) {
    setenv("ZEPTODB_LICENSE_KEY", "single_segment_no_dots", 1);
    LicenseValidator v("");
    // No dots → decode_and_verify fails → Community fallback
    EXPECT_FALSE(v.load(""));
    EXPECT_EQ(v.edition(), Edition::COMMUNITY);
    unsetenv("ZEPTODB_LICENSE_KEY");
}

// ============================================================================
// Feature Gate: SSO
// ============================================================================
TEST(LicenseTest, FeatureGateSSO) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    // Community (no features) → SSO gated
    LicenseValidator v_community("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_community.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_community.hasFeature(Feature::SSO));

    // Enterprise with SSO bit → allowed
    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::SSO);
    std::string payload_sso = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_sso));
    EXPECT_TRUE(v_ent.hasFeature(Feature::SSO));
}

// ============================================================================
// Feature Gate: Audit Export
// ============================================================================
TEST(LicenseTest, FeatureGateAuditExport) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v_community("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_community.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_community.hasFeature(Feature::AUDIT_EXPORT));

    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::AUDIT_EXPORT);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v_ent.hasFeature(Feature::AUDIT_EXPORT));
}

// ============================================================================
// Feature Gate: Kafka
// ============================================================================
TEST(LicenseTest, FeatureGateKafka) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v_community("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_community.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_community.hasFeature(Feature::KAFKA));

    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::KAFKA);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v_ent.hasFeature(Feature::KAFKA));
}

// ============================================================================
// Feature Gate: Migration
// ============================================================================
TEST(LicenseTest, FeatureGateMigration) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v_community("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_community.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_community.hasFeature(Feature::MIGRATION));

    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::MIGRATION);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v_ent.hasFeature(Feature::MIGRATION));
}

// ============================================================================
// Backward compat: "pro" edition string → Enterprise (2-tier consolidation)
// ============================================================================
TEST(LicenseTest, ProEditionMapsToEnterprise) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER);
    std::string payload = R"({"edition":"pro","features":)" + std::to_string(feats) +
        R"(,"max_nodes":8,"company":"LegacyCo","exp":)" + std::to_string(unix_now(86400)) + "}";

    EXPECT_TRUE(v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload)));
    EXPECT_EQ(v.edition(), Edition::ENTERPRISE);
    EXPECT_EQ(v.maxNodes(), 8);
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
}

// ============================================================================
// Startup Banner tests
// ============================================================================
TEST(LicenseTest, StartupBannerCommunity) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v;
    v.load("");
    std::string banner = v.startupBanner();
    EXPECT_NE(banner.find("Community"), std::string::npos);
    EXPECT_NE(banner.find("Upgrade"), std::string::npos);
    EXPECT_NE(banner.find("https://zeptodb.com/pricing"), std::string::npos);
}

TEST(LicenseTest, StartupBannerEnterprise) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":16,"company":"Acme Corp","exp":)" + std::to_string(unix_now(86400)) + "}";
    v.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    std::string banner = v.startupBanner();
    EXPECT_NE(banner.find("Enterprise"), std::string::npos);
    EXPECT_NE(banner.find("Acme Corp"), std::string::npos);
    EXPECT_EQ(banner.find("Upgrade"), std::string::npos);
}

// ============================================================================
// Trial Key tests
// ============================================================================
TEST(LicenseTest, TrialKeyGeneration) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    std::string trial_jwt = LicenseValidator::generate_trial_key("TestTrial");
    EXPECT_FALSE(trial_jwt.empty());
    // Should have two dots (header.payload.)
    EXPECT_NE(trial_jwt.find('.'), std::string::npos);

    LicenseValidator v;
    EXPECT_TRUE(v.load(trial_jwt));
    EXPECT_EQ(v.edition(), Edition::ENTERPRISE);
    EXPECT_TRUE(v.isTrial());
    EXPECT_EQ(v.maxNodes(), 1);
    EXPECT_TRUE(v.hasFeature(Feature::CLUSTER));
    EXPECT_TRUE(v.hasFeature(Feature::SSO));
}

TEST(LicenseTest, TrialKeyExpiry) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    // Build a trial JWT with expiry in the past (beyond grace)
    auto header = b64url_str(R"({"alg":"none","typ":"JWT"})");
    std::string payload_json = R"({"edition":"enterprise","features":255,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(-60 * 86400)) + R"(,"trial":true,"grace_days":30})";
    auto payload = b64url_str(payload_json);
    std::string expired_trial = header + "." + payload + ".";

    LicenseValidator v;
    // Should fail to load — expired beyond grace
    EXPECT_FALSE(v.load(expired_trial));
}

TEST(LicenseTest, StatusLineTrialSuffix) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    std::string trial_jwt = LicenseValidator::generate_trial_key();
    LicenseValidator v;
    v.load(trial_jwt);
    std::string line = v.statusLine();
    EXPECT_NE(line.find("Trial"), std::string::npos);
    EXPECT_NE(line.find("Enterprise"), std::string::npos);
}

// ============================================================================
// Feature Gate: Cluster (Batch 3)
// ============================================================================
TEST(LicenseTest, FeatureGateCluster) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v_none("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_none.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_none.hasFeature(Feature::CLUSTER));

    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::CLUSTER);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":8,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v_ent.hasFeature(Feature::CLUSTER));
}

// ============================================================================
// Feature Gate: Rolling Upgrade (Batch 3)
// ============================================================================
TEST(LicenseTest, FeatureGateRollingUpgrade) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v_none("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_none.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_none.hasFeature(Feature::ROLLING_UPGRADE));

    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::ROLLING_UPGRADE);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v_ent.hasFeature(Feature::ROLLING_UPGRADE));
}

// ============================================================================
// Feature Gate: Advanced RBAC (Batch 3)
// ============================================================================
TEST(LicenseTest, FeatureGateAdvancedRBAC) {
    unsetenv("ZEPTODB_LICENSE_KEY");
    LicenseValidator v_none("");
    std::string payload_none = R"({"edition":"enterprise","features":0,"max_nodes":1,"exp":)" +
        std::to_string(unix_now(86400)) + "}";
    v_none.load_from_jwt_string_for_testing(make_unsigned_jwt(payload_none));
    EXPECT_FALSE(v_none.hasFeature(Feature::ADVANCED_RBAC));

    LicenseValidator v_ent("");
    uint32_t feats = static_cast<uint32_t>(Feature::ADVANCED_RBAC);
    std::string payload = R"({"edition":"enterprise","features":)" + std::to_string(feats) +
        R"(,"max_nodes":1,"exp":)" + std::to_string(unix_now(86400)) + "}";
    v_ent.load_from_jwt_string_for_testing(make_unsigned_jwt(payload));
    EXPECT_TRUE(v_ent.hasFeature(Feature::ADVANCED_RBAC));
}