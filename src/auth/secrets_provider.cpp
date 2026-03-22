// ============================================================================
// APEX-DB: Secrets Provider Implementation
// ============================================================================
#include "apex/auth/secrets_provider.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef APEX_AUTH_OPENSSL
// Vault/AWS providers use HTTPS — only available when OpenSSL is linked
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "third_party/httplib.h"

namespace apex::auth {

// ============================================================================
// EnvSecretsProvider
// ============================================================================
std::string EnvSecretsProvider::get(const std::string& key,
                                     const std::string& default_val)
{
    const char* v = std::getenv(key.c_str());
    return v ? std::string(v) : default_val;
}

// ============================================================================
// FileSecretsProvider
// ============================================================================
FileSecretsProvider::FileSecretsProvider(std::string base_dir)
    : base_dir_(std::move(base_dir))
{}

std::string FileSecretsProvider::get(const std::string& key,
                                      const std::string& default_val)
{
    std::string path = base_dir_ + "/" + key;
    std::ifstream f(path);
    if (!f.is_open()) return default_val;

    std::ostringstream buf;
    buf << f.rdbuf();
    std::string content = buf.str();

    // Strip trailing whitespace / newlines
    while (!content.empty() &&
           (content.back() == '\n' || content.back() == '\r' ||
            content.back() == ' '))
        content.pop_back();

    return content.empty() ? default_val : content;
}

bool FileSecretsProvider::available() const {
    struct stat st;
    return stat(base_dir_.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ============================================================================
// VaultSecretsProvider — HashiCorp Vault KV v2
// ============================================================================
VaultSecretsProvider::VaultSecretsProvider(Config cfg)
    : cfg_(std::move(cfg))
{
    // Fallback: VAULT_TOKEN env var
    if (cfg_.token.empty()) {
        const char* t = std::getenv("VAULT_TOKEN");
        if (t) cfg_.token = t;
    }
    // Fallback: VAULT_ADDR env var
    if (cfg_.addr.empty()) {
        const char* a = std::getenv("VAULT_ADDR");
        if (a) cfg_.addr = a;
    }
}

bool VaultSecretsProvider::available() const {
    return !cfg_.addr.empty() && !cfg_.token.empty();
}

std::string VaultSecretsProvider::get(const std::string& key,
                                       const std::string& default_val)
{
    if (!available()) return default_val;

    // Parse addr into host/port
    // Expect format: "https://host" or "https://host:port" or "http://..."
    std::string host;
    int port = 443;
    bool use_ssl = true;

    std::string addr = cfg_.addr;
    if (addr.substr(0, 8) == "https://") {
        host = addr.substr(8);
        port = 8200;  // Vault default HTTPS port
    } else if (addr.substr(0, 7) == "http://") {
        host = addr.substr(7);
        port = 8200;
        use_ssl = false;
    } else {
        host = addr;
    }

    // Split host:port
    auto colon = host.rfind(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(host.substr(colon + 1)); } catch (...) {}
        host = host.substr(0, colon);
    }

    // Path: /v1/{mount}/data/{key}
    std::string path = "/v1/" + cfg_.mount + "/data/" + key;

    // Helper: parse "value" field out of Vault KV v2 JSON response body
    auto parse_body = [&](const std::string& body) -> std::string {
        auto vpos = body.rfind("\"value\"");
        if (vpos == std::string::npos) return default_val;
        auto colon_pos = body.find(':', vpos + 7);
        if (colon_pos == std::string::npos) return default_val;
        auto q1 = body.find('"', colon_pos + 1);
        if (q1 == std::string::npos) return default_val;
        auto q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos) return default_val;
        std::string val = body.substr(q1 + 1, q2 - q1 - 1);
        return val.empty() ? default_val : val;
    };

    httplib::Headers headers = {{"X-Vault-Token", cfg_.token}};

#ifdef APEX_AUTH_OPENSSL
    if (use_ssl) {
        httplib::SSLClient cli(host, port);
        cli.set_connection_timeout(cfg_.timeout_sec);
        cli.set_read_timeout(cfg_.timeout_sec);
        auto res = cli.Get(path, headers);
        if (!res || res->status != 200) return default_val;
        return parse_body(res->body);
    }
#else
    (void)use_ssl;
#endif
    {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(cfg_.timeout_sec);
        cli.set_read_timeout(cfg_.timeout_sec);
        auto res = cli.Get(path, headers);
        if (!res || res->status != 200) return default_val;
        return parse_body(res->body);
    }
}

// ============================================================================
// AwsSecretsProvider — AWS Secrets Manager
// ============================================================================
AwsSecretsProvider::AwsSecretsProvider(Config cfg)
    : cfg_(std::move(cfg))
{}

bool AwsSecretsProvider::available() const {
    return !cfg_.region.empty();
}

std::string AwsSecretsProvider::get(const std::string& key,
                                     const std::string& default_val)
{
    // AWS Secrets Manager uses SigV4 signed requests which require
    // AWS SDK C++ or a full SigV4 implementation.
    //
    // Production integration path:
    //   Option A: Link aws-sdk-cpp-secretsmanager
    //   Option B: Use IRSA + AWS CLI subprocess (for K8s deployments)
    //   Option C: Implement SigV4 signing manually with OpenSSL HMAC
    //
    // For now, fall through to env/file providers.
    // TODO: Implement AWS SigV4 request signing for full AWS SM support.
    //
    // Workaround for K8s deployments: mount secrets via ExternalSecrets Operator
    // which writes AWS SM secrets to K8s Secret objects → FileSecretsProvider reads them.
    (void)key;
    return default_val;
}

// ============================================================================
// CompositeSecretsProvider
// ============================================================================
void CompositeSecretsProvider::add(std::unique_ptr<SecretsProvider> provider) {
    providers_.push_back(std::move(provider));
}

std::string CompositeSecretsProvider::get(const std::string& key,
                                           const std::string& default_val)
{
    for (auto& p : providers_) {
        if (!p->available()) continue;
        auto v = p->get(key, "");
        if (!v.empty()) return v;
    }
    return default_val;
}

std::vector<std::string> CompositeSecretsProvider::active_backends() const {
    std::vector<std::string> names;
    for (const auto& p : providers_)
        if (p->available())
            names.push_back(p->backend_name());
    return names;
}

// ============================================================================
// SecretsProviderFactory
// ============================================================================
std::unique_ptr<CompositeSecretsProvider>
SecretsProviderFactory::create_composite()
{
    auto composite = std::make_unique<CompositeSecretsProvider>();

    // 1. Vault (if env vars set)
    const char* vault_addr  = std::getenv("VAULT_ADDR");
    const char* vault_token = std::getenv("VAULT_TOKEN");
    if (vault_addr && vault_token) {
        VaultSecretsProvider::Config vcfg;
        vcfg.addr  = vault_addr;
        vcfg.token = vault_token;
        composite->add(std::make_unique<VaultSecretsProvider>(vcfg));
    }

    // 2. File secrets (Docker / K8s)
    auto file_provider = std::make_unique<FileSecretsProvider>();
    if (file_provider->available())
        composite->add(std::move(file_provider));

    // 3. Env (always last / fallback)
    composite->add(std::make_unique<EnvSecretsProvider>());

    return composite;
}

std::unique_ptr<CompositeSecretsProvider>
SecretsProviderFactory::create_with_vault(
    const VaultSecretsProvider::Config& vault_cfg)
{
    auto composite = std::make_unique<CompositeSecretsProvider>();
    composite->add(std::make_unique<VaultSecretsProvider>(vault_cfg));
    composite->add(std::make_unique<FileSecretsProvider>());
    composite->add(std::make_unique<EnvSecretsProvider>());
    return composite;
}

std::unique_ptr<CompositeSecretsProvider>
SecretsProviderFactory::create_with_aws(
    const AwsSecretsProvider::Config& aws_cfg)
{
    auto composite = std::make_unique<CompositeSecretsProvider>();
    composite->add(std::make_unique<AwsSecretsProvider>(aws_cfg));
    composite->add(std::make_unique<FileSecretsProvider>());
    composite->add(std::make_unique<EnvSecretsProvider>());
    return composite;
}

} // namespace apex::auth
