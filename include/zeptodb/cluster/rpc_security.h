#pragma once
// ============================================================================
// RPC Security — internal cluster authentication
// ============================================================================
// Shared-secret mutual authentication for inter-node TCP RPC.
//
// Protocol v2 (all payload lengths are exact):
//   1. Client -> server AUTH_CLIENT_HELLO: client nonce (32 bytes)
//   2. Server -> client AUTH_SERVER_CHALLENGE:
//      server nonce (32) + HMAC-SHA256 server proof (32)
//   3. Client -> server AUTH_CLIENT_PROOF: HMAC-SHA256 client proof (32)
//   4. Server -> client AUTH_OK (empty payload), or AUTH_REJECT.
//
// Proofs are domain-separated and bind both nonces. The fresh server nonce
// prevents a captured client proof from being replayed; the server proof lets
// the client reject a peer that does not possess the shared secret.
//
// Configuration is loaded by default from exactly one of:
//   - ZEPTO_CLUSTER_SECRET
//   - ZEPTO_CLUSTER_SECRET_FILE
// A configured secret must contain at least 32 bytes. Authentication is
// fail-closed when configured incorrectly or when OpenSSL is unavailable.
//
// This authenticates peers but does not encrypt traffic. mTLS fields are
// retained for a future encrypted transport and are not wired to sockets yet.
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace zeptodb::cluster {

inline constexpr size_t RPC_AUTH_MIN_SECRET_SIZE = 32;
inline constexpr size_t RPC_AUTH_NONCE_SIZE = 32;
inline constexpr size_t RPC_AUTH_PROOF_SIZE = 32;
inline constexpr size_t RPC_AUTH_CLIENT_HELLO_SIZE = RPC_AUTH_NONCE_SIZE;
inline constexpr size_t RPC_AUTH_SERVER_CHALLENGE_SIZE =
    RPC_AUTH_NONCE_SIZE + RPC_AUTH_PROOF_SIZE;
inline constexpr size_t RPC_AUTH_CLIENT_PROOF_SIZE = RPC_AUTH_PROOF_SIZE;

using RpcAuthNonce = std::array<uint8_t, RPC_AUTH_NONCE_SIZE>;
using RpcAuthProof = std::array<uint8_t, RPC_AUTH_PROOF_SIZE>;
using RpcAuthServerChallenge =
    std::array<uint8_t, RPC_AUTH_SERVER_CHALLENGE_SIZE>;

struct RpcSecurityConfig {
    bool        enabled = false;
    std::string shared_secret;

    // mTLS (future — paths only, not wired to the TCP socket yet).
    std::string cert_path;
    std::string key_path;
    std::string ca_cert_path;

    // Non-secret configuration failure captured while loading an environment
    // or secret-file source. Never contains secret material or proof bytes.
    std::string configuration_error;

    /// Read the process-wide cluster secret configuration. Absence leaves
    /// authentication disabled for explicit local/development operation.
    static RpcSecurityConfig from_environment();

    /// Empty means the configuration is safe to use. When enabled, a failure
    /// must prevent a server from listening and a client from connecting.
    [[nodiscard]] std::string validation_error() const;
    [[nodiscard]] bool valid() const { return validation_error().empty(); }
};

/// Fill a nonce using OpenSSL's cryptographic RNG. Returns false rather than
/// falling back to a non-cryptographic source.
bool rpc_generate_auth_nonce(RpcAuthNonce& nonce);

/// Compute domain-separated mutual-authentication proofs.
bool rpc_compute_server_proof(const std::string& secret,
                              const RpcAuthNonce& client_nonce,
                              const RpcAuthNonce& server_nonce,
                              RpcAuthProof& proof);
bool rpc_compute_client_proof(const std::string& secret,
                              const RpcAuthNonce& client_nonce,
                              const RpcAuthNonce& server_nonce,
                              RpcAuthProof& proof);

/// Constant-time proof validation via OpenSSL CRYPTO_memcmp.
bool rpc_validate_server_proof(const std::string& secret,
                               const RpcAuthNonce& client_nonce,
                               const RpcAuthNonce& server_nonce,
                               const RpcAuthProof& proof);
bool rpc_validate_client_proof(const std::string& secret,
                               const RpcAuthNonce& client_nonce,
                               const RpcAuthNonce& server_nonce,
                               const RpcAuthProof& proof);

} // namespace zeptodb::cluster
