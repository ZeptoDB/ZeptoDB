#pragma once
// ============================================================================
// ZeptoDB: Vault-backed API Key Backend
// ============================================================================
// Stores and retrieves API key entries from HashiCorp Vault KV v2.
//
// Vault path layout:
//   {mount}/data/zeptodb/keys/{key_id}  →  JSON with key entry fields
//   {mount}/data/zeptodb/keys/_index    →  JSON list of all key IDs
//
// Usage:
//   VaultKeyBackend::Config cfg;
//   cfg.addr  = "https://vault.internal:8200";
//   cfg.token = getenv("VAULT_TOKEN");
//   auto backend = std::make_unique<VaultKeyBackend>(cfg);
//
//   // Inject into ApiKeyStore
//   ApiKeyStore store("/etc/zeptodb/keys.conf", std::move(backend));
//
// The local file remains the primary store. Vault acts as a sync target:
//   - On create/revoke/update: write-through to Vault
//   - On load: merge Vault entries not present locally (Vault → file sync)
//
// Thread safety: all methods are safe to call from multiple threads.
// ============================================================================

#include "zeptodb/auth/api_key_store.h"
#include <string>
#include <vector>
#include <optional>

namespace zeptodb::auth {

class VaultKeyBackend {
public:
    struct Config {
        std::string addr;                // Vault server address
        std::string token;               // Vault token (or VAULT_TOKEN env)
        std::string mount   = "secret";  // KV v2 mount path
        std::string prefix  = "zeptodb/keys"; // key path prefix
        int         timeout_sec = 3;
    };

    explicit VaultKeyBackend(Config cfg);

    /// Check if Vault is reachable and configured.
    bool available() const;

    /// Store a key entry in Vault. Returns true on success.
    bool store(const ApiKeyEntry& entry);

    /// Load a single key entry from Vault by id. Returns nullopt if not found.
    std::optional<ApiKeyEntry> load(const std::string& key_id);

    /// Load all key entries from Vault.
    std::vector<ApiKeyEntry> load_all();

    /// Delete a key entry from Vault by id.
    bool remove(const std::string& key_id);

private:
    Config cfg_;

    // Vault KV v2 HTTP helpers
    std::string vault_get(const std::string& path);
    bool        vault_put(const std::string& path, const std::string& json_body);
    bool        vault_delete(const std::string& path);

    // Serialization
    static std::string entry_to_json(const ApiKeyEntry& entry);
    static std::optional<ApiKeyEntry> json_to_entry(const std::string& json);
    static std::string entries_to_index_json(const std::vector<std::string>& ids);
    static std::vector<std::string> index_json_to_ids(const std::string& json);

    std::string key_path(const std::string& key_id) const;
    std::string index_path() const;

    // Update the _index list in Vault
    void update_index_add(const std::string& key_id);
    void update_index_remove(const std::string& key_id);
};

} // namespace zeptodb::auth
