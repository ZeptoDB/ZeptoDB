// ============================================================================
// ZeptoDB: Authentication & Authorization Tests
// ============================================================================
// Tests for:
//   - RBAC role/permission model
//   - ApiKeyStore: create, validate, revoke
//   - JwtValidator: HS256, RS256, expiry, claims
//   - AuthManager: middleware logic, public paths, JWT vs API key priority
//   - RateLimiter: token bucket, per-identity, per-IP
//   - CancellationToken: cancel/check
//   - QueryTracker: register, list, cancel, complete
//   - SecretsProvider: env, file, composite chain
//   - AuditBuffer: push, last(), ring capacity
// ============================================================================

#include <gtest/gtest.h>
#include "zeptodb/auth/rbac.h"
#include "zeptodb/auth/api_key_store.h"
#include "zeptodb/auth/jwt_validator.h"
#include "zeptodb/auth/auth_manager.h"
#include "zeptodb/auth/cancellation_token.h"
#include "zeptodb/auth/rate_limiter.h"
#include "zeptodb/auth/query_tracker.h"
#include "zeptodb/auth/secrets_provider.h"
#include "zeptodb/auth/audit_buffer.h"
#include "zeptodb/auth/tenant_manager.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bio.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace zeptodb::auth;

// ============================================================================
// Helpers
// ============================================================================

static std::string base64url_encode(const unsigned char* data, size_t len) {
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

static std::string base64url_str(const std::string& s) {
    return base64url_encode(reinterpret_cast<const unsigned char*>(s.data()),
                            s.size());
}

// Build a HS256 JWT with the given payload JSON and secret.
static std::string make_hs256_jwt(const std::string& payload_json,
                                   const std::string& secret)
{
    std::string header  = base64url_str(R"({"alg":"HS256","typ":"JWT"})");
    std::string payload = base64url_str(payload_json);
    std::string hp = header + "." + payload;

    unsigned int hmac_len = 0;
    unsigned char hmac_buf[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(hp.data()), hp.size(),
         hmac_buf, &hmac_len);

    return hp + "." + base64url_encode(hmac_buf, hmac_len);
}

// Current Unix time + offset seconds
static int64_t unix_now(int64_t offset_sec = 0) {
    using namespace std::chrono;
    return duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count() + offset_sec;
}

// Temporary file path (deleted in TearDown)
static std::string tmp_key_file() {
    return "/tmp/zepto_test_keys_" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           ".txt";
}

// ============================================================================
// RBAC Tests
// ============================================================================
TEST(RbacTest, AdminHasAllPermissions) {
    auto p = role_permissions(Role::ADMIN);
    EXPECT_TRUE(p & Permission::READ);
    EXPECT_TRUE(p & Permission::WRITE);
    EXPECT_TRUE(p & Permission::ADMIN);
    EXPECT_TRUE(p & Permission::METRICS);
}

TEST(RbacTest, ReaderHasOnlyRead) {
    auto p = role_permissions(Role::READER);
    EXPECT_TRUE(p & Permission::READ);
    EXPECT_FALSE(p & Permission::WRITE);
    EXPECT_FALSE(p & Permission::ADMIN);
    EXPECT_FALSE(p & Permission::METRICS);
}

TEST(RbacTest, WriterHasReadWriteMetrics) {
    auto p = role_permissions(Role::WRITER);
    EXPECT_TRUE(p & Permission::READ);
    EXPECT_TRUE(p & Permission::WRITE);
    EXPECT_TRUE(p & Permission::METRICS);
    EXPECT_FALSE(p & Permission::ADMIN);
}

TEST(RbacTest, MetricsRoleOnlyMetrics) {
    auto p = role_permissions(Role::METRICS);
    EXPECT_TRUE(p & Permission::METRICS);
    EXPECT_FALSE(p & Permission::READ);
    EXPECT_FALSE(p & Permission::WRITE);
    EXPECT_FALSE(p & Permission::ADMIN);
}

TEST(RbacTest, RoleRoundtrip) {
    EXPECT_EQ(role_from_string(role_to_string(Role::ADMIN)),   Role::ADMIN);
    EXPECT_EQ(role_from_string(role_to_string(Role::WRITER)),  Role::WRITER);
    EXPECT_EQ(role_from_string(role_to_string(Role::READER)),  Role::READER);
    EXPECT_EQ(role_from_string(role_to_string(Role::ANALYST)), Role::ANALYST);
    EXPECT_EQ(role_from_string(role_to_string(Role::METRICS)), Role::METRICS);
    EXPECT_EQ(role_from_string("garbage"), Role::UNKNOWN);
}

// ============================================================================
// ApiKeyStore Tests
// ============================================================================
class ApiKeyTest : public ::testing::Test {
protected:
    std::string path_;
    void SetUp() override { path_ = tmp_key_file(); }
    void TearDown() override { std::filesystem::remove(path_); }
};

TEST_F(ApiKeyTest, CreateAndValidate) {
    ApiKeyStore store(path_);
    std::string key = store.create_key("test-service", Role::READER);

    EXPECT_EQ(key.substr(0, 6), "zepto_");
    EXPECT_EQ(key.size(), 6 + 64u);  // "zepto_" + 64 hex chars

    auto entry = store.validate(key);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, "test-service");
    EXPECT_EQ(entry->role, Role::READER);
    EXPECT_TRUE(entry->enabled);
}

TEST_F(ApiKeyTest, InvalidKeyReturnsNullopt) {
    ApiKeyStore store(path_);
    EXPECT_FALSE(store.validate("zepto_invalid_key").has_value());
    EXPECT_FALSE(store.validate("").has_value());
    EXPECT_FALSE(store.validate("Bearer zepto_abc").has_value());
}

TEST_F(ApiKeyTest, RevokedKeyFails) {
    ApiKeyStore store(path_);
    std::string key = store.create_key("svc", Role::WRITER);
    auto entry = store.validate(key);
    ASSERT_TRUE(entry.has_value());

    bool revoked = store.revoke(entry->id);
    EXPECT_TRUE(revoked);
    EXPECT_FALSE(store.validate(key).has_value());
}

TEST_F(ApiKeyTest, RevokeNonexistentReturnsFalse) {
    ApiKeyStore store(path_);
    EXPECT_FALSE(store.revoke("ak_nonexistent"));
}

TEST_F(ApiKeyTest, SymbolWhitelistPreserved) {
    ApiKeyStore store(path_);
    std::vector<std::string> syms = {"AAPL", "GOOG", "MSFT"};
    std::string key = store.create_key("analyst", Role::ANALYST, syms);

    auto entry = store.validate(key);
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry->allowed_symbols.size(), 3u);
    EXPECT_EQ(entry->allowed_symbols[0], "AAPL");
    EXPECT_EQ(entry->allowed_symbols[1], "GOOG");
    EXPECT_EQ(entry->allowed_symbols[2], "MSFT");
}

TEST_F(ApiKeyTest, PersistenceAcrossInstances) {
    std::string key;
    {
        ApiKeyStore store(path_);
        key = store.create_key("persistent-svc", Role::ADMIN);
    }
    // New instance reads the same file
    ApiKeyStore store2(path_);
    auto entry = store2.validate(key);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, "persistent-svc");
    EXPECT_EQ(entry->role, Role::ADMIN);
}

TEST_F(ApiKeyTest, Sha256Hex) {
    std::string h = ApiKeyStore::sha256_hex("abc");
    // SHA256 always produces 32 bytes = 64 hex chars
    EXPECT_EQ(h.size(), 64u);
    // Must be lowercase hex only
    for (char c : h)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
    // Idempotent
    EXPECT_EQ(ApiKeyStore::sha256_hex("abc"), h);
    // Different inputs produce different hashes
    EXPECT_NE(ApiKeyStore::sha256_hex("abc"), ApiKeyStore::sha256_hex("def"));
    EXPECT_NE(ApiKeyStore::sha256_hex(""),    ApiKeyStore::sha256_hex("abc"));
}

// ============================================================================
// JwtValidator Tests (HS256)
// ============================================================================
class JwtHs256Test : public ::testing::Test {
protected:
    static constexpr const char* SECRET = "zepto_test_hs256_secret";
    JwtValidator::Config cfg_;
    void SetUp() override {
        cfg_.hs256_secret     = SECRET;
        cfg_.expected_issuer  = "test-issuer";
        cfg_.verify_expiry    = true;
    }
};

TEST_F(JwtHs256Test, ValidTokenDecodesClaims) {
    std::string payload = R"({"sub":"user1","zepto_role":"reader","exp":)"
                        + std::to_string(unix_now(3600))
                        + R"(,"iss":"test-issuer"})";
    std::string token = make_hs256_jwt(payload, SECRET);

    JwtValidator v(cfg_);
    auto claims = v.validate(token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "user1");
    EXPECT_EQ(claims->role, Role::READER);
    EXPECT_EQ(claims->issuer, "test-issuer");
}

TEST_F(JwtHs256Test, AdminRole) {
    std::string payload = R"({"sub":"admin1","zepto_role":"admin","exp":)"
                        + std::to_string(unix_now(3600))
                        + R"(,"iss":"test-issuer"})";
    auto claims = JwtValidator(cfg_).validate(make_hs256_jwt(payload, SECRET));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->role, Role::ADMIN);
}

TEST_F(JwtHs256Test, ExpiredTokenReturnsNullopt) {
    std::string payload = R"({"sub":"u","zepto_role":"reader","exp":)"
                        + std::to_string(unix_now(-3600))  // 1 hour ago
                        + R"(,"iss":"test-issuer"})";
    JwtValidator v(cfg_);
    EXPECT_FALSE(v.validate(make_hs256_jwt(payload, SECRET)).has_value());
}

TEST_F(JwtHs256Test, WrongIssuerReturnsNullopt) {
    std::string payload = R"({"sub":"u","zepto_role":"reader","exp":)"
                        + std::to_string(unix_now(3600))
                        + R"(,"iss":"wrong-issuer"})";
    JwtValidator v(cfg_);
    EXPECT_FALSE(v.validate(make_hs256_jwt(payload, SECRET)).has_value());
}

TEST_F(JwtHs256Test, WrongSecretReturnsNullopt) {
    std::string payload = R"({"sub":"u","zepto_role":"reader","exp":)"
                        + std::to_string(unix_now(3600))
                        + R"(,"iss":"test-issuer"})";
    JwtValidator v(cfg_);
    EXPECT_FALSE(v.validate(make_hs256_jwt(payload, "wrong_secret")).has_value());
}

TEST_F(JwtHs256Test, SymbolClaimParsed) {
    std::string payload = R"({"sub":"a","zepto_role":"analyst","zepto_symbols":"AAPL,GOOG","exp":)"
                        + std::to_string(unix_now(3600))
                        + R"(,"iss":"test-issuer"})";
    auto claims = JwtValidator(cfg_).validate(make_hs256_jwt(payload, SECRET));
    ASSERT_TRUE(claims.has_value());
    ASSERT_EQ(claims->allowed_symbols.size(), 2u);
    EXPECT_EQ(claims->allowed_symbols[0], "AAPL");
    EXPECT_EQ(claims->allowed_symbols[1], "GOOG");
}

TEST_F(JwtHs256Test, SkipExpiryValidation) {
    cfg_.verify_expiry = false;
    std::string payload = R"({"sub":"u","zepto_role":"reader","exp":1,"iss":"test-issuer"})";
    JwtValidator v(cfg_);
    // expired, but verification disabled
    EXPECT_TRUE(v.validate(make_hs256_jwt(payload, SECRET)).has_value());
}

TEST_F(JwtHs256Test, MalformedTokenReturnsNullopt) {
    JwtValidator v(cfg_);
    EXPECT_FALSE(v.validate("not.a.jwt").has_value());
    EXPECT_FALSE(v.validate("").has_value());
    EXPECT_FALSE(v.validate("only-one-part").has_value());
}

TEST_F(JwtHs256Test, AudienceValidation) {
    cfg_.expected_audience = "zeptodb";
    std::string payload = R"({"sub":"u","zepto_role":"reader","exp":)"
                        + std::to_string(unix_now(3600))
                        + R"(,"iss":"test-issuer","aud":"zeptodb"})";
    JwtValidator v(cfg_);
    EXPECT_TRUE(v.validate(make_hs256_jwt(payload, SECRET)).has_value());

    // Wrong audience
    std::string bad_payload = R"({"sub":"u","zepto_role":"reader","exp":)"
                            + std::to_string(unix_now(3600))
                            + R"(,"iss":"test-issuer","aud":"other-service"})";
    EXPECT_FALSE(v.validate(make_hs256_jwt(bad_payload, SECRET)).has_value());
}

// ============================================================================
// JwtValidator Tests (RS256)
// ============================================================================
class JwtRs256Test : public ::testing::Test {
protected:
    std::string private_key_pem_;
    std::string public_key_pem_;

    void SetUp() override {
        // Generate a 2048-bit RSA key pair for testing
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
        EVP_PKEY* pkey = nullptr;
        EVP_PKEY_keygen(ctx, &pkey);
        EVP_PKEY_CTX_free(ctx);

        // Export private key
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        char* data; long len = BIO_get_mem_data(bio, &data);
        private_key_pem_ = std::string(data, len);
        BIO_free(bio);

        // Export public key
        bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(bio, pkey);
        len = BIO_get_mem_data(bio, &data);
        public_key_pem_ = std::string(data, len);
        BIO_free(bio);

        EVP_PKEY_free(pkey);
    }

    std::string make_rs256_jwt(const std::string& payload_json) const {
        std::string header  = base64url_str(R"({"alg":"RS256","typ":"JWT"})");
        std::string payload = base64url_str(payload_json);
        std::string hp = header + "." + payload;

        // Load private key and sign
        BIO* bio = BIO_new_mem_buf(private_key_pem_.data(),
                                   static_cast<int>(private_key_pem_.size()));
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
        EVP_DigestSignUpdate(ctx,
            reinterpret_cast<const unsigned char*>(hp.data()), hp.size());

        size_t sig_len = 0;
        EVP_DigestSignFinal(ctx, nullptr, &sig_len);
        std::vector<unsigned char> sig(sig_len);
        EVP_DigestSignFinal(ctx, sig.data(), &sig_len);
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);

        return hp + "." + base64url_encode(sig.data(), sig_len);
    }
};

TEST_F(JwtRs256Test, ValidRS256Token) {
    std::string payload = R"({"sub":"rsauser","zepto_role":"writer","exp":)"
                        + std::to_string(unix_now(3600)) + "}";
    std::string token = make_rs256_jwt(payload);

    JwtValidator::Config cfg;
    cfg.rs256_public_key_pem = public_key_pem_;
    JwtValidator v(cfg);

    auto claims = v.validate(token);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "rsauser");
    EXPECT_EQ(claims->role, Role::WRITER);
}

TEST_F(JwtRs256Test, TamperedPayloadFails) {
    std::string payload = R"({"sub":"user","zepto_role":"reader","exp":)"
                        + std::to_string(unix_now(3600)) + "}";
    std::string token = make_rs256_jwt(payload);

    // Replace "reader" with "admin" in the payload segment
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    std::string tampered_payload = base64url_str(
        R"({"sub":"user","zepto_role":"admin","exp":)" +
        std::to_string(unix_now(3600)) + "}");
    token = token.substr(0, dot1 + 1) + tampered_payload +
            token.substr(dot2);

    JwtValidator::Config cfg;
    cfg.rs256_public_key_pem = public_key_pem_;
    EXPECT_FALSE(JwtValidator(cfg).validate(token).has_value());
}

// ============================================================================
// Base64url decode Tests
// ============================================================================
TEST(Base64urlTest, DecodeKnownValue) {
    // base64url("hello") = "aGVsbG8"
    EXPECT_EQ(JwtValidator::base64url_decode("aGVsbG8"), "hello");
}

TEST(Base64urlTest, DecodeWithPaddingEquivalent) {
    // base64url of "{}" = "e30"
    EXPECT_EQ(JwtValidator::base64url_decode("e30"), "{}");
}

// ============================================================================
// AuthContext Tests
// ============================================================================
TEST(AuthContextTest, PermissionCheck) {
    AuthContext ctx;
    ctx.role = Role::READER;
    EXPECT_TRUE(ctx.has_permission(Permission::READ));
    EXPECT_FALSE(ctx.has_permission(Permission::WRITE));
    EXPECT_FALSE(ctx.has_permission(Permission::ADMIN));
}

TEST(AuthContextTest, SymbolAccessUnrestricted) {
    AuthContext ctx;
    ctx.role = Role::READER;
    // Empty allowed_symbols → all symbols accessible
    EXPECT_TRUE(ctx.can_access_symbol("AAPL"));
    EXPECT_TRUE(ctx.can_access_symbol("GOOG"));
    EXPECT_TRUE(ctx.can_access_symbol("ANY"));
}

TEST(AuthContextTest, SymbolAccessRestricted) {
    AuthContext ctx;
    ctx.role = Role::ANALYST;
    ctx.allowed_symbols = {"AAPL", "GOOG"};
    EXPECT_TRUE(ctx.can_access_symbol("AAPL"));
    EXPECT_TRUE(ctx.can_access_symbol("GOOG"));
    EXPECT_FALSE(ctx.can_access_symbol("TSLA"));
}

// ============================================================================
// AuthManager Tests
// ============================================================================
class AuthManagerTest : public ::testing::Test {
protected:
    std::string key_file_;
    void SetUp() override { key_file_ = tmp_key_file(); }
    void TearDown() override { std::filesystem::remove(key_file_); }

    AuthManager make_manager() {
        AuthManager::Config cfg;
        cfg.enabled       = true;
        cfg.api_keys_file = key_file_;
        cfg.audit_enabled = false;
        return AuthManager(std::move(cfg));
    }
};

TEST_F(AuthManagerTest, PublicPathsAlwaysAllowed) {
    AuthManager mgr = make_manager();
    for (const auto& path : {"/ping", "/health", "/ready"}) {
        auto d = mgr.check("GET", path, "", "127.0.0.1");
        EXPECT_EQ(d.status, AuthStatus::OK) << "path: " << path;
    }
}

TEST_F(AuthManagerTest, NoHeaderReturnsUnauthorized) {
    AuthManager mgr = make_manager();
    auto d = mgr.check("POST", "/", "", "127.0.0.1");
    EXPECT_EQ(d.status, AuthStatus::UNAUTHORIZED);
}

TEST_F(AuthManagerTest, InvalidApiKeyReturnsUnauthorized) {
    AuthManager mgr = make_manager();
    auto d = mgr.check("POST", "/", "Bearer zepto_invalid", "127.0.0.1");
    EXPECT_EQ(d.status, AuthStatus::UNAUTHORIZED);
}

TEST_F(AuthManagerTest, ValidApiKeyGrantsAccess) {
    AuthManager mgr = make_manager();
    std::string key = mgr.create_api_key("test", Role::READER);

    auto d = mgr.check("POST", "/", "Bearer " + key, "127.0.0.1");
    EXPECT_EQ(d.status, AuthStatus::OK);
    EXPECT_EQ(d.context.role, Role::READER);
    EXPECT_EQ(d.context.source, "api_key");
}

TEST_F(AuthManagerTest, RevokedKeyReturnsUnauthorized) {
    AuthManager mgr = make_manager();
    std::string key = mgr.create_api_key("svc", Role::WRITER);

    auto entries = mgr.list_api_keys();
    ASSERT_EQ(entries.size(), 1u);
    mgr.revoke_api_key(entries[0].id);

    auto d = mgr.check("POST", "/", "Bearer " + key, "127.0.0.1");
    EXPECT_EQ(d.status, AuthStatus::UNAUTHORIZED);
}

TEST_F(AuthManagerTest, ContextHasCorrectRole) {
    AuthManager mgr = make_manager();
    std::string key = mgr.create_api_key("admin-svc", Role::ADMIN);

    auto d = mgr.check("POST", "/", "Bearer " + key, "");
    ASSERT_EQ(d.status, AuthStatus::OK);
    EXPECT_TRUE(d.context.has_permission(Permission::ADMIN));
    EXPECT_TRUE(d.context.has_permission(Permission::READ));
    EXPECT_TRUE(d.context.has_permission(Permission::WRITE));
}

TEST_F(AuthManagerTest, AuthDisabledAllowsAll) {
    AuthManager::Config cfg;
    cfg.enabled       = false;
    cfg.api_keys_file = key_file_;
    cfg.audit_enabled = false;
    AuthManager mgr(cfg);

    auto d = mgr.check("POST", "/", "", "127.0.0.1");
    EXPECT_EQ(d.status, AuthStatus::OK);
    EXPECT_EQ(d.context.role, Role::ADMIN);
}

TEST_F(AuthManagerTest, JwtBeatsApiKeyWhenBothConfigured) {
    static constexpr const char* SECRET = "test-jwt-secret";

    AuthManager::Config cfg;
    cfg.enabled       = true;
    cfg.api_keys_file = key_file_;
    cfg.jwt_enabled   = true;
    cfg.jwt.hs256_secret = SECRET;
    cfg.audit_enabled = false;
    AuthManager mgr(cfg);

    // Create a JWT that starts with "ey" — should be tried first
    std::string payload = R"({"sub":"jwt_user","zepto_role":"writer","exp":)"
                        + std::to_string(unix_now(3600)) + "}";
    std::string token = make_hs256_jwt(payload, SECRET);

    auto d = mgr.check("POST", "/", "Bearer " + token, "127.0.0.1");
    EXPECT_EQ(d.status, AuthStatus::OK);
    EXPECT_EQ(d.context.source, "jwt");
    EXPECT_EQ(d.context.role, Role::WRITER);
}

TEST_F(AuthManagerTest, MalformedBearerHeaderReturnsUnauthorized) {
    AuthManager mgr = make_manager();
    auto d1 = mgr.check("POST", "/", "Basic dXNlcjpwYXNz", "");
    EXPECT_EQ(d1.status, AuthStatus::UNAUTHORIZED);

    auto d2 = mgr.check("POST", "/", "NotBearer token", "");
    EXPECT_EQ(d2.status, AuthStatus::UNAUTHORIZED);
}

// ============================================================================
// CancellationToken Tests
// ============================================================================
TEST(CancellationTokenTest, InitiallyNotCancelled) {
    CancellationToken tok;
    EXPECT_FALSE(tok.is_cancelled());
}

TEST(CancellationTokenTest, CancelSetsFlag) {
    CancellationToken tok;
    tok.cancel();
    EXPECT_TRUE(tok.is_cancelled());
}

TEST(CancellationTokenTest, CancelIdempotent) {
    CancellationToken tok;
    tok.cancel();
    tok.cancel();
    EXPECT_TRUE(tok.is_cancelled());
}

TEST(CancellationTokenTest, ThreadedCancel) {
    auto token = std::make_shared<CancellationToken>();
    EXPECT_FALSE(token->is_cancelled());
    std::thread t([token]() { token->cancel(); });
    t.join();
    EXPECT_TRUE(token->is_cancelled());
}

// ============================================================================
// RateLimiter Tests
// ============================================================================
TEST(RateLimiterTest, AllowsUnderLimit) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 600;  // 10/sec
    cfg.burst_capacity      = 10;
    RateLimiter limiter(cfg);

    // Burst should all succeed immediately
    for (int i = 0; i < 10; ++i) {
        auto rd = limiter.check_identity("user1");
        EXPECT_EQ(rd, RateDecision::ALLOWED) << "request " << i;
    }
}

TEST(RateLimiterTest, BlocksAfterBurstExceeded) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 60;  // 1/sec
    cfg.burst_capacity      = 3;
    RateLimiter limiter(cfg);

    // Exhaust burst
    for (int i = 0; i < 3; ++i)
        limiter.check_identity("user2");

    // Next request should be rate-limited
    EXPECT_EQ(limiter.check_identity("user2"), RateDecision::RATE_LIMITED);
}

TEST(RateLimiterTest, DifferentIdentitiesIndependent) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 60;
    cfg.burst_capacity      = 2;
    RateLimiter limiter(cfg);

    // Exhaust user1
    limiter.check_identity("user1");
    limiter.check_identity("user1");
    EXPECT_EQ(limiter.check_identity("user1"), RateDecision::RATE_LIMITED);

    // user2 should still be allowed
    EXPECT_EQ(limiter.check_identity("user2"), RateDecision::ALLOWED);
}

TEST(RateLimiterTest, IpLimitIndependentOfIdentity) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 600;
    cfg.burst_capacity      = 100;
    cfg.per_ip_rpm          = 60;
    cfg.ip_burst            = 2;
    RateLimiter limiter(cfg);

    // Exhaust IP burst
    limiter.check_ip("1.2.3.4");
    limiter.check_ip("1.2.3.4");
    EXPECT_EQ(limiter.check_ip("1.2.3.4"), RateDecision::RATE_LIMITED);

    // Different IP unaffected
    EXPECT_EQ(limiter.check_ip("5.6.7.8"), RateDecision::ALLOWED);
}

TEST(RateLimiterTest, RefillsOverTime) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 6000;  // 100 req/sec (fast refill for test)
    cfg.burst_capacity      = 1;
    RateLimiter limiter(cfg);

    // Use the one token
    EXPECT_EQ(limiter.check_identity("u"), RateDecision::ALLOWED);
    EXPECT_EQ(limiter.check_identity("u"), RateDecision::RATE_LIMITED);

    // Wait 20ms — at 100 req/sec we should have ~2 tokens
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(limiter.check_identity("u"), RateDecision::ALLOWED);
}

TEST(RateLimiterTest, IpLimitDisabledWhenZero) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 60;
    cfg.burst_capacity      = 5;
    cfg.per_ip_rpm          = 0;  // disabled
    cfg.ip_burst            = 1;
    RateLimiter limiter(cfg);

    // Even after many calls, IP should never be rate-limited
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(limiter.check_ip("1.2.3.4"), RateDecision::ALLOWED);
}

TEST(RateLimiterTest, BucketCountTracked) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 6000;
    cfg.burst_capacity      = 100;
    RateLimiter limiter(cfg);

    EXPECT_EQ(limiter.identity_bucket_count(), 0u);
    limiter.check_identity("alpha");
    limiter.check_identity("beta");
    limiter.check_identity("gamma");
    EXPECT_EQ(limiter.identity_bucket_count(), 3u);
}

TEST(RateLimiterTest, CleanupRemovesStaleBuckets) {
    RateLimiter::Config cfg;
    cfg.requests_per_minute = 6000;
    cfg.burst_capacity      = 100;
    RateLimiter limiter(cfg);

    limiter.check_identity("stale_user");
    EXPECT_EQ(limiter.identity_bucket_count(), 1u);

    // cleanup with max_idle_sec=0 removes everything just accessed
    // (last_ns == now, but we ask max_idle < 0 sec → cutoff is in future)
    // Use a small sleep + max_idle_sec=0 to force eviction
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    limiter.cleanup(0);  // cutoff = now_ns − 0 = now → evict all older than now
    EXPECT_EQ(limiter.identity_bucket_count(), 0u);
}

TEST(RateLimiterTest, AuthManagerIntegration) {
    std::string key_file = tmp_key_file();

    AuthManager::Config cfg;
    cfg.enabled               = true;
    cfg.api_keys_file         = key_file;
    cfg.audit_enabled         = false;
    cfg.rate_limit_enabled    = true;
    cfg.rate_limit.requests_per_minute = 60;
    cfg.rate_limit.burst_capacity      = 2;

    AuthManager mgr(cfg);
    std::string key = mgr.create_api_key("rl-test", Role::READER);

    // First two requests allowed
    EXPECT_EQ(mgr.check("POST", "/", "Bearer " + key, "").status, AuthStatus::OK);
    EXPECT_EQ(mgr.check("POST", "/", "Bearer " + key, "").status, AuthStatus::OK);

    // Third request rate-limited
    auto d = mgr.check("POST", "/", "Bearer " + key, "");
    EXPECT_EQ(d.status, AuthStatus::FORBIDDEN);
    EXPECT_NE(d.reason.find("Rate limit"), std::string::npos);

    std::filesystem::remove(key_file);
}

// ============================================================================
// QueryTracker Tests
// ============================================================================
TEST(QueryTrackerTest, RegisterAndList) {
    QueryTracker tracker;
    auto token = std::make_shared<CancellationToken>();
    std::string id = tracker.register_query("user1", "SELECT * FROM trades", token);

    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.substr(0, 2), "q_");
    EXPECT_EQ(id.size(), 18u);  // "q_" + 16 hex chars

    EXPECT_EQ(tracker.active_count(), 1u);
    auto list = tracker.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].subject, "user1");
    EXPECT_EQ(list[0].query_id, id);
    EXPECT_FALSE(list[0].sql_preview.empty());
}

TEST(QueryTrackerTest, CompleteRemovesQuery) {
    QueryTracker tracker;
    auto token = std::make_shared<CancellationToken>();
    std::string id = tracker.register_query("user1", "SELECT 1", token);
    EXPECT_EQ(tracker.active_count(), 1u);

    tracker.complete(id);
    EXPECT_EQ(tracker.active_count(), 0u);
}

TEST(QueryTrackerTest, CompleteIdempotent) {
    QueryTracker tracker;
    auto token = std::make_shared<CancellationToken>();
    std::string id = tracker.register_query("u", "SELECT 1", token);
    tracker.complete(id);
    tracker.complete(id);  // Should not throw
    EXPECT_EQ(tracker.active_count(), 0u);
}

TEST(QueryTrackerTest, CancelSetsToken) {
    QueryTracker tracker;
    auto token = std::make_shared<CancellationToken>();
    EXPECT_FALSE(token->is_cancelled());

    std::string id = tracker.register_query("u", "SELECT * FROM trades", token);
    bool ok = tracker.cancel(id);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(token->is_cancelled());
}

TEST(QueryTrackerTest, CancelNonexistentReturnsFalse) {
    QueryTracker tracker;
    EXPECT_FALSE(tracker.cancel("q_nonexistent123456"));
}

TEST(QueryTrackerTest, MultipleQueriesTracked) {
    QueryTracker tracker;
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        auto tok = std::make_shared<CancellationToken>();
        ids.push_back(tracker.register_query(
            "user" + std::to_string(i), "SELECT " + std::to_string(i), tok));
    }
    EXPECT_EQ(tracker.active_count(), 5u);

    tracker.complete(ids[2]);
    EXPECT_EQ(tracker.active_count(), 4u);
}

TEST(QueryTrackerTest, SqlPreviewTruncated) {
    QueryTracker tracker;
    std::string long_sql(500, 'x');
    auto tok = std::make_shared<CancellationToken>();
    std::string id = tracker.register_query("u", long_sql, tok);

    auto list = tracker.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_LE(list[0].sql_preview.size(), QueryTracker::SQL_PREVIEW_LEN);
}

TEST(QueryTrackerTest, StartedAtNsIsSet) {
    QueryTracker tracker;
    auto tok = std::make_shared<CancellationToken>();

    int64_t before = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string id = tracker.register_query("u", "SELECT 1", tok);
    auto list = tracker.list();

    ASSERT_EQ(list.size(), 1u);
    EXPECT_GT(list[0].started_at_ns, 0);
    EXPECT_GE(list[0].started_at_ns, before);
}

// ============================================================================
// SecretsProvider Tests
// ============================================================================
TEST(EnvSecretsProviderTest, ReadsEnvVar) {
    setenv("APEX_TEST_SECRET_XYZ", "mysecretvalue", 1);
    EnvSecretsProvider p;
    EXPECT_EQ(p.get("APEX_TEST_SECRET_XYZ", "default"), "mysecretvalue");
    unsetenv("APEX_TEST_SECRET_XYZ");
}

TEST(EnvSecretsProviderTest, DefaultWhenMissing) {
    unsetenv("APEX_NONEXISTENT_SECRET_ZZZZ");
    EnvSecretsProvider p;
    EXPECT_EQ(p.get("APEX_NONEXISTENT_SECRET_ZZZZ", "fallback"), "fallback");
}

TEST(EnvSecretsProviderTest, AlwaysAvailable) {
    EnvSecretsProvider p;
    EXPECT_TRUE(p.available());
}

class FileSecretsTest : public ::testing::Test {
protected:
    std::string dir_;
    void SetUp() override {
        dir_ = "/tmp/zepto_secrets_test_" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch().count());
        std::filesystem::create_directory(dir_);
    }
    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }
};

TEST_F(FileSecretsTest, ReadsFile) {
    std::ofstream f(dir_ + "/mykey");
    f << "file_secret_value\n";
    f.close();

    FileSecretsProvider p(dir_);
    EXPECT_TRUE(p.available());
    EXPECT_EQ(p.get("mykey", "default"), "file_secret_value");
}

TEST_F(FileSecretsTest, StripsTrailingNewline) {
    std::ofstream f(dir_ + "/key_with_nl");
    f << "value\n\r\n";
    f.close();

    FileSecretsProvider p(dir_);
    EXPECT_EQ(p.get("key_with_nl", ""), "value");
}

TEST_F(FileSecretsTest, MissingFileReturnsDefault) {
    FileSecretsProvider p(dir_);
    EXPECT_EQ(p.get("nonexistent_key", "fallback"), "fallback");
}

TEST_F(FileSecretsTest, MissingDirNotAvailable) {
    FileSecretsProvider p("/tmp/no_such_dir_zepto_xyz");
    EXPECT_FALSE(p.available());
}

TEST(CompositeSecretsProviderTest, FirstAvailableWins) {
    // Env has the key
    setenv("APEX_COMPOSITE_TEST_KEY", "env_value", 1);

    auto composite = std::make_unique<CompositeSecretsProvider>();
    composite->add(std::make_unique<EnvSecretsProvider>());

    EXPECT_EQ(composite->get("APEX_COMPOSITE_TEST_KEY", "fallback"), "env_value");
    unsetenv("APEX_COMPOSITE_TEST_KEY");
}

TEST(CompositeSecretsProviderTest, FallsBackThroughChain) {
    // Neither Vault (addr empty → unavailable) nor env has this key
    auto composite = std::make_unique<CompositeSecretsProvider>();
    // Add Vault-like provider that is unavailable (empty config)
    VaultSecretsProvider::Config vcfg;  // empty addr → unavailable
    composite->add(std::make_unique<VaultSecretsProvider>(vcfg));
    composite->add(std::make_unique<EnvSecretsProvider>());

    unsetenv("APEX_FALLBACK_TEST_KEY_ZZZ");
    EXPECT_EQ(composite->get("APEX_FALLBACK_TEST_KEY_ZZZ", "default"), "default");
}

TEST(CompositeSecretsProviderTest, ActiveBackendsListing) {
    auto composite = std::make_unique<CompositeSecretsProvider>();
    composite->add(std::make_unique<EnvSecretsProvider>());

    auto backends = composite->active_backends();
    ASSERT_EQ(backends.size(), 1u);
    EXPECT_EQ(backends[0], "env");
}

TEST(VaultSecretsProviderTest, UnavailableWhenAddrEmpty) {
    VaultSecretsProvider::Config cfg;  // addr is empty
    VaultSecretsProvider p(cfg);
    EXPECT_FALSE(p.available());
}

TEST(VaultSecretsProviderTest, BackendNameContainsVault) {
    VaultSecretsProvider::Config cfg;
    cfg.addr = "https://vault.internal:8200";
    VaultSecretsProvider p(cfg);
    EXPECT_NE(p.backend_name().find("vault"), std::string::npos);
    EXPECT_NE(p.backend_name().find("vault.internal"), std::string::npos);
}

TEST(VaultSecretsProviderTest, AvailableWhenAddrSet) {
    VaultSecretsProvider::Config cfg;
    cfg.addr  = "https://vault.internal:8200";
    cfg.token = "s.test";
    VaultSecretsProvider p(cfg);
    EXPECT_TRUE(p.available());
    // Note: get() will fail (no real Vault), but available() just checks config
}

TEST(AwsSecretsProviderTest, AvailableWithRegion) {
    AwsSecretsProvider::Config cfg;
    cfg.region = "us-east-1";
    AwsSecretsProvider p(cfg);
    EXPECT_TRUE(p.available());
    EXPECT_NE(p.backend_name().find("aws-sm"), std::string::npos);
}

TEST(AwsSecretsProviderTest, UnavailableWhenRegionEmpty) {
    AwsSecretsProvider::Config cfg;
    cfg.region = "";
    AwsSecretsProvider p(cfg);
    EXPECT_FALSE(p.available());
}

TEST(SecretsProviderFactoryTest, CreateCompositeAlwaysHasEnvFallback) {
    // Ensure VAULT_ADDR is not set so we don't accidentally connect
    unsetenv("VAULT_ADDR");
    unsetenv("VAULT_TOKEN");

    auto composite = SecretsProviderFactory::create_composite();
    ASSERT_NE(composite, nullptr);
    EXPECT_TRUE(composite->available());

    // Env fallback should work
    setenv("APEX_FACTORY_TEST_KEY", "factory_val", 1);
    EXPECT_EQ(composite->get("APEX_FACTORY_TEST_KEY", "miss"), "factory_val");
    unsetenv("APEX_FACTORY_TEST_KEY");
}

TEST(SecretsProviderFactoryTest, CreateWithVaultHasVaultAndEnv) {
    VaultSecretsProvider::Config vcfg;
    vcfg.addr  = "https://vault.example.com";
    vcfg.token = "s.test";
    auto composite = SecretsProviderFactory::create_with_vault(vcfg);
    ASSERT_NE(composite, nullptr);

    auto backends = composite->active_backends();
    EXPECT_GE(backends.size(), 2u);  // vault + env at minimum

    bool has_vault = false, has_env = false;
    for (const auto& b : backends) {
        if (b.find("vault") != std::string::npos) has_vault = true;
        if (b == "env") has_env = true;
    }
    EXPECT_TRUE(has_vault);
    EXPECT_TRUE(has_env);
}

// ============================================================================
// AuditBuffer Tests
// ============================================================================
TEST(AuditBufferTest, PushAndRetrieve) {
    AuditBuffer buf;
    AuditEvent ev;
    ev.timestamp_ns = 12345;
    ev.subject      = "user1";
    ev.role_str     = "reader";
    ev.action       = "GET /";
    ev.detail       = "test";
    ev.remote_addr  = "127.0.0.1";
    buf.push(ev);

    EXPECT_EQ(buf.size(), 1u);
    auto events = buf.last(10);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].subject, "user1");
    EXPECT_EQ(events[0].timestamp_ns, 12345);
}

TEST(AuditBufferTest, LastNReturnsAtMostN) {
    AuditBuffer buf;
    for (int i = 0; i < 20; ++i) {
        AuditEvent ev;
        ev.subject = "u" + std::to_string(i);
        buf.push(ev);
    }
    auto events = buf.last(5);
    EXPECT_EQ(events.size(), 5u);
}

TEST(AuditBufferTest, CapacityEvictsOldest) {
    AuditBuffer buf(5);  // capacity = 5
    for (int i = 0; i < 7; ++i) {
        AuditEvent ev;
        ev.subject = "u" + std::to_string(i);
        buf.push(ev);
    }
    EXPECT_EQ(buf.size(), 5u);
    auto events = buf.last(10);
    // Oldest 2 (u0, u1) should have been evicted
    EXPECT_EQ(events[0].subject, "u2");
    EXPECT_EQ(events[4].subject, "u6");
}

TEST(AuditBufferTest, ClearWorks) {
    AuditBuffer buf;
    AuditEvent ev;
    ev.subject = "u";
    buf.push(ev);
    EXPECT_EQ(buf.size(), 1u);
    buf.clear();
    EXPECT_EQ(buf.size(), 0u);
}

TEST(AuditBufferTest, AuthManagerPushesToBuffer) {
    std::string key_file = tmp_key_file();
    AuthManager::Config cfg;
    cfg.enabled               = true;
    cfg.api_keys_file         = key_file;
    cfg.audit_enabled         = true;
    cfg.audit_buffer_enabled  = true;
    cfg.rate_limit_enabled    = false;

    AuthManager mgr(cfg);
    std::string key = mgr.create_api_key("buf-test", Role::READER);

    mgr.check("POST", "/", "Bearer " + key, "10.0.0.1");

    EXPECT_GE(mgr.audit_buffer().size(), 1u);
    auto events = mgr.audit_buffer().last(5);
    ASSERT_FALSE(events.empty());
    EXPECT_FALSE(events.back().subject.empty());

    std::filesystem::remove(key_file);
}

TEST(AuditBufferTest, ThreadSafeConcurrentPush) {
    AuditBuffer buf(10000);
    constexpr int N_THREADS = 8;
    constexpr int PER_THREAD = 100;

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&buf, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                AuditEvent ev;
                ev.subject = "thread" + std::to_string(t);
                ev.action  = "action" + std::to_string(i);
                buf.push(ev);
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(buf.size(), static_cast<size_t>(N_THREADS * PER_THREAD));
}

TEST(AuditBufferTest, LastZeroReturnsAll) {
    AuditBuffer buf;
    for (int i = 0; i < 5; ++i) {
        AuditEvent ev;
        ev.subject = "u" + std::to_string(i);
        buf.push(ev);
    }
    // last(0) should return all
    auto all = buf.last(0);
    EXPECT_EQ(all.size(), 5u);
}

TEST(AuditBufferTest, LastNLargerThanSizeReturnsAll) {
    AuditBuffer buf;
    for (int i = 0; i < 3; ++i) {
        AuditEvent ev;
        ev.subject = "u" + std::to_string(i);
        buf.push(ev);
    }
    auto events = buf.last(1000);
    EXPECT_EQ(events.size(), 3u);
}

// ============================================================================
// AuthManager edge-case Tests
// ============================================================================
TEST_F(AuthManagerTest, RateLimit_PublicPathNotCounted) {
    // Public paths should bypass rate limiting completely
    std::string key_file = tmp_key_file();
    AuthManager::Config cfg;
    cfg.enabled               = true;
    cfg.api_keys_file         = key_file;
    cfg.audit_enabled         = false;
    cfg.rate_limit_enabled    = true;
    cfg.rate_limit.requests_per_minute = 60;
    cfg.rate_limit.burst_capacity      = 1;

    AuthManager mgr(cfg);
    std::string key = mgr.create_api_key("rl-public", Role::READER);

    // Exhaust rate limit on real path
    EXPECT_EQ(mgr.check("POST", "/", "Bearer " + key, "").status, AuthStatus::OK);
    EXPECT_EQ(mgr.check("POST", "/", "Bearer " + key, "").status, AuthStatus::FORBIDDEN);

    // Public paths must always return OK regardless of rate limit state
    EXPECT_EQ(mgr.check("GET", "/ping",   "", "").status, AuthStatus::OK);
    EXPECT_EQ(mgr.check("GET", "/health", "", "").status, AuthStatus::OK);
    EXPECT_EQ(mgr.check("GET", "/ready",  "", "").status, AuthStatus::OK);

    std::filesystem::remove(key_file);
}

TEST_F(AuthManagerTest, CreateKeyIncreasesListSize) {
    AuthManager mgr = make_manager();
    EXPECT_EQ(mgr.list_api_keys().size(), 0u);
    mgr.create_api_key("svc1", Role::READER);
    EXPECT_EQ(mgr.list_api_keys().size(), 1u);
    mgr.create_api_key("svc2", Role::WRITER);
    EXPECT_EQ(mgr.list_api_keys().size(), 2u);
}

TEST_F(AuthManagerTest, RevokeNonexistentReturnsFalse) {
    AuthManager mgr = make_manager();
    EXPECT_FALSE(mgr.revoke_api_key("ak_nonexistent_id"));
}

// ============================================================================
// TenantManager Tests
// ============================================================================
TEST(TenantManagerTest, CreateAndList) {
    auto mgr = std::make_shared<TenantManager>();
    TenantConfig cfg;
    cfg.tenant_id = "test_1";
    cfg.name = "Test Tenant";
    cfg.max_concurrent_queries = 5;

    EXPECT_TRUE(mgr->create_tenant(cfg));
    EXPECT_FALSE(mgr->create_tenant(cfg)); // Duplicate ID fails

    auto list = mgr->list_tenants();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].tenant_id, "test_1");
    EXPECT_EQ(list[0].name, "Test Tenant");
}

TEST(TenantManagerTest, DropTenant) {
    auto mgr = std::make_shared<TenantManager>();
    TenantConfig cfg; cfg.tenant_id = "d1";
    mgr->create_tenant(cfg);
    EXPECT_TRUE(mgr->drop_tenant("d1"));
    EXPECT_FALSE(mgr->drop_tenant("d1"));
}

TEST(TenantManagerTest, QuotaEnforcement) {
    auto mgr = std::make_shared<TenantManager>();
    TenantConfig cfg;
    cfg.tenant_id = "q1";
    cfg.max_concurrent_queries = 2;
    mgr->create_tenant(cfg);

    EXPECT_TRUE(mgr->acquire_query_slot("q1"));
    EXPECT_TRUE(mgr->acquire_query_slot("q1"));
    EXPECT_FALSE(mgr->acquire_query_slot("q1")); // 3rd fails

    mgr->release_query_slot("q1");
    EXPECT_TRUE(mgr->acquire_query_slot("q1")); // Now succeeds

    auto usage = mgr->usage("q1");
    ASSERT_NE(usage, nullptr);
    EXPECT_EQ(usage->active_queries.load(), 2u);
}

TEST(TenantManagerTest, TableNamespace) {
    auto mgr = std::make_shared<TenantManager>();
    TenantConfig cfg;
    cfg.tenant_id = "ns1";
    cfg.table_namespace = "deskA.";
    mgr->create_tenant(cfg);

    EXPECT_TRUE(mgr->can_access_table("ns1", "deskA.trades"));
    EXPECT_FALSE(mgr->can_access_table("ns1", "public_trades"));
}
