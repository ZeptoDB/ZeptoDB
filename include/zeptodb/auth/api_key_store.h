#pragma once
// ============================================================================
// ZeptoDB: API Key Store
// ============================================================================
// Manages API keys for bearer-token authentication.
//
// Key format:  zepto_<64-char lowercase hex>  (32 bytes random = 256-bit entropy)
// Stored:      SHA256(full_key) hex — raw key never persisted
// Config file: line-based, pipe-separated (see load/save)
//
// Thread safety: all public methods are protected by internal mutex.
// ============================================================================

#include "zeptodb/auth/rbac.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mutex>
#include <cstdint>

namespace zeptodb::auth {

// Forward declaration — Vault backend for key sync
class VaultKeyBackend;

// ============================================================================
// ApiKeyEntry — one registered key
// ============================================================================
struct ApiKeyEntry {
    std::string              id;              // short id, e.g. "ak_7f3k"
    std::string              name;            // human label, e.g. "trading-desk-1"
    std::string              key_hash;        // sha256 hex of the full key
    Role                     role = Role::READER;
    std::vector<std::string> allowed_symbols; // empty = unrestricted
    std::vector<std::string> allowed_tables;  // empty = unrestricted
    std::string              tenant_id;       // empty = no tenant
    bool                     enabled = true;
    int64_t                  created_at_ns = 0;
    mutable int64_t          last_used_ns = 0;
    int64_t                  expires_at_ns = 0; // 0 = never expires

    bool is_expired() const;
};

// ============================================================================
// ApiKeyStore
// ============================================================================
class ApiKeyStore {
public:
    // config_path: path to the key file. Created if it does not exist.
    explicit ApiKeyStore(std::string config_path);

    // config_path + Vault backend for write-through sync.
    ApiKeyStore(std::string config_path,
                std::unique_ptr<VaultKeyBackend> vault_backend);

    // Create a new API key. Returns the full plaintext key (shown exactly once).
    // Persists to config_path immediately.
    std::string create_key(const std::string& name,
                           Role role,
                           const std::vector<std::string>& allowed_symbols = {},
                           const std::vector<std::string>& allowed_tables = {},
                           const std::string& tenant_id = "",
                           int64_t expires_at_ns = 0);

    // Validate a full plaintext key. Returns the entry on success.
    // Returns std::nullopt if key is invalid, disabled, or expired.
    std::optional<ApiKeyEntry> validate(const std::string& key) const;

    // Disable (soft-delete) a key by its short id.
    bool revoke(const std::string& key_id);

    // Update mutable fields of an existing key. Returns false if key not found.
    bool update_key(const std::string& key_id,
                    const std::optional<std::vector<std::string>>& symbols,
                    const std::optional<std::vector<std::string>>& tables,
                    const std::optional<bool>& enabled,
                    const std::optional<std::string>& tenant_id,
                    const std::optional<int64_t>& expires_at_ns);

    // List all keys (for admin API).
    std::vector<ApiKeyEntry> list() const;

    // Reload from disk (useful after external edits).
    void reload();

    // SHA-256 hex of input (public for testing).
    static std::string sha256_hex(const std::string& input);

private:
    std::string              config_path_;
    std::vector<ApiKeyEntry> entries_;
    mutable std::mutex       mutex_;
    std::unique_ptr<VaultKeyBackend> vault_backend_;

    void load();
    void save() const;  // caller must hold mutex_
    void sync_from_vault();  // merge Vault entries not present locally
    void sync_to_vault(const ApiKeyEntry& entry);  // write-through

    static std::string generate_key();   // returns full "zepto_<hex>" key
    static std::string generate_key_id();
    static int64_t     now_ns();
};

} // namespace zeptodb::auth
