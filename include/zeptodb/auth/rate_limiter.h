#pragma once
// ============================================================================
// ZeptoDB: Rate Limiter (Token Bucket)
// ============================================================================
// Per-identity and per-IP rate limiting using the token bucket algorithm.
//
// Token bucket properties:
//   - Each identity starts with `burst_capacity` tokens
//   - Tokens refill at `requests_per_minute / 60` per second
//   - Each request consumes 1 token
//   - When tokens < 1.0: request is rate-limited (429)
//
// Thread safety: per-bucket mutex; bucket map protected by shared_mutex.
// ============================================================================

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <cstdint>

namespace zeptodb::auth {

enum class RateDecision { ALLOWED, RATE_LIMITED };

class RateLimiter {
public:
    struct Config {
        uint32_t requests_per_minute = 1000;  // per-identity limit
        uint32_t burst_capacity      = 200;   // max burst tokens
        uint32_t per_ip_rpm          = 10000; // per-IP limit (0 = disabled)
        uint32_t ip_burst            = 500;
    };

    explicit RateLimiter(Config config);

    // Check and consume one token for the given identity (API key id or JWT sub).
    RateDecision check_identity(const std::string& identity);

    // Check and consume one token for the given IP address.
    RateDecision check_ip(const std::string& ip);

    // Remove buckets not accessed in the last `max_idle_sec` seconds.
    // Call periodically to prevent memory growth.
    void cleanup(int64_t max_idle_sec = 300);

    // Expose config (for admin API)
    const Config& config() const { return config_; }

    // Number of tracked identities/IPs (for diagnostics)
    size_t identity_bucket_count() const;
    size_t ip_bucket_count() const;

private:
    struct Bucket {
        mutable std::mutex mu;
        double   tokens;
        int64_t  last_ns;
        double   capacity;
        double   refill_per_ns;  // tokens per nanosecond
    };

    Config config_;

    mutable std::shared_mutex identity_mu_;
    std::unordered_map<std::string, std::unique_ptr<Bucket>> identity_buckets_;

    mutable std::shared_mutex ip_mu_;
    std::unordered_map<std::string, std::unique_ptr<Bucket>> ip_buckets_;

    Bucket* get_or_create(const std::string& key,
                          std::shared_mutex& map_mu,
                          std::unordered_map<std::string, std::unique_ptr<Bucket>>& map,
                          double capacity, double rpm);

    RateDecision consume(Bucket* bucket);

    static int64_t now_ns();
};

} // namespace zeptodb::auth
