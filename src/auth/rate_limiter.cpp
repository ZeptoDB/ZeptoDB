// ============================================================================
// ZeptoDB: Rate Limiter Implementation
// ============================================================================
#include "zeptodb/auth/rate_limiter.h"

#include <chrono>
#include <algorithm>
#include <vector>

namespace zeptodb::auth {

static constexpr double NS_PER_SEC = 1e9;

RateLimiter::RateLimiter(Config config) : config_(config) {}

// ============================================================================
// check_identity
// ============================================================================
RateDecision RateLimiter::check_identity(const std::string& identity) {
    double capacity    = static_cast<double>(config_.burst_capacity);
    double rpm         = static_cast<double>(config_.requests_per_minute);
    auto bucket = get_or_create(identity, identity_mu_, identity_buckets_,
                                capacity, rpm,
                                config_.max_identity_buckets);
    return consume(bucket);
}

// ============================================================================
// check_ip
// ============================================================================
RateDecision RateLimiter::check_ip(const std::string& ip) {
    if (config_.per_ip_rpm == 0) return RateDecision::ALLOWED;
    double capacity = static_cast<double>(config_.ip_burst);
    double rpm      = static_cast<double>(config_.per_ip_rpm);
    auto bucket     = get_or_create(ip, ip_mu_, ip_buckets_, capacity, rpm,
                                    config_.max_ip_buckets);
    return consume(bucket);
}

// ============================================================================
// cleanup — remove stale buckets
// ============================================================================
void RateLimiter::cleanup(int64_t max_idle_sec) {
    int64_t cutoff = now_ns() - max_idle_sec * static_cast<int64_t>(NS_PER_SEC);

    auto clean = [&](std::shared_mutex& map_mu,
                     std::unordered_map<std::string, std::shared_ptr<Bucket>>& map) {
        std::unique_lock<std::shared_mutex> lk(map_mu);
        for (auto it = map.begin(); it != map.end(); ) {
            bool stale;
            {
                // Check staleness under bucket lock, then release before erase.
                // Destroying a locked mutex is undefined behaviour — never erase
                // while the bucket lock is held.
                std::lock_guard<std::mutex> blk(it->second->mu);
                stale = (it->second->last_ns < cutoff);
            }
            if (stale)
                it = map.erase(it);
            else
                ++it;
        }
    };

    clean(identity_mu_, identity_buckets_);
    clean(ip_mu_, ip_buckets_);
}

// ============================================================================
// identity_bucket_count / ip_bucket_count
// ============================================================================
size_t RateLimiter::identity_bucket_count() const {
    std::shared_lock<std::shared_mutex> lk(identity_mu_);
    return identity_buckets_.size();
}

size_t RateLimiter::ip_bucket_count() const {
    std::shared_lock<std::shared_mutex> lk(ip_mu_);
    return ip_buckets_.size();
}

// ============================================================================
// get_or_create
// ============================================================================
std::shared_ptr<RateLimiter::Bucket> RateLimiter::get_or_create(
    const std::string& key,
    std::shared_mutex& map_mu,
    std::unordered_map<std::string, std::shared_ptr<Bucket>>& map,
    double capacity,
    double rpm,
    size_t max_buckets)
{
    // Fast path: shared lock
    {
        std::shared_lock<std::shared_mutex> lk(map_mu);
        auto it = map.find(key);
        if (it != map.end()) return it->second;
    }

    // Slow path: exclusive lock to create
    std::unique_lock<std::shared_mutex> lk(map_mu);
    auto it = map.find(key);
    if (it != map.end()) return it->second;  // double-check

    // Keep adversarial identity/IP churn memory-bounded. Shared ownership
    // ensures an in-flight consume remains valid if its map entry is evicted.
    if (max_buckets == 0) max_buckets = 1;
    if (map.size() >= max_buckets) {
        auto oldest = map.end();
        int64_t oldest_ns = now_ns();
        size_t sampled = 0;
        // Bound eviction work as well as memory. Scanning all 100k entries for
        // every new adversarial key would turn the safety cap into a CPU DoS.
        for (auto candidate = map.begin(); candidate != map.end() && sampled < 16;
             ++candidate, ++sampled) {
            std::lock_guard<std::mutex> bucket_lock(candidate->second->mu);
            if (oldest == map.end() || candidate->second->last_ns < oldest_ns) {
                oldest_ns = candidate->second->last_ns;
                oldest = candidate;
            }
        }
        if (oldest != map.end()) map.erase(oldest);
    }

    auto bucket = std::make_shared<Bucket>();
    bucket->tokens       = capacity;  // start full
    bucket->last_ns      = now_ns();
    bucket->capacity     = capacity;
    bucket->refill_per_ns = (rpm / 60.0) / NS_PER_SEC;  // tokens per ns

    map.emplace(key, bucket);
    return bucket;
}

// ============================================================================
// consume — atomic refill + consume
// ============================================================================
RateDecision RateLimiter::consume(const std::shared_ptr<Bucket>& bucket) {
    std::lock_guard<std::mutex> lk(bucket->mu);

    int64_t now     = now_ns();
    int64_t elapsed = now - bucket->last_ns;
    if (elapsed < 0) elapsed = 0;

    // Refill tokens
    bucket->tokens = std::min(
        bucket->capacity,
        bucket->tokens + bucket->refill_per_ns * static_cast<double>(elapsed));
    bucket->last_ns = now;

    if (bucket->tokens >= 1.0) {
        bucket->tokens -= 1.0;
        return RateDecision::ALLOWED;
    }
    return RateDecision::RATE_LIMITED;
}

// ============================================================================
// now_ns
// ============================================================================
int64_t RateLimiter::now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
}

} // namespace zeptodb::auth
