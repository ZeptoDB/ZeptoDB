#pragma once
// ============================================================================
// ZeptoDB: JWKS Provider — Auto-fetch RS256 public keys from IdP
// ============================================================================
// Fetches JSON Web Key Sets from a JWKS endpoint (e.g. Okta, Azure AD, Google)
// and converts RSA JWK entries to PEM public keys for JwtValidator.
//
// Features:
//   - Background refresh (default: every 3600s)
//   - kid-based key lookup (supports key rotation)
//   - Force refresh on validation failure
//   - Thread-safe key map access
//
// Usage:
//   JwksProvider provider("https://dev-123.okta.com/.../v1/keys");
//   provider.start();  // background refresh
//   auto pem = provider.get_pem("key-id-123");
// ============================================================================

#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace zeptodb::auth {

class JwksProvider {
public:
    /// @param jwks_url  Full URL to the JWKS endpoint.
    /// @param refresh_interval_s  Seconds between background refreshes (0 = no background).
    explicit JwksProvider(std::string jwks_url, int refresh_interval_s = 3600);
    ~JwksProvider();

    JwksProvider(const JwksProvider&) = delete;
    JwksProvider& operator=(const JwksProvider&) = delete;

    /// Fetch keys synchronously (called once at startup, then by background thread).
    /// Returns true if at least one key was loaded.
    bool refresh();

    /// Start background refresh thread.
    void start();

    /// Stop background refresh thread.
    void stop();

    /// Get PEM public key by kid. Returns empty string if not found.
    std::string get_pem(const std::string& kid) const;

    /// Get the first available PEM (for tokens without kid header).
    std::string get_default_pem() const;

    /// Number of keys currently loaded.
    size_t key_count() const;

    /// Convert a JWK RSA key (base64url n, e) to PEM public key string.
    /// Public for testing.
    static std::string jwk_to_pem(const std::string& n_b64url,
                                   const std::string& e_b64url);

private:
    std::string jwks_url_;
    int         refresh_interval_s_;

    mutable std::mutex                            mutex_;
    std::unordered_map<std::string, std::string>  keys_;  // kid → PEM
    std::string                                   default_kid_;

    std::atomic<bool>  running_{false};
    std::thread        thread_;

    bool fetch_and_parse();

    // Minimal JSON helpers
    static std::string get_json_string(const std::string& json,
                                       const std::string& key);
};

} // namespace zeptodb::auth
