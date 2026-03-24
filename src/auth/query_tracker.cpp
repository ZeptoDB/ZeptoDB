// ============================================================================
// ZeptoDB: QueryTracker Implementation
// ============================================================================
#include "zeptodb/auth/query_tracker.h"

#include <openssl/rand.h>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace zeptodb::auth {

// ============================================================================
// register_query
// ============================================================================
std::string QueryTracker::register_query(
    const std::string& subject,
    const std::string& sql,
    std::shared_ptr<CancellationToken> token)
{
    ActiveQueryInfo info;
    info.query_id      = generate_id();
    info.subject       = subject;
    info.sql_preview   = sql.substr(0, SQL_PREVIEW_LEN);
    info.started_at_ns = now_ns();
    info.token         = std::move(token);

    std::unique_lock<std::shared_mutex> lk(mu_);
    auto id = info.query_id;
    active_.emplace(id, std::move(info));
    return id;
}

// ============================================================================
// complete
// ============================================================================
void QueryTracker::complete(const std::string& query_id) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    active_.erase(query_id);
}

// ============================================================================
// cancel
// ============================================================================
bool QueryTracker::cancel(const std::string& query_id) {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = active_.find(query_id);
    if (it == active_.end()) return false;
    if (it->second.token) it->second.token->cancel();
    return true;
}

// ============================================================================
// list
// ============================================================================
std::vector<ActiveQueryInfo> QueryTracker::list() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<ActiveQueryInfo> result;
    result.reserve(active_.size());
    for (const auto& [id, info] : active_)
        result.push_back(info);
    return result;
}

// ============================================================================
// active_count
// ============================================================================
size_t QueryTracker::active_count() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return active_.size();
}

// ============================================================================
// generate_id — "q_<16-char hex>"
// ============================================================================
std::string QueryTracker::generate_id() {
    unsigned char buf[8];
    RAND_bytes(buf, sizeof(buf));
    static const char hex[] = "0123456789abcdef";
    std::string id = "q_";
    for (int i = 0; i < 8; ++i) {
        id += hex[(buf[i] >> 4) & 0xF];
        id += hex[buf[i] & 0xF];
    }
    return id;
}

// ============================================================================
// now_ns
// ============================================================================
int64_t QueryTracker::now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace zeptodb::auth
