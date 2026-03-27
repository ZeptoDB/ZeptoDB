// ============================================================================
// ZeptoDB: JWKS Provider Implementation
// ============================================================================
#include "zeptodb/auth/jwks_provider.h"
#include "zeptodb/auth/jwt_validator.h"  // for base64url_decode

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "third_party/httplib.h"

#include <sstream>
#include <chrono>

namespace zeptodb::auth {

// ============================================================================
// Constructor / Destructor
// ============================================================================
JwksProvider::JwksProvider(std::string jwks_url, int refresh_interval_s)
    : jwks_url_(std::move(jwks_url))
    , refresh_interval_s_(refresh_interval_s)
{}

JwksProvider::~JwksProvider() { stop(); }

// ============================================================================
// start / stop
// ============================================================================
void JwksProvider::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this]() {
        while (running_.load()) {
            fetch_and_parse();
            for (int i = 0; i < refresh_interval_s_ && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

void JwksProvider::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

// ============================================================================
// refresh — public synchronous fetch
// ============================================================================
bool JwksProvider::refresh() { return fetch_and_parse(); }

// ============================================================================
// get_pem / get_default_pem / key_count
// ============================================================================
std::string JwksProvider::get_pem(const std::string& kid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = keys_.find(kid);
    return it != keys_.end() ? it->second : "";
}

std::string JwksProvider::get_default_pem() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!default_kid_.empty()) {
        auto it = keys_.find(default_kid_);
        if (it != keys_.end()) return it->second;
    }
    if (!keys_.empty()) return keys_.begin()->second;
    return "";
}

size_t JwksProvider::key_count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return keys_.size();
}

// ============================================================================
// fetch_and_parse — HTTP GET + parse JWKS JSON
// ============================================================================
bool JwksProvider::fetch_and_parse() {
    // Parse URL: scheme://host[:port]/path
    std::string url = jwks_url_;
    bool use_ssl = false;
    if (url.substr(0, 8) == "https://") { use_ssl = true; url = url.substr(8); }
    else if (url.substr(0, 7) == "http://") { url = url.substr(7); }

    auto slash = url.find('/');
    std::string host_port = (slash != std::string::npos) ? url.substr(0, slash) : url;
    std::string path = (slash != std::string::npos) ? url.substr(slash) : "/";

    std::string body;
    if (use_ssl) {
        httplib::SSLClient cli(host_port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        auto res = cli.Get(path);
        if (!res || res->status != 200) return false;
        body = res->body;
    } else {
        httplib::Client cli(host_port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        auto res = cli.Get(path);
        if (!res || res->status != 200) return false;
        body = res->body;
    }

    // Parse JWKS: {"keys":[{...},{...}]}
    // Find the "keys" array
    auto keys_pos = body.find("\"keys\"");
    if (keys_pos == std::string::npos) return false;
    auto arr_start = body.find('[', keys_pos);
    if (arr_start == std::string::npos) return false;

    // Parse individual key objects within the array
    std::unordered_map<std::string, std::string> new_keys;
    std::string first_kid;

    size_t pos = arr_start + 1;
    while (pos < body.size()) {
        auto obj_start = body.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching closing brace (no nested objects in JWK)
        auto obj_end = body.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string obj = body.substr(obj_start, obj_end - obj_start + 1);
        pos = obj_end + 1;

        std::string kty = get_json_string(obj, "kty");
        std::string use = get_json_string(obj, "use");
        std::string kid = get_json_string(obj, "kid");

        // Only process RSA signing keys
        if (kty != "RSA") continue;
        if (!use.empty() && use != "sig") continue;

        std::string n = get_json_string(obj, "n");
        std::string e = get_json_string(obj, "e");
        if (n.empty() || e.empty()) continue;

        std::string pem = jwk_to_pem(n, e);
        if (pem.empty()) continue;

        if (first_kid.empty()) first_kid = kid;
        new_keys[kid] = std::move(pem);
    }

    if (new_keys.empty()) return false;

    std::lock_guard<std::mutex> lk(mutex_);
    keys_ = std::move(new_keys);
    default_kid_ = first_kid;
    return true;
}

// ============================================================================
// jwk_to_pem — convert RSA JWK (n, e) to PEM public key
// ============================================================================
std::string JwksProvider::jwk_to_pem(const std::string& n_b64url,
                                      const std::string& e_b64url)
{
    std::string n_bytes = JwtValidator::base64url_decode(n_b64url);
    std::string e_bytes = JwtValidator::base64url_decode(e_b64url);
    if (n_bytes.empty() || e_bytes.empty()) return "";

    BIGNUM* bn_n = BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes.data()),
                              static_cast<int>(n_bytes.size()), nullptr);
    BIGNUM* bn_e = BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes.data()),
                              static_cast<int>(e_bytes.size()), nullptr);
    if (!bn_n || !bn_e) { BN_free(bn_n); BN_free(bn_e); return ""; }

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(bld, "n", bn_n);
    OSSL_PARAM_BLD_push_BN(bld, "e", bn_e);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    BN_free(bn_n); BN_free(bn_e);

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    if (!pctx) { OSSL_PARAM_free(params); return ""; }

    EVP_PKEY* pkey = nullptr;
    bool ok = (EVP_PKEY_fromdata_init(pctx) == 1 &&
               EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1);
    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);
    if (!ok || !pkey) { EVP_PKEY_free(pkey); return ""; }

    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) { EVP_PKEY_free(pkey); return ""; }

    PEM_write_bio_PUBKEY(bio, pkey);

    char* pem_data = nullptr;
    long pem_len = BIO_get_mem_data(bio, &pem_data);
    std::string result(pem_data, static_cast<size_t>(pem_len));

    BIO_free(bio);
    EVP_PKEY_free(pkey);
    return result;
}

// ============================================================================
// get_json_string — minimal JSON string extractor
// ============================================================================
std::string JwksProvider::get_json_string(const std::string& json,
                                           const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto kpos = json.find(search);
    if (kpos == std::string::npos) return "";
    auto colon = json.find(':', kpos + search.size());
    if (colon == std::string::npos) return "";
    auto p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != '"') return "";
    auto start = p + 1;
    auto end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

} // namespace zeptodb::auth
