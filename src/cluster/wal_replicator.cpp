#include "zeptodb/cluster/wal_replicator.h"
#include <chrono>

namespace zeptodb::cluster {

void WalReplicator::add_replica(const std::string& host, uint16_t port) {
    replicas_.push_back(std::make_unique<TcpRpcClient>(host, port));
}

void WalReplicator::start() {
    if (running_.exchange(true)) return;
    send_thread_ = std::thread([this]() { send_loop(); });
}

void WalReplicator::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (send_thread_.joinable()) send_thread_.join();

    // Flush remaining
    std::vector<zeptodb::ingestion::TickMessage> remaining;
    {
        std::lock_guard<std::mutex> lock(mu_);
        remaining.swap(queue_);
    }
    if (!remaining.empty()) flush_batch(remaining);
}

bool WalReplicator::enqueue(const zeptodb::ingestion::TickMessage& msg) {
    uint64_t epoch_before;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.size() >= config_.queue_capacity) {
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(msg);
        stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
        epoch_before = flush_epoch_.load(std::memory_order_relaxed);
    }
    if (queue_.size() >= config_.batch_size)
        cv_.notify_one();

    // Sync mode: wait until this batch is flushed and ACKed
    if (config_.sync_mode) {
        std::unique_lock<std::mutex> slock(sync_mu_);
        sync_cv_.wait_for(slock,
            std::chrono::milliseconds(config_.sync_timeout_ms),
            [&]() { return flush_epoch_.load(std::memory_order_acquire) > epoch_before; });
    }
    return true;
}

void WalReplicator::send_loop() {
    std::vector<zeptodb::ingestion::TickMessage> batch;
    batch.reserve(config_.batch_size);

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock,
                         std::chrono::milliseconds(config_.flush_interval_ms),
                         [this]() {
                             return !running_.load() ||
                                    queue_.size() >= config_.batch_size;
                         });
            if (queue_.empty()) continue;
            batch.swap(queue_);
            queue_.reserve(config_.queue_capacity);
        }
        flush_batch(batch);
        batch.clear();
    }
}

void WalReplicator::flush_batch(
    std::vector<zeptodb::ingestion::TickMessage>& batch)
{
    bool all_ok = true;
    for (auto& replica : replicas_) {
        if (replica->replicate_wal(batch)) {
            stats_.replicated.fetch_add(batch.size(),
                                        std::memory_order_relaxed);
        } else {
            stats_.send_errors.fetch_add(1, std::memory_order_relaxed);
            all_ok = false;
        }
    }

    if (all_ok)
        stats_.sync_acked.fetch_add(1, std::memory_order_relaxed);

    // Signal sync waiters
    flush_epoch_.fetch_add(1, std::memory_order_release);
    sync_cv_.notify_all();
}

} // namespace zeptodb::cluster
