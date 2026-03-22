#pragma once
// ============================================================================
// APEX-DB: Secrets Provider
// ============================================================================
// Abstraction for loading secrets from multiple backends.
//
// Priority chain (CompositeSecretsProvider):
//   1. HashiCorp Vault (production / enterprise)
//   2. AWS Secrets Manager (cloud deployments)
//   3. File (/run/secrets/ — Docker secrets / K8s secret mounts)
//   4. Environment variable (development / fallback)
//
// Usage:
//   auto secrets = SecretsProviderFactory::create_composite();
//   std::string jwt_secret = secrets->get("APEX_JWT_SECRET");
//
// SOC2 note: Using this abstraction means no secrets are hardcoded or
// stored in environment variables in production — they come from a
// secrets manager with audit logging, rotation, and access control.
// ============================================================================

#include <string>
#include <vector>
#include <memory>

namespace apex::auth {

// ============================================================================
// SecretsProvider — abstract interface
// ============================================================================
class SecretsProvider {
public:
    virtual ~SecretsProvider() = default;

    /// Get a secret by key. Returns default_val if not found or unavailable.
    virtual std::string get(const std::string& key,
                            const std::string& default_val = "") = 0;

    /// Returns true if this backend is configured and reachable.
    virtual bool available() const = 0;

    /// Human-readable backend name (for diagnostics).
    virtual std::string backend_name() const = 0;
};

// ============================================================================
// EnvSecretsProvider — reads from environment variables
// ============================================================================
class EnvSecretsProvider : public SecretsProvider {
public:
    std::string get(const std::string& key,
                    const std::string& default_val = "") override;
    bool        available() const override { return true; }
    std::string backend_name() const override { return "env"; }
};

// ============================================================================
// FileSecretsProvider — reads from files (Docker secrets / K8s mounts)
// Default dir: /run/secrets  (Docker) or /var/run/secrets/apex (K8s)
// File name = secret key, file content = secret value (trailing newline stripped)
// ============================================================================
class FileSecretsProvider : public SecretsProvider {
public:
    explicit FileSecretsProvider(std::string base_dir = "/run/secrets");

    std::string get(const std::string& key,
                    const std::string& default_val = "") override;
    bool        available() const override;
    std::string backend_name() const override { return "file:" + base_dir_; }

private:
    std::string base_dir_;
};

// ============================================================================
// VaultSecretsProvider — HashiCorp Vault KV v2
//
// Config required:
//   vault_addr  = "https://vault.corp.example.com"
//   vault_token = "s.xxxx"  (or set VAULT_TOKEN env var)
//   mount_path  = "secret"  (KV mount, default)
//
// API: GET {addr}/v1/{mount}/data/{key}
// Response: {"data":{"data":{"value":"<secret>"}}}
// ============================================================================
class VaultSecretsProvider : public SecretsProvider {
public:
    struct Config {
        std::string addr;         // Vault server address
        std::string token;        // Vault token (can also use VAULT_TOKEN env var)
        std::string mount  = "secret";
        int         timeout_sec = 3;
    };

    explicit VaultSecretsProvider(Config cfg);

    std::string get(const std::string& key,
                    const std::string& default_val = "") override;
    bool        available() const override;
    std::string backend_name() const override { return "vault:" + cfg_.addr; }

private:
    Config cfg_;
};

// ============================================================================
// AwsSecretsProvider — AWS Secrets Manager
//
// Uses AWS SDK C++ or falls back to IMDS-based HTTP calls.
// Requires IAM role with secretsmanager:GetSecretValue permission.
// ============================================================================
class AwsSecretsProvider : public SecretsProvider {
public:
    struct Config {
        std::string region = "us-east-1";
        std::string prefix;         // optional key prefix, e.g. "apex-db/"
        int         timeout_sec = 3;
    };

    explicit AwsSecretsProvider(Config cfg);

    std::string get(const std::string& key,
                    const std::string& default_val = "") override;
    bool        available() const override;
    std::string backend_name() const override { return "aws-sm:" + cfg_.region; }

private:
    Config cfg_;
};

// ============================================================================
// CompositeSecretsProvider — chain of providers, first non-empty wins
// ============================================================================
class CompositeSecretsProvider : public SecretsProvider {
public:
    void add(std::unique_ptr<SecretsProvider> provider);

    std::string get(const std::string& key,
                    const std::string& default_val = "") override;
    bool        available() const override { return !providers_.empty(); }
    std::string backend_name() const override { return "composite"; }

    // List active backends (for diagnostics)
    std::vector<std::string> active_backends() const;

private:
    std::vector<std::unique_ptr<SecretsProvider>> providers_;
};

// ============================================================================
// SecretsProviderFactory — convenience factory
// ============================================================================
class SecretsProviderFactory {
public:
    /// Build a composite provider using auto-detection:
    ///   Vault (if VAULT_ADDR + VAULT_TOKEN env vars set)
    ///   → File (/run/secrets if exists)
    ///   → Env (always available as fallback)
    static std::unique_ptr<CompositeSecretsProvider> create_composite();

    /// Build from explicit Vault config
    static std::unique_ptr<CompositeSecretsProvider> create_with_vault(
        const VaultSecretsProvider::Config& vault_cfg);

    /// Build from explicit AWS config
    static std::unique_ptr<CompositeSecretsProvider> create_with_aws(
        const AwsSecretsProvider::Config& aws_cfg);
};

} // namespace apex::auth
