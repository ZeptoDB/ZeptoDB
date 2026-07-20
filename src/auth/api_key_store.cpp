// ============================================================================
// ZeptoDB: ApiKeyStore Implementation
// ============================================================================
#include "zeptodb/auth/api_key_store.h"
#include "zeptodb/auth/vault_key_backend.h"

#include <openssl/sha.h>
#include <openssl/rand.h>

#include <fstream>
#include <sstream>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <cstring>
#include <string_view>
#include <unordered_set>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

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
static constexpr size_t MAX_SCOPE_ITEMS = 256;
static constexpr size_t MAX_FIELD_BYTES = 4096;
static constexpr size_t MAX_SCOPE_BYTES = 8192;

static bool is_safe_store_field(std::string_view value,
                                bool reject_comma = false) {
    if (value.size() > MAX_FIELD_BYTES || value.find(SEP) != value.npos ||
        (reject_comma && value.find(',') != value.npos)) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte < 0x20U || byte == 0x7fU;
    });
}

static bool is_valid_scope_list(const std::vector<std::string>& values) {
    if (values.size() > MAX_SCOPE_ITEMS) return false;
    size_t serialized_bytes = values.empty() ? 0 : values.size() - 1;
    for (const auto& value : values) {
        if (value.empty() || !is_safe_store_field(value, true) ||
            value.size() > MAX_SCOPE_BYTES -
                std::min(serialized_bytes, MAX_SCOPE_BYTES)) {
            return false;
        }
        serialized_bytes += value.size();
    }
    return serialized_bytes <= MAX_SCOPE_BYTES;
}

static bool parse_scope_field(std::string_view field,
                              std::vector<std::string>* output) {
    output->clear();
    if (field.empty()) return true;
    size_t start = 0;
    while (start <= field.size()) {
        const size_t separator = field.find(',', start);
        const auto value = field.substr(
            start, separator == field.npos ? field.npos : separator - start);
        if (value.empty()) return false;
        output->emplace_back(value);
        if (separator == field.npos) break;
        start = separator + 1;
    }
    return is_valid_scope_list(*output);
}

static bool parse_int64_field(std::string_view field, int64_t* output) {
    if (field.empty()) return false;
    const auto [end, error] = std::from_chars(
        field.data(), field.data() + field.size(), *output);
    return error == std::errc{} && end == field.data() + field.size();
}

static bool is_sha256_hex(std::string_view value) {
    return value.size() == SHA256_DIGEST_LENGTH * 2 &&
        std::all_of(value.begin(), value.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

static bool is_valid_entry(const ApiKeyEntry& entry) {
    return !entry.id.empty() && is_safe_store_field(entry.id, true) &&
        !entry.name.empty() && is_safe_store_field(entry.name, true) &&
        is_sha256_hex(entry.key_hash) && entry.role != Role::UNKNOWN &&
        is_valid_scope_list(entry.allowed_symbols) &&
        is_valid_scope_list(entry.allowed_tables) &&
        is_safe_store_field(entry.tenant_id, true) &&
        entry.created_at_ns >= 0 && entry.expires_at_ns >= 0;
}

static void validate_mutable_fields(
    const std::string& name, Role role,
    const std::vector<std::string>& allowed_symbols,
    const std::vector<std::string>& allowed_tables,
    const std::string& tenant_id, int64_t expires_at_ns) {
    if (name.empty() || !is_safe_store_field(name, true)) {
        throw std::invalid_argument("API key name contains unsupported characters");
    }
    if (role == Role::UNKNOWN) {
        throw std::invalid_argument("API key role must be recognized");
    }
    if (!is_valid_scope_list(allowed_symbols) ||
        !is_valid_scope_list(allowed_tables)) {
        throw std::invalid_argument("API key scope contains an invalid entry");
    }
    if (!is_safe_store_field(tenant_id, true)) {
        throw std::invalid_argument("API key tenant contains unsupported characters");
    }
    if (expires_at_ns < 0) {
        throw std::invalid_argument("API key expiry must not be negative");
    }
}

// ============================================================================
// Constructor
// ============================================================================
ApiKeyStore::ApiKeyStore(std::string config_path)
    : config_path_(std::move(config_path))
{
    load();
}

ApiKeyStore::ApiKeyStore(std::string config_path,
                         std::unique_ptr<VaultKeyBackend> vault_backend)
    : config_path_(std::move(config_path)),
      vault_backend_(std::move(vault_backend))
{
    load();
    sync_from_vault();
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
    validate_mutable_fields(name, role, allowed_symbols, allowed_tables,
                            tenant_id, expires_at_ns);
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
    try {
        save();
    } catch (...) {
        entries_.pop_back();
        throw;
    }
    sync_to_vault(entries_.back());

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
            const bool was_enabled = e.enabled;
            e.enabled = false;
            try {
                save();
            } catch (...) {
                e.enabled = was_enabled;
                throw;
            }
            sync_to_vault(e);
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
    if (symbols && !is_valid_scope_list(*symbols)) {
        throw std::invalid_argument("API key symbol scope contains an invalid entry");
    }
    if (tables && !is_valid_scope_list(*tables)) {
        throw std::invalid_argument("API key table scope contains an invalid entry");
    }
    if (tenant_id && !is_safe_store_field(*tenant_id, true)) {
        throw std::invalid_argument("API key tenant contains unsupported characters");
    }
    if (expires_at_ns && *expires_at_ns < 0) {
        throw std::invalid_argument("API key expiry must not be negative");
    }
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
        if (e.id == key_id) {
            const ApiKeyEntry previous = e;
            if (symbols)       e.allowed_symbols = *symbols;
            if (tables)        e.allowed_tables  = *tables;
            if (enabled)       e.enabled         = *enabled;
            if (tenant_id)     e.tenant_id       = *tenant_id;
            if (expires_at_ns) e.expires_at_ns   = *expires_at_ns;
            try {
                save();
            } catch (...) {
                e = previous;
                throw;
            }
            sync_to_vault(e);
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
    load();
}

// ============================================================================
// load — caller must NOT hold mutex_ (called from constructor and reload)
// ============================================================================
void ApiKeyStore::load() {
    std::ifstream f(config_path_);
    if (!f.is_open()) {
        entries_.clear();
        return;  // file not yet created — start empty
    }

    std::vector<ApiKeyEntry> loaded;
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> hashes;
    std::string line;
    size_t line_number = 0;
    while (std::getline(f, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') continue;

        // Split on '|' while preserving trailing empty optional fields.
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= line.size()) {
            const size_t separator = line.find(SEP, start);
            parts.emplace_back(line.substr(
                start, separator == std::string::npos
                    ? std::string::npos
                    : separator - start));
            if (separator == std::string::npos) break;
            start = separator + 1;
        }
        auto malformed = [line_number]() {
            throw std::runtime_error(
                "ApiKeyStore: malformed credential entry at line " +
                std::to_string(line_number));
        };
        if (parts.size() < 7 || parts.size() > 10) malformed();

        ApiKeyEntry e;
        e.id       = parts[0];
        e.name     = parts[1];
        e.key_hash = parts[2];
        e.role     = role_from_string(parts[3]);
        if (!parse_scope_field(parts[4], &e.allowed_symbols)) malformed();
        if (parts[5] != "0" && parts[5] != "1") malformed();
        e.enabled = parts[5] == "1";
        if (!parse_int64_field(parts[6], &e.created_at_ns) ||
            e.created_at_ns < 0) {
            malformed();
        }

        // Parse comma-separated tables (field 7, optional)
        if (parts.size() > 7 &&
            !parse_scope_field(parts[7], &e.allowed_tables)) {
            malformed();
        }

        // Parse tenant_id (field 8, optional)
        if (parts.size() > 8) e.tenant_id = parts[8];

        // Parse expires_at_ns (field 9, optional)
        if (parts.size() > 9 &&
            (!parse_int64_field(parts[9], &e.expires_at_ns) ||
             e.expires_at_ns < 0)) {
            malformed();
        }

        if (!is_valid_entry(e) || !ids.insert(e.id).second ||
            !hashes.insert(e.key_hash).second) {
            malformed();
        }
        loaded.push_back(std::move(e));
    }
    entries_ = std::move(loaded);
}

// ============================================================================
// save — caller must hold mutex_
// ============================================================================
void ApiKeyStore::save() const {
    namespace fs = std::filesystem;
    const fs::path target(config_path_);
    const fs::path parent = target.parent_path().empty()
        ? fs::path{"."}
        : target.parent_path();
    std::array<unsigned char, 8> random_suffix{};
    if (RAND_bytes(random_suffix.data(),
                   static_cast<int>(random_suffix.size())) != 1) {
        throw std::runtime_error("ApiKeyStore: RAND_bytes failed");
    }
    static constexpr char HEX[] = "0123456789abcdef";
    std::string suffix;
    suffix.reserve(random_suffix.size() * 2);
    for (const auto byte : random_suffix) {
        suffix.push_back(HEX[(byte >> 4) & 0x0fU]);
        suffix.push_back(HEX[byte & 0x0fU]);
    }
    const fs::path temporary = parent /
        (target.filename().string() + ".tmp." + suffix);

    auto remove_temporary = [&temporary]() noexcept {
        std::error_code error;
        fs::remove(temporary, error);
    };
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        throw std::runtime_error(
            "ApiKeyStore: cannot create a temporary credential file");
    }

    std::error_code permission_error;
    fs::permissions(
        temporary,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace,
        permission_error);
    if (permission_error) {
        f.close();
        remove_temporary();
        throw std::runtime_error(
            "ApiKeyStore: cannot secure the temporary credential file");
    }

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
    f.flush();
    f.close();
    if (!f) {
        remove_temporary();
        throw std::runtime_error(
            "ApiKeyStore: cannot write the temporary credential file");
    }

#if defined(__linux__)
    const int temporary_fd = ::open(
        temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (temporary_fd < 0) {
        remove_temporary();
        throw std::runtime_error(
            "ApiKeyStore: cannot reopen the temporary credential file");
    }
    const bool file_synced = ::fsync(temporary_fd) == 0;
    ::close(temporary_fd);
    if (!file_synced) {
        remove_temporary();
        throw std::runtime_error(
            "ApiKeyStore: cannot sync the temporary credential file");
    }
#endif

    std::error_code rename_error;
    fs::rename(temporary, target, rename_error);
    if (rename_error) {
        remove_temporary();
        throw std::runtime_error(
            "ApiKeyStore: cannot publish the credential file");
    }

#if defined(__linux__)
    const int directory_fd = ::open(
        parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        throw std::runtime_error(
            "ApiKeyStore: cannot open the credential directory");
    }
    const bool directory_synced = ::fsync(directory_fd) == 0;
    ::close(directory_fd);
    if (!directory_synced) {
        throw std::runtime_error(
            "ApiKeyStore: cannot sync the credential directory");
    }
#endif
}

// ============================================================================
// sync_from_vault — merge Vault entries not present locally
// ============================================================================
void ApiKeyStore::sync_from_vault() {
    if (!vault_backend_ || !vault_backend_->available()) return;

    auto vault_entries = vault_backend_->load_all();
    if (vault_entries.empty()) return;

    std::lock_guard<std::mutex> lk(mutex_);
    bool changed = false;
    for (auto& ve : vault_entries) {
        if (!is_valid_entry(ve)) {
            throw std::runtime_error(
                "ApiKeyStore: Vault returned an invalid credential entry");
        }
        bool found = false;
        for (const auto& le : entries_) {
            if (le.id == ve.id) { found = true; break; }
        }
        if (!found) {
            entries_.push_back(std::move(ve));
            changed = true;
        }
    }
    if (changed) save();
}

// ============================================================================
// sync_to_vault — write-through (best-effort, does not fail the operation)
// ============================================================================
void ApiKeyStore::sync_to_vault(const ApiKeyEntry& entry) {
    if (!vault_backend_ || !vault_backend_->available()) return;
    vault_backend_->store(entry);  // fire-and-forget
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
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("ApiKeyStore: RAND_bytes failed");

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
