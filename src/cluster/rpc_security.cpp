#include "zeptodb/cluster/rpc_security.h"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#ifdef ZEPTO_RPC_OPENSSL
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

namespace zeptodb::cluster {
namespace {

constexpr std::string_view kServerProofDomain =
    "zeptodb-rpc-auth-v2/server-proof";
constexpr std::string_view kClientProofDomain =
    "zeptodb-rpc-auth-v2/client-proof";
constexpr size_t kMaxSecretFileBytes = 4096;

bool compute_proof(const std::string& secret,
                   std::string_view domain,
                   const RpcAuthNonce& client_nonce,
                   const RpcAuthNonce& server_nonce,
                   RpcAuthProof& proof) {
#ifdef ZEPTO_RPC_OPENSSL
    if (secret.size() < RPC_AUTH_MIN_SECRET_SIZE ||
        secret.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    std::vector<uint8_t> message;
    message.reserve(domain.size() + client_nonce.size() + server_nonce.size());
    message.insert(message.end(), domain.begin(), domain.end());
    message.insert(message.end(), client_nonce.begin(), client_nonce.end());
    message.insert(message.end(), server_nonce.begin(), server_nonce.end());

    unsigned int output_size = 0;
    const auto* output = HMAC(
        EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
        message.data(), message.size(), proof.data(), &output_size);
    return output != nullptr && output_size == proof.size();
#else
    (void)secret;
    (void)domain;
    (void)client_nonce;
    (void)server_nonce;
    (void)proof;
    return false;
#endif
}

bool validate_proof(const RpcAuthProof& expected,
                    const RpcAuthProof& received) {
#ifdef ZEPTO_RPC_OPENSSL
    return CRYPTO_memcmp(expected.data(), received.data(), expected.size()) == 0;
#else
    (void)expected;
    (void)received;
    return false;
#endif
}

} // namespace

RpcSecurityConfig RpcSecurityConfig::from_environment() {
    RpcSecurityConfig config;
    const char* inline_secret = std::getenv("ZEPTO_CLUSTER_SECRET");
    const char* secret_file = std::getenv("ZEPTO_CLUSTER_SECRET_FILE");

    if (inline_secret != nullptr && secret_file != nullptr) {
        config.enabled = true;
        config.configuration_error =
            "set only one of ZEPTO_CLUSTER_SECRET or "
            "ZEPTO_CLUSTER_SECRET_FILE";
        return config;
    }
    if (inline_secret != nullptr) {
        config.enabled = true;
        config.shared_secret = inline_secret;
        return config;
    }
    if (secret_file == nullptr) return config;

    config.enabled = true;
    if (*secret_file == '\0') {
        config.configuration_error =
            "ZEPTO_CLUSTER_SECRET_FILE is empty";
        return config;
    }

    std::ifstream input(secret_file, std::ios::binary);
    if (!input) {
        config.configuration_error =
            "cannot read ZEPTO_CLUSTER_SECRET_FILE";
        return config;
    }

    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > kMaxSecretFileBytes) {
        config.shared_secret.clear();
        config.configuration_error =
            "ZEPTO_CLUSTER_SECRET_FILE is unreadable or exceeds 4096 bytes";
        return config;
    }
    input.seekg(0, std::ios::beg);
    config.shared_secret.resize(static_cast<size_t>(end));
    if (!config.shared_secret.empty() &&
        !input.read(config.shared_secret.data(), end)) {
        config.shared_secret.clear();
        config.configuration_error =
            "ZEPTO_CLUSTER_SECRET_FILE is unreadable or exceeds 4096 bytes";
        return config;
    }
    while (!config.shared_secret.empty() &&
           (config.shared_secret.back() == '\n' ||
            config.shared_secret.back() == '\r')) {
        config.shared_secret.pop_back();
    }
    return config;
}

std::string RpcSecurityConfig::validation_error() const {
    if (!enabled) return {};
    if (!configuration_error.empty()) return configuration_error;
#ifndef ZEPTO_RPC_OPENSSL
    return "cluster RPC authentication requires OpenSSL";
#else
    if (shared_secret.size() < RPC_AUTH_MIN_SECRET_SIZE) {
        return "cluster RPC shared secret must contain at least 32 bytes";
    }
    if (shared_secret.size() > kMaxSecretFileBytes) {
        return "cluster RPC shared secret exceeds 4096 bytes";
    }
    return {};
#endif
}

bool rpc_generate_auth_nonce(RpcAuthNonce& nonce) {
#ifdef ZEPTO_RPC_OPENSSL
    return RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) == 1;
#else
    (void)nonce;
    return false;
#endif
}

bool rpc_compute_server_proof(const std::string& secret,
                              const RpcAuthNonce& client_nonce,
                              const RpcAuthNonce& server_nonce,
                              RpcAuthProof& proof) {
    return compute_proof(secret, kServerProofDomain,
                         client_nonce, server_nonce, proof);
}

bool rpc_compute_client_proof(const std::string& secret,
                              const RpcAuthNonce& client_nonce,
                              const RpcAuthNonce& server_nonce,
                              RpcAuthProof& proof) {
    return compute_proof(secret, kClientProofDomain,
                         client_nonce, server_nonce, proof);
}

bool rpc_validate_server_proof(const std::string& secret,
                               const RpcAuthNonce& client_nonce,
                               const RpcAuthNonce& server_nonce,
                               const RpcAuthProof& proof) {
    RpcAuthProof expected{};
    return rpc_compute_server_proof(secret, client_nonce, server_nonce,
                                    expected) &&
           validate_proof(expected, proof);
}

bool rpc_validate_client_proof(const std::string& secret,
                               const RpcAuthNonce& client_nonce,
                               const RpcAuthNonce& server_nonce,
                               const RpcAuthProof& proof) {
    RpcAuthProof expected{};
    return rpc_compute_client_proof(secret, client_nonce, server_nonce,
                                    expected) &&
           validate_proof(expected, proof);
}

} // namespace zeptodb::cluster
