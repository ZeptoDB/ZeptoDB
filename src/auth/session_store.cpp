// ============================================================================
// ZeptoDB: Server-Side Session Store Implementation
// ============================================================================
#include "zeptodb/auth/session_store.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace zeptodb::auth {

namespace {

constexpr int64_t kNanosecondsPerSecond = 1'000'000'000LL;

int64_t add_seconds_saturated(int64_t base, int64_t seconds) {
    if (seconds > std::numeric_limits<int64_t>::max() /
                      kNanosecondsPerSecond) {
        return std::numeric_limits<int64_t>::max();
    }
    const int64_t nanoseconds = seconds * kNanosecondsPerSecond;
    if (base > std::numeric_limits<int64_t>::max() - nanoseconds) {
        return std::numeric_limits<int64_t>::max();
    }
    return base + nanoseconds;
}

int64_t subtract_seconds_saturated(int64_t base, int64_t seconds) {
    if (seconds > std::numeric_limits<int64_t>::max() /
                      kNanosecondsPerSecond) {
        return std::numeric_limits<int64_t>::min();
    }
    const int64_t nanoseconds = seconds * kNanosecondsPerSecond;
    if (base < std::numeric_limits<int64_t>::min() + nanoseconds) {
        return std::numeric_limits<int64_t>::min();
    }
    return base - nanoseconds;
}

}  // namespace

SessionStore::SessionStore() : SessionStore(Config{}) {}

SessionStore::SessionStore(Config config) : config_(std::move(config)) {
    if (config_.session_ttl_s < 0) {
        throw std::invalid_argument(
            "SessionStore: session_ttl_s must be non-negative");
    }
    if (config_.refresh_window_s < 0) {
        throw std::invalid_argument("SessionStore: refresh_window_s must be non-negative");
    }
    if (config_.max_session_lifetime_s < 0) {
        throw std::invalid_argument(
            "SessionStore: max_session_lifetime_s must be non-negative");
    }
}

int64_t SessionStore::now_ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string SessionStore::generate_id() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("SessionStore: RAND_bytes failed");
    }
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        os << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return os.str();
}

std::string SessionStore::create(const std::string& subject, const std::string& name,
                                  Role role, const std::string& source,
                                  const std::vector<std::string>& allowed_symbols,
                                  const std::string& tenant_id,
                                  const std::string& refresh_token,
                                  const std::vector<std::string>& allowed_tables) {
    std::lock_guard lock(mu_);

    if (config_.max_sessions == 0) {
        throw std::runtime_error("SessionStore: max_sessions is zero");
    }

    // Evict expired if at capacity
    if (sessions_.size() >= config_.max_sessions) {
        auto now = now_ns();
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (now > it->second.expires_at_ns)
                it = sessions_.erase(it);
            else
                ++it;
        }
        if (sessions_.size() >= config_.max_sessions) {
            throw std::runtime_error("SessionStore: capacity unavailable");
        }
    }

    auto id = generate_id();
    auto now = now_ns();
    Session s;
    s.session_id      = id;
    s.subject         = subject;
    s.name            = name;
    s.role            = role;
    s.source          = source;
    s.allowed_symbols = allowed_symbols;
    s.tenant_id       = tenant_id;
    s.allowed_tables  = allowed_tables;
    s.refresh_token   = refresh_token;
    s.created_at_ns   = now;
    const int64_t idle_expiry =
        add_seconds_saturated(now, config_.session_ttl_s);
    const int64_t absolute_expiry = config_.max_session_lifetime_s > 0
        ? add_seconds_saturated(now, config_.max_session_lifetime_s)
        : std::numeric_limits<int64_t>::max();
    s.expires_at_ns   = std::min(idle_expiry, absolute_expiry);
    s.last_active_ns  = now;
    sessions_[id] = std::move(s);
    return id;
}

std::optional<Session> SessionStore::get(const std::string& session_id) {
    std::lock_guard lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return std::nullopt;

    auto now = now_ns();
    if (now > it->second.expires_at_ns) {
        sessions_.erase(it);
        return std::nullopt;
    }

    // Sliding window: extend if active within refresh window
    const int64_t refresh_threshold = subtract_seconds_saturated(
        it->second.expires_at_ns, config_.refresh_window_s);
    if (now > refresh_threshold) {
        const int64_t idle_expiry =
            add_seconds_saturated(now, config_.session_ttl_s);
        const int64_t absolute_expiry =
            config_.max_session_lifetime_s > 0
                ? add_seconds_saturated(
                    it->second.created_at_ns,
                    config_.max_session_lifetime_s)
                : std::numeric_limits<int64_t>::max();
        it->second.expires_at_ns = std::min(idle_expiry, absolute_expiry);
    }
    it->second.last_active_ns = now;
    return it->second;
}

bool SessionStore::destroy(const std::string& session_id) {
    std::lock_guard lock(mu_);
    return sessions_.erase(session_id) > 0;
}

bool SessionStore::update_refresh_token(const std::string& session_id,
                                         const std::string& new_refresh_token) {
    std::lock_guard lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second.refresh_token = new_refresh_token;
    return true;
}

bool SessionStore::update_identity(
    const std::string& session_id, const std::string& name, Role role,
    const std::string& source,
    const std::vector<std::string>& allowed_symbols,
    const std::string& tenant_id,
    const std::vector<std::string>& allowed_tables,
    const std::string& refresh_token) {
    std::lock_guard lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    it->second.name = name;
    it->second.role = role;
    it->second.source = source;
    it->second.allowed_symbols = allowed_symbols;
    it->second.tenant_id = tenant_id;
    it->second.allowed_tables = allowed_tables;
    it->second.refresh_token = refresh_token;
    return true;
}

size_t SessionStore::size() const {
    std::lock_guard lock(mu_);
    return sessions_.size();
}

size_t SessionStore::evict_expired() {
    std::lock_guard lock(mu_);
    auto now = now_ns();
    size_t count = 0;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (now > it->second.expires_at_ns) {
            it = sessions_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    return count;
}

std::string SessionStore::make_cookie(const std::string& session_id) const {
    std::ostringstream os;
    const int64_t max_age = config_.max_session_lifetime_s > 0
        ? std::min(config_.session_ttl_s, config_.max_session_lifetime_s)
        : config_.session_ttl_s;
    os << config_.cookie_name << "=" << session_id
       << "; Path=/; Max-Age=" << max_age
       << "; SameSite=Lax";
    if (config_.cookie_httponly) os << "; HttpOnly";
    if (config_.cookie_secure)  os << "; Secure";
    return os.str();
}

std::string SessionStore::make_clear_cookie() const {
    std::ostringstream os;
    os << config_.cookie_name << "=; Path=/; Max-Age=0; SameSite=Lax";
    if (config_.cookie_httponly) os << "; HttpOnly";
    if (config_.cookie_secure)  os << "; Secure";
    return os.str();
}

} // namespace zeptodb::auth
