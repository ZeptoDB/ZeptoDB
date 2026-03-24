#pragma once
// ============================================================================
// ZeptoDB: In-Memory Audit Buffer
// ============================================================================
// Thread-safe ring buffer of the last N audit events.
// Exposed via GET /admin/audit for compliance inspection.
// Events are also written to the spdlog audit logger.
// ============================================================================
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstdint>

namespace zeptodb::auth {

struct AuditEvent {
    int64_t     timestamp_ns = 0;
    std::string subject;       // API key id or JWT sub
    std::string role_str;      // role name
    std::string action;        // "POST /"
    std::string detail;        // "apikey-auth" | "jwt-auth" | "public"
    std::string remote_addr;   // client IP
};

class AuditBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 10'000;

    explicit AuditBuffer(size_t capacity = DEFAULT_CAPACITY)
        : capacity_(capacity) {}

    void push(AuditEvent e) {
        std::lock_guard<std::mutex> lk(mu_);
        ring_.push_back(std::move(e));
        if (ring_.size() > capacity_) ring_.pop_front();
    }

    // Return the last `n` events (oldest first), or all if n == 0.
    std::vector<AuditEvent> last(size_t n = 0) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (n == 0 || n >= ring_.size()) {
            return std::vector<AuditEvent>(ring_.begin(), ring_.end());
        }
        return std::vector<AuditEvent>(
            ring_.end() - static_cast<std::ptrdiff_t>(n), ring_.end());
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return ring_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        ring_.clear();
    }

private:
    size_t                capacity_;
    std::deque<AuditEvent> ring_;
    mutable std::mutex    mu_;
};

} // namespace zeptodb::auth
