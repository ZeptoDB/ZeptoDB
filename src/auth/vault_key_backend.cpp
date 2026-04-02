// ============================================================================
// ZeptoDB: Vault-backed API Key Backend Implementation
// ============================================================================
#include "zeptodb/auth/vault_key_backend.h"

#include <cstdlib>
#include <sstream>
#include <algorithm>

#ifdef ZEPTO_AUTH_OPENSSL
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "third_party/httplib.h"

namespace zeptodb::auth {

// ============================================================================
// Constructor
// ============================================================================
VaultKeyBackend::VaultKeyBackend(Config cfg)
    : cfg_(std::move(cfg))
{
    if (cfg_.token.empty()) {
        const char* t = std::getenv("VAULT_TOKEN");
        if (t) cfg_.token = t;
    }
    if (cfg_.addr.empty()) {
        const char* a = std::getenv("VAULT_ADDR");
        if (a) cfg_.addr = a;
    }
}

bool VaultKeyBackend::available() const {
    return !cfg_.addr.empty() && !cfg_.token.empty();
}

// ============================================================================
// Public API
// ============================================================================
bool VaultKeyBackend::store(const ApiKeyEntry& entry) {
    if (!available()) return false;
    std::string json = entry_to_json(entry);
    std::string body = R"({"data":)" + json + "}";
    if (!vault_put(key_path(entry.id), body)) return false;
    update_index_add(entry.id);
    return true;
}

std::optional<ApiKeyEntry> VaultKeyBackend::load(const std::string& key_id) {
    if (!available()) return std::nullopt;
    std::string resp = vault_get(key_path(key_id));
    if (resp.empty()) return std::nullopt;
    return json_to_entry(resp);
}

std::vector<ApiKeyEntry> VaultKeyBackend::load_all() {
    std::vector<ApiKeyEntry> result;
    if (!available()) return result;

    std::string idx_resp = vault_get(index_path());
    if (idx_resp.empty()) return result;

    auto ids = index_json_to_ids(idx_resp);
    for (const auto& id : ids) {
        auto entry = load(id);
        if (entry) result.push_back(std::move(*entry));
    }
    return result;
}

bool VaultKeyBackend::remove(const std::string& key_id) {
    if (!available()) return false;
    update_index_remove(key_id);
    return vault_delete(key_path(key_id));
}

// ============================================================================
// Vault HTTP helpers
// ============================================================================
// Parse Vault addr into host/port/ssl components
namespace {
struct VaultConn {
    std::string host;
    int port = 8200;
    bool ssl = true;
};

VaultConn parse_addr(const std::string& addr) {
    VaultConn c;
    std::string rest;
    if (addr.substr(0, 8) == "https://") {
        rest = addr.substr(8);
        c.ssl = true;
    } else if (addr.substr(0, 7) == "http://") {
        rest = addr.substr(7);
        c.ssl = false;
    } else {
        rest = addr;
    }
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        try { c.port = std::stoi(rest.substr(colon + 1)); } catch (...) {}
        c.host = rest.substr(0, colon);
    } else {
        c.host = rest;
    }
    return c;
}
} // anonymous namespace

std::string VaultKeyBackend::vault_get(const std::string& path) {
    auto conn = parse_addr(cfg_.addr);
    std::string url = "/v1/" + cfg_.mount + "/data/" + path;
    httplib::Headers headers = {{"X-Vault-Token", cfg_.token}};

    auto do_get = [&](auto& cli) -> std::string {
        cli.set_connection_timeout(cfg_.timeout_sec);
        cli.set_read_timeout(cfg_.timeout_sec);
        auto res = cli.Get(url, headers);
        if (!res || res->status != 200) return "";
        return res->body;
    };

#ifdef ZEPTO_AUTH_OPENSSL
    if (conn.ssl) {
        httplib::SSLClient cli(conn.host, conn.port);
        return do_get(cli);
    }
#endif
    httplib::Client cli(conn.host, conn.port);
    return do_get(cli);
}

bool VaultKeyBackend::vault_put(const std::string& path,
                                 const std::string& json_body) {
    auto conn = parse_addr(cfg_.addr);
    std::string url = "/v1/" + cfg_.mount + "/data/" + path;
    httplib::Headers headers = {{"X-Vault-Token", cfg_.token}};

    auto do_put = [&](auto& cli) -> bool {
        cli.set_connection_timeout(cfg_.timeout_sec);
        cli.set_read_timeout(cfg_.timeout_sec);
        auto res = cli.Post(url, headers, json_body, "application/json");
        return res && (res->status == 200 || res->status == 204);
    };

#ifdef ZEPTO_AUTH_OPENSSL
    if (conn.ssl) {
        httplib::SSLClient cli(conn.host, conn.port);
        return do_put(cli);
    }
#endif
    httplib::Client cli(conn.host, conn.port);
    return do_put(cli);
}

bool VaultKeyBackend::vault_delete(const std::string& path) {
    auto conn = parse_addr(cfg_.addr);
    // Vault KV v2 metadata delete
    std::string url = "/v1/" + cfg_.mount + "/metadata/" + path;
    httplib::Headers headers = {{"X-Vault-Token", cfg_.token}};

    auto do_del = [&](auto& cli) -> bool {
        cli.set_connection_timeout(cfg_.timeout_sec);
        cli.set_read_timeout(cfg_.timeout_sec);
        auto res = cli.Delete(url, headers);
        return res && (res->status == 200 || res->status == 204);
    };

#ifdef ZEPTO_AUTH_OPENSSL
    if (conn.ssl) {
        httplib::SSLClient cli(conn.host, conn.port);
        return do_del(cli);
    }
#endif
    httplib::Client cli(conn.host, conn.port);
    return do_del(cli);
}

// ============================================================================
// Path helpers
// ============================================================================
std::string VaultKeyBackend::key_path(const std::string& key_id) const {
    return cfg_.prefix + "/" + key_id;
}

std::string VaultKeyBackend::index_path() const {
    return cfg_.prefix + "/_index";
}

// ============================================================================
// Minimal JSON serialization (no external JSON library dependency)
// ============================================================================
static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// Extract a string value for a given key from a flat JSON object
static std::string json_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Extract the inner {"data":{...}} from Vault KV v2 response
static std::string extract_vault_data(const std::string& body) {
    // Vault KV v2: {"data":{"data":{...},...},...}
    // We need the inner "data" object
    auto outer = body.find("\"data\"");
    if (outer == std::string::npos) return "";
    auto inner_start = body.find("\"data\"", outer + 6);
    if (inner_start == std::string::npos) return "";
    auto brace = body.find('{', inner_start + 6);
    if (brace == std::string::npos) return "";
    // Find matching closing brace
    int depth = 1;
    size_t i = brace + 1;
    for (; i < body.size() && depth > 0; ++i) {
        if (body[i] == '{') ++depth;
        else if (body[i] == '}') --depth;
    }
    return body.substr(brace, i - brace);
}

std::string VaultKeyBackend::entry_to_json(const ApiKeyEntry& entry) {
    std::ostringstream o;
    o << "{";
    o << "\"id\":\"" << escape_json(entry.id) << "\",";
    o << "\"name\":\"" << escape_json(entry.name) << "\",";
    o << "\"key_hash\":\"" << escape_json(entry.key_hash) << "\",";
    o << "\"role\":\"" << role_to_string(entry.role) << "\",";

    // symbols as comma-separated
    std::string syms;
    for (size_t i = 0; i < entry.allowed_symbols.size(); ++i) {
        if (i > 0) syms += ",";
        syms += entry.allowed_symbols[i];
    }
    o << "\"symbols\":\"" << escape_json(syms) << "\",";

    // tables as comma-separated
    std::string tbls;
    for (size_t i = 0; i < entry.allowed_tables.size(); ++i) {
        if (i > 0) tbls += ",";
        tbls += entry.allowed_tables[i];
    }
    o << "\"tables\":\"" << escape_json(tbls) << "\",";
    o << "\"tenant_id\":\"" << escape_json(entry.tenant_id) << "\",";
    o << "\"enabled\":" << (entry.enabled ? "true" : "false") << ",";
    o << "\"created_at_ns\":" << entry.created_at_ns << ",";
    o << "\"expires_at_ns\":" << entry.expires_at_ns;
    o << "}";
    return o.str();
}

std::optional<ApiKeyEntry> VaultKeyBackend::json_to_entry(const std::string& raw) {
    std::string json = extract_vault_data(raw);
    if (json.empty()) return std::nullopt;

    ApiKeyEntry e;
    e.id       = json_str(json, "id");
    e.name     = json_str(json, "name");
    e.key_hash = json_str(json, "key_hash");
    if (e.id.empty() || e.key_hash.empty()) return std::nullopt;

    e.role = role_from_string(json_str(json, "role"));

    // Parse comma-separated symbols
    std::string syms = json_str(json, "symbols");
    if (!syms.empty()) {
        std::istringstream ss(syms);
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) e.allowed_symbols.push_back(tok);
    }

    // Parse comma-separated tables
    std::string tbls = json_str(json, "tables");
    if (!tbls.empty()) {
        std::istringstream ss(tbls);
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) e.allowed_tables.push_back(tok);
    }

    e.tenant_id = json_str(json, "tenant_id");

    std::string enabled_str = json_str(json, "enabled");
    // For booleans, check the raw JSON since json_str only gets quoted strings
    e.enabled = (json.find("\"enabled\":true") != std::string::npos);

    // Parse numeric fields from raw JSON
    auto parse_int64 = [&](const std::string& key) -> int64_t {
        std::string needle = "\"" + key + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return 0;
        auto start = pos + needle.size();
        try { return std::stoll(json.substr(start)); } catch (...) { return 0; }
    };
    e.created_at_ns = parse_int64("created_at_ns");
    e.expires_at_ns = parse_int64("expires_at_ns");

    return e;
}

std::string VaultKeyBackend::entries_to_index_json(
    const std::vector<std::string>& ids) {
    std::ostringstream o;
    o << "{\"ids\":\"";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) o << ",";
        o << ids[i];
    }
    o << "\"}";
    return o.str();
}

std::vector<std::string> VaultKeyBackend::index_json_to_ids(
    const std::string& raw) {
    std::string json = extract_vault_data(raw);
    if (json.empty()) return {};

    std::string ids_str = json_str(json, "ids");
    if (ids_str.empty()) return {};

    std::vector<std::string> result;
    std::istringstream ss(ids_str);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) result.push_back(tok);
    return result;
}

// ============================================================================
// Index management
// ============================================================================
void VaultKeyBackend::update_index_add(const std::string& key_id) {
    std::string resp = vault_get(index_path());
    auto ids = resp.empty() ? std::vector<std::string>{} : index_json_to_ids(resp);

    // Avoid duplicates
    if (std::find(ids.begin(), ids.end(), key_id) == ids.end())
        ids.push_back(key_id);

    std::string body = R"({"data":)" + entries_to_index_json(ids) + "}";
    vault_put(index_path(), body);
}

void VaultKeyBackend::update_index_remove(const std::string& key_id) {
    std::string resp = vault_get(index_path());
    if (resp.empty()) return;

    auto ids = index_json_to_ids(resp);
    ids.erase(std::remove(ids.begin(), ids.end(), key_id), ids.end());

    std::string body = R"({"data":)" + entries_to_index_json(ids) + "}";
    vault_put(index_path(), body);
}

} // namespace zeptodb::auth
