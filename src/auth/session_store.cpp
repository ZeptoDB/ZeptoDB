// ============================================================================
// ZeptoDB: Server-Side Session Store Implementation
// ============================================================================
#include "zeptodb/auth/session_store.h"

#include <random>
#include <sstream>
#include <iomanip>

namespace zeptodb::auth {

SessionStore::SessionStore() : config_{} {}
SessionStore::SessionStore(Config config) : config_(std::move(config)) {}

int64_t SessionStore::now_ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string SessionStore::generate_id() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::ostringstream os;
    os << std::hex << std::setfill('0')
       << std::setw(16) << rng() << std::setw(16) << rng();
    return os.str();
}

std::string SessionStore::create(const std::string& subject, const std::string& name,
                                  Role role, const std::string& source,
                                  const std::vector<std::string>& allowed_symbols,
                                  const std::string& tenant_id,
                                  const std::string& refresh_token) {
    std::lock_guard lock(mu_);

    // Evict expired if at capacity
    if (sessions_.size() >= config_.max_sessions) {
        auto now = now_ns();
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (now > it->second.expires_at_ns)
                it = sessions_.erase(it);
            else
                ++it;
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
    s.refresh_token   = refresh_token;
    s.created_at_ns   = now;
    s.expires_at_ns   = now + config_.session_ttl_s * 1'000'000'000LL;
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
    int64_t refresh_threshold = it->second.expires_at_ns
        - config_.refresh_window_s * 1'000'000'000LL;
    if (now > refresh_threshold) {
        it->second.expires_at_ns = now + config_.session_ttl_s * 1'000'000'000LL;
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
    os << config_.cookie_name << "=" << session_id
       << "; Path=/; Max-Age=" << config_.session_ttl_s
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
