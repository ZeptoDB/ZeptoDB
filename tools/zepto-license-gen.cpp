// ============================================================================
// zepto-license-gen — Enterprise license key generator
// ============================================================================
// Signs RS256 JWT license keys using a private PEM key.
//
// Usage:
//   ./zepto-license-gen --key keys/license-private.pem \
//       --company "Acme Corp" --max-nodes 16 --days 365
//
// Output: signed JWT string to stdout.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef ZEPTO_AUTH_OPENSSL
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#endif

// ============================================================================
// Base64url
// ============================================================================
static std::string base64url_encode(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[n & 63] : '=';
    }
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

static std::string base64url_encode(const std::string& s) {
    return base64url_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

// ============================================================================
// RS256 sign
// ============================================================================
#ifdef ZEPTO_AUTH_OPENSSL
static std::string rs256_sign(const std::string& message, const std::string& private_key_pem) {
    BIO* bio = BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()));
    if (!bio) return "";
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        std::cerr << "Error: failed to parse private key PEM\n";
        return "";
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t sig_len = 0;
    std::string sig;

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestSignUpdate(ctx, message.data(), message.size()) == 1 &&
        EVP_DigestSignFinal(ctx, nullptr, &sig_len) == 1) {
        sig.resize(sig_len);
        EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(sig.data()), &sig_len);
        sig.resize(sig_len);
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return sig;
}
#endif

// ============================================================================
// Read file
// ============================================================================
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ============================================================================
// Main
// ============================================================================
static void usage() {
    std::cerr << R"(zepto-license-gen — ZeptoDB Enterprise license key generator

Usage:
  zepto-license-gen --key <private.pem> [options]

Required:
  --key <path>        RSA private key PEM file

Options:
  --company <name>    Licensee company name (default: "Enterprise")
  --max-nodes <n>     Maximum cluster nodes (default: 16)
  --features <hex>    Feature bitmask (default: 0xFF = all features)
  --days <n>          Validity in days (default: 365)
  --tenant <id>       Tenant ID for SaaS isolation (default: "")
  --grace <n>         Grace period days after expiry (default: 30)

Feature bits:
  0x01 CLUSTER          0x02 SSO
  0x04 AUDIT_EXPORT     0x08 ADVANCED_RBAC
  0x10 KAFKA            0x20 MIGRATION
  0x40 GEO_REPLICATION  0x80 ROLLING_UPGRADE
  0xFF ALL FEATURES
)";
}

int main(int argc, char* argv[]) {
#ifndef ZEPTO_AUTH_OPENSSL
    std::cerr << "Error: built without OpenSSL — cannot sign license keys\n";
    return 1;
#else
    std::string key_path, company = "Enterprise", tenant_id;
    int max_nodes = 16, days = 365, grace_days = 30;
    uint32_t features = 0xFF;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (arg == "--key") key_path = next();
        else if (arg == "--company") company = next();
        else if (arg == "--max-nodes") max_nodes = std::stoi(next());
        else if (arg == "--features") features = static_cast<uint32_t>(std::stoul(next(), nullptr, 0));
        else if (arg == "--days") days = std::stoi(next());
        else if (arg == "--tenant") tenant_id = next();
        else if (arg == "--grace") grace_days = std::stoi(next());
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else { std::cerr << "Unknown option: " << arg << "\n"; usage(); return 1; }
    }

    if (key_path.empty()) {
        std::cerr << "Error: --key is required\n\n";
        usage();
        return 1;
    }

    std::string private_key = read_file(key_path);
    if (private_key.empty()) {
        std::cerr << "Error: cannot read private key: " << key_path << "\n";
        return 1;
    }

    // Build JWT
    int64_t now = std::time(nullptr);
    int64_t exp = now + static_cast<int64_t>(days) * 86400;

    std::string header_json = R"({"alg":"RS256","typ":"JWT"})";
    std::ostringstream pj;
    pj << R"({"edition":"enterprise")"
       << R"(,"features":)" << features
       << R"(,"max_nodes":)" << max_nodes
       << R"(,"company":")" << company << "\""
       << R"(,"tenant_id":")" << tenant_id << "\""
       << R"(,"exp":)" << exp
       << R"(,"iat":)" << now
       << R"(,"grace_days":)" << grace_days
       << "}";

    std::string b64_header = base64url_encode(header_json);
    std::string b64_payload = base64url_encode(pj.str());
    std::string header_payload = b64_header + "." + b64_payload;

    std::string sig = rs256_sign(header_payload, private_key);
    if (sig.empty()) {
        std::cerr << "Error: RS256 signing failed\n";
        return 1;
    }

    std::string jwt = header_payload + "." +
        base64url_encode(reinterpret_cast<const unsigned char*>(sig.data()), sig.size());

    // Output
    std::cout << jwt << "\n";

    // Info to stderr
    std::cerr << "License generated:\n"
              << "  Company:    " << company << "\n"
              << "  Max nodes:  " << max_nodes << "\n"
              << "  Features:   0x" << std::hex << features << std::dec << "\n"
              << "  Expires:    " << days << " days from now\n"
              << "  Grace:      " << grace_days << " days\n";

    return 0;
#endif
}
