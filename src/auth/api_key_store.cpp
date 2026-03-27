// ============================================================================
// ZeptoDB: ApiKeyStore Implementation
// ============================================================================
#include "zeptodb/auth/api_key_store.h"

#include <openssl/sha.h>
#include <openssl/rand.h>

#include <fstream>
#include <sstream>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace zeptodb::auth {

// ============================================================================
// File format (one key per line, lines starting with '#' are comments):
//
//   id|name|key_hash|role|symbols|enabled|created_at_ns[|tables[|tenant_id[|expires_at_ns]]]
//
// Fields:
//   id             — short key id, e.g. "ak_7f3k"
//   name           — human label (no pipe characters)
//   key_hash       — sha256 hex of full "zepto_<hex>" key
//   role           — role string (admin/writer/reader/analyst/metrics)
//   symbols        — comma-separated symbol whitelist, empty = unrestricted
//   enabled        — "1" or "0"
//   created_at_ns  — nanoseconds since Unix epoch
//   tables         — comma-separated table whitelist, empty = unrestricted (optional)
//   tenant_id      — tenant identifier, empty = no tenant (optional)
//   expires_at_ns  — nanoseconds since Unix epoch, 0 = never (optional)
// ============================================================================

static constexpr const char* FILE_HEADER = "# zeptodb-keys-v1\n";
static constexpr const char  SEP = '|';

// ============================================================================
// Constructor
// ============================================================================
ApiKeyStore::ApiKeyStore(std::string config_path)
    : config_path_(std::move(config_path))
{
    load();
}

// ============================================================================
// is_expired
// ============================================================================
bool ApiKeyEntry::is_expired() const {
    if (expires_at_ns <= 0) return false;
    using namespace std::chrono;
    auto now = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
    return now > expires_at_ns;
}

// ============================================================================
// create_key
// ============================================================================
std::string ApiKeyStore::create_key(const std::string& name,
                                     Role role,
                                     const std::vector<std::string>& allowed_symbols,
                                     const std::vector<std::string>& allowed_tables,
                                     const std::string& tenant_id,
                                     int64_t expires_at_ns)
{
    std::string full_key = generate_key();

    ApiKeyEntry entry;
    entry.id              = generate_key_id();
    entry.name            = name;
    entry.key_hash        = sha256_hex(full_key);
    entry.role            = role;
    entry.allowed_symbols = allowed_symbols;
    entry.allowed_tables  = allowed_tables;
    entry.tenant_id       = tenant_id;
    entry.enabled         = true;
    entry.created_at_ns   = now_ns();
    entry.expires_at_ns   = expires_at_ns;

    std::lock_guard<std::mutex> lk(mutex_);
    entries_.push_back(std::move(entry));
    save();

    return full_key;
}

// ============================================================================
// validate
// ============================================================================
std::optional<ApiKeyEntry> ApiKeyStore::validate(const std::string& key) const {
    if (key.empty()) return std::nullopt;

    std::string hash = sha256_hex(key);

    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& e : entries_) {
        if (e.enabled && !e.is_expired() && e.key_hash == hash) {
            e.last_used_ns = now_ns();
            return e;
        }
    }
    return std::nullopt;
}

// ============================================================================
// revoke
// ============================================================================
bool ApiKeyStore::revoke(const std::string& key_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
        if (e.id == key_id) {
            e.enabled = false;
            save();
            return true;
        }
    }
    return false;
}

// ============================================================================
// update_key
// ============================================================================
bool ApiKeyStore::update_key(const std::string& key_id,
                              const std::optional<std::vector<std::string>>& symbols,
                              const std::optional<std::vector<std::string>>& tables,
                              const std::optional<bool>& enabled,
                              const std::optional<std::string>& tenant_id,
                              const std::optional<int64_t>& expires_at_ns)
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
        if (e.id == key_id) {
            if (symbols)       e.allowed_symbols = *symbols;
            if (tables)        e.allowed_tables  = *tables;
            if (enabled)       e.enabled         = *enabled;
            if (tenant_id)     e.tenant_id       = *tenant_id;
            if (expires_at_ns) e.expires_at_ns   = *expires_at_ns;
            save();
            return true;
        }
    }
    return false;
}

// ============================================================================
// list
// ============================================================================
std::vector<ApiKeyEntry> ApiKeyStore::list() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return entries_;
}

// ============================================================================
// reload
// ============================================================================
void ApiKeyStore::reload() {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.clear();
    load();
}

// ============================================================================
// load — caller must NOT hold mutex_ (called from constructor and reload)
// ============================================================================
void ApiKeyStore::load() {
    std::ifstream f(config_path_);
    if (!f.is_open()) return;  // file not yet created — start empty

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Split on '|'
        auto split = [&](const std::string& s) {
            std::vector<std::string> parts;
            std::istringstream ss(s);
            std::string token;
            while (std::getline(ss, token, SEP))
                parts.push_back(std::move(token));
            return parts;
        };

        auto parts = split(line);
        if (parts.size() < 7) continue;

        ApiKeyEntry e;
        e.id       = parts[0];
        e.name     = parts[1];
        e.key_hash = parts[2];
        e.role     = role_from_string(parts[3]);
        // Parse comma-separated symbols
        if (!parts[4].empty()) {
            std::istringstream ss(parts[4]);
            std::string sym;
            while (std::getline(ss, sym, ','))
                if (!sym.empty()) e.allowed_symbols.push_back(sym);
        }
        e.enabled       = (parts[5] == "1");
        try { e.created_at_ns = std::stoll(parts[6]); } catch (...) {}

        // Parse comma-separated tables (field 7, optional)
        if (parts.size() > 7 && !parts[7].empty()) {
            std::istringstream ts(parts[7]);
            std::string tbl;
            while (std::getline(ts, tbl, ','))
                if (!tbl.empty()) e.allowed_tables.push_back(tbl);
        }

        // Parse tenant_id (field 8, optional)
        if (parts.size() > 8) e.tenant_id = parts[8];

        // Parse expires_at_ns (field 9, optional)
        if (parts.size() > 9 && !parts[9].empty()) {
            try { e.expires_at_ns = std::stoll(parts[9]); } catch (...) {}
        }

        entries_.push_back(std::move(e));
    }
}

// ============================================================================
// save — caller must hold mutex_
// ============================================================================
void ApiKeyStore::save() const {
    std::ofstream f(config_path_, std::ios::trunc);
    if (!f.is_open())
        throw std::runtime_error("ApiKeyStore: cannot write " + config_path_);

    f << FILE_HEADER;

    for (const auto& e : entries_) {
        f << e.id << SEP
          << e.name << SEP
          << e.key_hash << SEP
          << role_to_string(e.role) << SEP;

        // Symbols (comma-separated)
        for (size_t i = 0; i < e.allowed_symbols.size(); ++i) {
            if (i > 0) f << ',';
            f << e.allowed_symbols[i];
        }

        f << SEP
          << (e.enabled ? '1' : '0') << SEP
          << e.created_at_ns << SEP;

        // Tables (comma-separated)
        for (size_t i = 0; i < e.allowed_tables.size(); ++i) {
            if (i > 0) f << ',';
            f << e.allowed_tables[i];
        }

        f << SEP << e.tenant_id
          << SEP << e.expires_at_ns
          << '\n';
    }
}

// ============================================================================
// sha256_hex
// ============================================================================
std::string ApiKeyStore::sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), digest);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        result += hex[(digest[i] >> 4) & 0xF];
        result += hex[digest[i] & 0xF];
    }
    return result;
}

// ============================================================================
// generate_key — returns "zepto_<64-char hex>"
// ============================================================================
std::string ApiKeyStore::generate_key() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("ApiKeyStore: RAND_bytes failed");

    static const char hex[] = "0123456789abcdef";
    std::string result = "zepto_";
    result.reserve(5 + 64);
    for (int i = 0; i < 32; ++i) {
        result += hex[(buf[i] >> 4) & 0xF];
        result += hex[buf[i] & 0xF];
    }
    return result;
}

// ============================================================================
// generate_key_id — returns "ak_<8-char hex>"
// ============================================================================
std::string ApiKeyStore::generate_key_id() {
    unsigned char buf[4];
    RAND_bytes(buf, sizeof(buf));

    static const char hex[] = "0123456789abcdef";
    std::string result = "ak_";
    for (int i = 0; i < 4; ++i) {
        result += hex[(buf[i] >> 4) & 0xF];
        result += hex[buf[i] & 0xF];
    }
    return result;
}

// ============================================================================
// now_ns
// ============================================================================
int64_t ApiKeyStore::now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace zeptodb::auth
