// ============================================================================
// ZeptoDB: JWT Validator Implementation (HS256 + RS256)
// ============================================================================
#include "zeptodb/auth/jwt_validator.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/sha.h>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace zeptodb::auth {

// ============================================================================
// Constructor
// ============================================================================
JwtValidator::JwtValidator(Config config)
    : config_(std::move(config))
{}

// ============================================================================
// validate — main entry point
// ============================================================================
std::optional<JwtClaims> JwtValidator::validate(const std::string& token) const {
    // JWT is three base64url-encoded sections separated by '.'
    auto p1 = token.find('.');
    if (p1 == std::string::npos) return std::nullopt;
    auto p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos) return std::nullopt;

    std::string b64_header  = token.substr(0, p1);
    std::string b64_payload = token.substr(p1 + 1, p2 - p1 - 1);
    std::string b64_sig     = token.substr(p2 + 1);
    std::string header_payload = b64_header + "." + b64_payload;

    // Decode and parse header to determine algorithm
    std::string header_json = base64url_decode(b64_header);
    std::string alg = get_json_string(header_json, "alg");

    // Verify signature
    bool sig_ok = false;
    if (alg == "HS256") {
        if (config_.hs256_secret.empty()) return std::nullopt;
        sig_ok = verify_hs256(header_payload, b64_sig);
    } else if (alg == "RS256") {
        // Try static PEM first, then dynamic key resolver (JWKS)
        if (!config_.rs256_public_key_pem.empty()) {
            sig_ok = verify_rs256(header_payload, b64_sig);
        } else if (key_resolver_) {
            std::string kid = get_json_string(header_json, "kid");
            std::string pem = key_resolver_(kid);
            if (!pem.empty()) {
                // Temporarily set PEM for verification
                std::string saved = config_.rs256_public_key_pem;
                const_cast<Config&>(config_).rs256_public_key_pem = pem;
                sig_ok = verify_rs256(header_payload, b64_sig);
                const_cast<Config&>(config_).rs256_public_key_pem = saved;
            }
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;  // unsupported algorithm
    }

    if (!sig_ok) return std::nullopt;

    // Decode and parse payload
    std::string payload_json = base64url_decode(b64_payload);

    JwtClaims claims;
    claims.subject = get_json_string(payload_json, "sub");
    claims.email   = get_json_string(payload_json, "email");
    claims.issuer  = get_json_string(payload_json, "iss");
    claims.expiry  = get_json_int64(payload_json, "exp");

    // Validate expiry
    if (config_.verify_expiry && claims.expiry > 0) {
        using namespace std::chrono;
        int64_t now = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        if (now > claims.expiry) return std::nullopt;
    }

    // Validate issuer
    if (!config_.expected_issuer.empty() &&
        claims.issuer != config_.expected_issuer)
        return std::nullopt;

    // Validate audience
    if (!config_.expected_audience.empty()) {
        // "aud" can be string or array in JWT spec
        std::string aud_str = get_json_string(payload_json, "aud");
        if (aud_str != config_.expected_audience) {
            // Try as array
            auto aud_arr = get_json_string_array(payload_json, "aud");
            bool found = false;
            for (const auto& a : aud_arr)
                if (a == config_.expected_audience) { found = true; break; }
            if (!found) return std::nullopt;
        }
    }

    // Extract role from configured claim
    std::string role_str = get_json_string(payload_json, config_.role_claim);
    claims.role = role_from_string(role_str);
    if (claims.role == Role::UNKNOWN) claims.role = Role::READER;

    // Extract symbol whitelist from configured claim (comma-separated string)
    std::string syms_str = get_json_string(payload_json, config_.symbols_claim);
    if (!syms_str.empty()) {
        std::istringstream ss(syms_str);
        std::string sym;
        while (std::getline(ss, sym, ','))
            if (!sym.empty()) claims.allowed_symbols.push_back(sym);
    }

    return claims;
}

// ============================================================================
// verify_hs256
// ============================================================================
bool JwtValidator::verify_hs256(const std::string& header_payload,
                                 const std::string& b64sig) const
{
    unsigned int hmac_len = 0;
    unsigned char hmac_buf[EVP_MAX_MD_SIZE];

    HMAC(EVP_sha256(),
         config_.hs256_secret.data(),
         static_cast<int>(config_.hs256_secret.size()),
         reinterpret_cast<const unsigned char*>(header_payload.data()),
         header_payload.size(),
         hmac_buf,
         &hmac_len);

    // base64url-encode the computed MAC and compare
    // We decode the received signature instead (avoids needing a b64url encoder here)
    std::string decoded_sig = base64url_decode(b64sig);

    if (decoded_sig.size() != hmac_len) return false;

    // Constant-time compare
    return CRYPTO_memcmp(decoded_sig.data(), hmac_buf, hmac_len) == 0;
}

// ============================================================================
// verify_rs256
// ============================================================================
bool JwtValidator::verify_rs256(const std::string& header_payload,
                                 const std::string& b64sig) const
{
    // Load public key from PEM
    BIO* bio = BIO_new_mem_buf(config_.rs256_public_key_pem.data(),
                                static_cast<int>(config_.rs256_public_key_pem.size()));
    if (!bio) return false;

    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return false;

    // Decode signature
    std::string sig = base64url_decode(b64sig);

    // Verify
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx) {
        if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1) {
            if (EVP_DigestVerifyUpdate(ctx,
                    reinterpret_cast<const unsigned char*>(header_payload.data()),
                    header_payload.size()) == 1) {
                ok = (EVP_DigestVerifyFinal(ctx,
                        reinterpret_cast<const unsigned char*>(sig.data()),
                        sig.size()) == 1);
            }
        }
        EVP_MD_CTX_free(ctx);
    }

    EVP_PKEY_free(pkey);
    return ok;
}

// ============================================================================
// base64url_decode
// ============================================================================
std::string JwtValidator::base64url_decode(const std::string& input) {
    // Convert base64url → base64
    std::string b64;
    b64.reserve(input.size() + 4);
    for (char c : input) {
        if (c == '-')      b64 += '+';
        else if (c == '_') b64 += '/';
        else               b64 += c;
    }
    // Add padding
    while (b64.size() % 4 != 0) b64 += '=';

    // Standard base64 decode
    static const int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::string result;
    result.reserve(b64.size() * 3 / 4);

    int val = 0, bits = -8;
    for (unsigned char c : b64) {
        if (c == '=') break;
        int v = table[c];
        if (v < 0) continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            result += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return result;
}

// ============================================================================
// Minimal JSON extractors (flat objects only)
// ============================================================================
std::string JwtValidator::get_json_string(const std::string& json,
                                           const std::string& key)
{
    // Find "key" : "value"
    std::string search = "\"" + key + "\"";
    auto kpos = json.find(search);
    if (kpos == std::string::npos) return "";

    auto colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return "";

    // Skip whitespace
    auto p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;

    if (p >= json.size() || json[p] != '"') return "";

    // Find closing quote (handle \" escape)
    auto start = p + 1;
    auto end = start;
    while (end < json.size()) {
        if (json[end] == '\\') { end += 2; continue; }
        if (json[end] == '"') break;
        ++end;
    }
    return json.substr(start, end - start);
}

int64_t JwtValidator::get_json_int64(const std::string& json,
                                      const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto kpos = json.find(search);
    if (kpos == std::string::npos) return 0;

    auto colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return 0;

    auto p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;

    auto end = p;
    if (end < json.size() && json[end] == '-') ++end;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;

    if (end == p) return 0;
    try { return std::stoll(json.substr(p, end - p)); } catch (...) { return 0; }
}

std::vector<std::string> JwtValidator::get_json_string_array(
    const std::string& json, const std::string& key)
{
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto kpos = json.find(search);
    if (kpos == std::string::npos) return result;

    auto colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return result;

    auto bracket = json.find('[', colon + 1);
    if (bracket == std::string::npos) return result;

    auto end_bracket = json.find(']', bracket + 1);
    if (end_bracket == std::string::npos) return result;

    // Parse strings within the array
    auto p = bracket + 1;
    while (p < end_bracket) {
        auto q = json.find('"', p);
        if (q == std::string::npos || q >= end_bracket) break;
        auto e = json.find('"', q + 1);
        if (e == std::string::npos || e >= end_bracket) break;
        result.push_back(json.substr(q + 1, e - q - 1));
        p = e + 1;
    }
    return result;
}

} // namespace zeptodb::auth
