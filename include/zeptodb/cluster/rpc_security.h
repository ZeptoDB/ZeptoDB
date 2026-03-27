#pragma once
// ============================================================================
// RPC Security — internal cluster authentication
// ============================================================================
// Shared-secret HMAC authentication for inter-node RPC.
//
// Protocol:
//   1. Client sends AUTH_HANDSHAKE as first message after connect
//      Payload: 8-byte nonce + 32-byte HMAC(secret, nonce)
//   2. Server validates HMAC, replies AUTH_OK or closes connection
//   3. All subsequent messages proceed as normal
//
// The shared secret is distributed via:
//   - Environment variable (ZEPTO_CLUSTER_SECRET)
//   - Config file
//   - Vault (via SecretsProvider)
//
// mTLS: RpcTlsConfig provides cert/key/ca paths for future TLS wrapping.
// ============================================================================

#include "zeptodb/cluster/rpc_protocol.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

namespace zeptodb::cluster {

// ============================================================================
// RpcSecurityConfig
// ============================================================================
struct RpcSecurityConfig {
    bool        enabled = false;
    std::string shared_secret;     // HMAC key for auth handshake

    // mTLS (future — paths only, not wired to socket yet)
    std::string cert_path;         // node certificate PEM
    std::string key_path;          // node private key PEM
    std::string ca_cert_path;      // CA cert for peer verification
};

// ============================================================================
// Auth token: 8-byte nonce + 32-byte HMAC
// ============================================================================
static constexpr size_t RPC_NONCE_SIZE = 8;
static constexpr size_t RPC_HMAC_SIZE  = 32;
static constexpr size_t RPC_AUTH_PAYLOAD_SIZE = RPC_NONCE_SIZE + RPC_HMAC_SIZE;

// Simple HMAC-SHA256-like using FNV + mixing (no OpenSSL dependency).
// For production mTLS, this is a defense-in-depth layer, not the sole auth.
// Uses double-round FNV-1a mixing to produce a 256-bit digest.
inline std::array<uint8_t, RPC_HMAC_SIZE> rpc_compute_hmac(
    const std::string& secret,
    const uint8_t* nonce, size_t nonce_len)
{
    // Concatenate secret + nonce into a buffer
    std::array<uint8_t, RPC_HMAC_SIZE> result{};

    // 4 independent FNV-1a-64 hashes with different seeds → 256 bits
    const uint64_t seeds[4] = {
        0xcbf29ce484222325ULL,
        0x100000001b3ULL,
        0x6c62272e07bb0142ULL,
        0x2b992ddfa23249d6ULL
    };

    for (int round = 0; round < 4; ++round) {
        uint64_t h = seeds[round];
        // Mix secret
        for (char c : secret) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        // Mix nonce
        for (size_t i = 0; i < nonce_len; ++i) {
            h ^= nonce[i];
            h *= 0x100000001b3ULL;
        }
        // Mix round number
        h ^= static_cast<uint64_t>(round);
        h *= 0x100000001b3ULL;

        // Second pass (strengthen)
        for (char c : secret) {
            h ^= static_cast<uint8_t>(c);
            h *= 0x100000001b3ULL;
        }

        // Store 8 bytes
        std::memcpy(result.data() + round * 8, &h, 8);
    }

    return result;
}

inline void rpc_generate_nonce(uint8_t* out) {
    std::random_device rd;
    uint64_t val = (static_cast<uint64_t>(rd()) << 32) | rd();
    std::memcpy(out, &val, RPC_NONCE_SIZE);
}

/// Build auth handshake payload: nonce(8) + hmac(32) = 40 bytes
inline std::array<uint8_t, RPC_AUTH_PAYLOAD_SIZE> rpc_build_auth(
    const std::string& secret)
{
    std::array<uint8_t, RPC_AUTH_PAYLOAD_SIZE> payload{};
    rpc_generate_nonce(payload.data());
    auto hmac = rpc_compute_hmac(secret, payload.data(), RPC_NONCE_SIZE);
    std::memcpy(payload.data() + RPC_NONCE_SIZE, hmac.data(), RPC_HMAC_SIZE);
    return payload;
}

/// Validate auth handshake payload against secret
inline bool rpc_validate_auth(const std::string& secret,
                               const uint8_t* payload, size_t len) {
    if (len < RPC_AUTH_PAYLOAD_SIZE) return false;
    auto expected = rpc_compute_hmac(secret, payload, RPC_NONCE_SIZE);
    return std::memcmp(payload + RPC_NONCE_SIZE, expected.data(),
                       RPC_HMAC_SIZE) == 0;
}

} // namespace zeptodb::cluster
