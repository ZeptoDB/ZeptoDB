// ============================================================================
// ZeptoDB: JWT Validator Implementation (HS256 + RS256)
// ============================================================================
#include "zeptodb/auth/jwt_validator.h"
#include "json_claims.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/sha.h>

#include <chrono>
#include <algorithm>
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
    if (p1 == std::string::npos || p1 == 0) return std::nullopt;
    auto p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos || p2 == p1 + 1 || p2 + 1 >= token.size() ||
        token.find('.', p2 + 1) != std::string::npos) {
        return std::nullopt;
    }

    std::string b64_header  = token.substr(0, p1);
    std::string b64_payload = token.substr(p1 + 1, p2 - p1 - 1);
    std::string b64_sig     = token.substr(p2 + 1);
    std::string header_payload = b64_header + "." + b64_payload;

    // Decode and parse header to determine algorithm
    std::string header_json = base64url_decode(b64_header);
    if (header_json.empty()) return std::nullopt;
    std::string alg;
    if (detail::read_json_string(header_json, "alg", &alg) !=
        detail::JsonFieldStatus::Valid) {
        return std::nullopt;
    }

    // Verify signature
    bool sig_ok = false;
    if (alg == "HS256") {
        if (config_.hs256_secret.empty()) return std::nullopt;
        sig_ok = verify_hs256(header_payload, b64_sig);
    } else if (alg == "RS256") {
        // Try static PEM first, then dynamic key resolver (JWKS)
        if (!config_.rs256_public_key_pem.empty()) {
            sig_ok = verify_rs256(
                header_payload, b64_sig, config_.rs256_public_key_pem);
        } else if (key_resolver_) {
            std::string kid;
            const auto kid_status =
                detail::read_json_string(header_json, "kid", &kid);
            if (kid_status == detail::JsonFieldStatus::Invalid) {
                return std::nullopt;
            }
            std::string pem = key_resolver_(kid);
            if (!pem.empty()) {
                sig_ok = verify_rs256(header_payload, b64_sig, pem);
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
    if (payload_json.empty()) return std::nullopt;

    JwtClaims claims;
    if (detail::read_json_string(payload_json, "sub", &claims.subject) !=
            detail::JsonFieldStatus::Valid ||
        claims.subject.empty()) {
        return std::nullopt;
    }
    const auto email_status =
        detail::read_json_string(payload_json, "email", &claims.email);
    const auto issuer_status =
        detail::read_json_string(payload_json, "iss", &claims.issuer);
    const auto expiry_status =
        detail::read_json_int64(payload_json, "exp", &claims.expiry);
    if (email_status == detail::JsonFieldStatus::Invalid ||
        issuer_status == detail::JsonFieldStatus::Invalid ||
        expiry_status == detail::JsonFieldStatus::Invalid) {
        return std::nullopt;
    }

    // Validate expiry
    if (config_.verify_expiry) {
        if (expiry_status != detail::JsonFieldStatus::Valid ||
            claims.expiry <= 0) {
            return std::nullopt;
        }
        using namespace std::chrono;
        int64_t now = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        if (now >= claims.expiry) return std::nullopt;
    }

    // Validate issuer
    if (!config_.expected_issuer.empty() &&
        claims.issuer != config_.expected_issuer)
        return std::nullopt;

    // Validate audience
    if (!config_.expected_audience.empty()) {
        // "aud" can be string or array in JWT spec
        std::string aud_str;
        const auto string_status =
            detail::read_json_string(payload_json, "aud", &aud_str);
        std::vector<std::string> audiences;
        const auto array_status = detail::read_json_string_array(
            payload_json, "aud", &audiences);
        const bool string_match =
            string_status == detail::JsonFieldStatus::Valid &&
            aud_str == config_.expected_audience;
        const bool array_match =
            array_status == detail::JsonFieldStatus::Valid &&
            std::find(audiences.begin(), audiences.end(),
                      config_.expected_audience) != audiences.end();
        if (!string_match && !array_match) return std::nullopt;
    }

    // Extract role from configured claim
    std::string role_str;
    if (!config_.role_claim.empty() &&
        detail::read_json_string(payload_json, config_.role_claim,
                                 &role_str) ==
            detail::JsonFieldStatus::Invalid) {
        return std::nullopt;
    }
    claims.role = role_from_string(role_str);
    if (claims.role == Role::UNKNOWN) claims.role = config_.default_role;

    // Extract symbol whitelist from configured claim (comma-separated string)
    std::string syms_str;
    if (!config_.symbols_claim.empty() &&
        detail::read_json_string(payload_json, config_.symbols_claim,
                                 &syms_str) ==
            detail::JsonFieldStatus::Invalid) {
        return std::nullopt;
    }
    if (!syms_str.empty()) {
        std::istringstream ss(syms_str);
        std::string sym;
        while (std::getline(ss, sym, ','))
            if (!sym.empty()) claims.allowed_symbols.push_back(sym);
        // A present, non-empty restriction that decodes to no entries (for
        // example ",,,") must not collapse to the empty=unrestricted state.
        if (claims.allowed_symbols.empty()) return std::nullopt;
    }

    if (!config_.tenant_claim.empty() &&
        detail::read_json_string(payload_json, config_.tenant_claim,
                                 &claims.tenant_id) ==
            detail::JsonFieldStatus::Invalid) {
        return std::nullopt;
    }
    if (!config_.tables_claim.empty() &&
        detail::read_json_string_array(payload_json, config_.tables_claim,
                                       &claims.allowed_tables) ==
            detail::JsonFieldStatus::Invalid) {
        return std::nullopt;
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
                                 const std::string& b64sig,
                                 const std::string& public_key_pem) const
{
    // Load public key from PEM
    BIO* bio = BIO_new_mem_buf(public_key_pem.data(),
                               static_cast<int>(public_key_pem.size()));
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
    // JWT uses canonical, unpadded base64url. Rejecting invalid characters,
    // padding, impossible lengths, and non-zero unused bits prevents multiple
    // textual tokens from decoding to the same signed bytes.
    if (input.empty() || input.size() % 4 == 1) return {};

    auto sextet = [](const unsigned char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '-') return 62;
        if (value == '_') return 63;
        return -1;
    };

    std::string result;
    result.reserve((input.size() * 3) / 4);
    size_t pos = 0;
    while (input.size() - pos >= 4) {
        const int a = sextet(static_cast<unsigned char>(input[pos]));
        const int b = sextet(static_cast<unsigned char>(input[pos + 1]));
        const int c = sextet(static_cast<unsigned char>(input[pos + 2]));
        const int d = sextet(static_cast<unsigned char>(input[pos + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0) return {};
        result.push_back(static_cast<char>((a << 2) | (b >> 4)));
        result.push_back(static_cast<char>((b << 4) | (c >> 2)));
        result.push_back(static_cast<char>((c << 6) | d));
        pos += 4;
    }

    const size_t remaining = input.size() - pos;
    if (remaining == 2) {
        const int a = sextet(static_cast<unsigned char>(input[pos]));
        const int b = sextet(static_cast<unsigned char>(input[pos + 1]));
        if (a < 0 || b < 0 || (b & 0x0f) != 0) return {};
        result.push_back(static_cast<char>((a << 2) | (b >> 4)));
    } else if (remaining == 3) {
        const int a = sextet(static_cast<unsigned char>(input[pos]));
        const int b = sextet(static_cast<unsigned char>(input[pos + 1]));
        const int c = sextet(static_cast<unsigned char>(input[pos + 2]));
        if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0) return {};
        result.push_back(static_cast<char>((a << 2) | (b >> 4)));
        result.push_back(static_cast<char>((b << 4) | (c >> 2)));
    }
    return result;
}

// ============================================================================
// Minimal JSON extractors (flat objects only)
// ============================================================================
std::string JwtValidator::get_json_string(const std::string& json,
                                           const std::string& key)
{
    std::string value;
    if (detail::read_json_string(json, key, &value) !=
        detail::JsonFieldStatus::Valid) {
        return "";
    }
    return value;
}

int64_t JwtValidator::get_json_int64(const std::string& json,
                                      const std::string& key)
{
    int64_t value = 0;
    if (detail::read_json_int64(json, key, &value) !=
        detail::JsonFieldStatus::Valid) {
        return 0;
    }
    return value;
}

std::vector<std::string> JwtValidator::get_json_string_array(
    const std::string& json, const std::string& key)
{
    std::vector<std::string> result;
    if (detail::read_json_string_array(json, key, &result) !=
        detail::JsonFieldStatus::Valid) {
        result.clear();
    }
    return result;
}

} // namespace zeptodb::auth
