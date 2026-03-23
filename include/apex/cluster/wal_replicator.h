#pragma once
// ============================================================================
// WAL Replicator — async WAL replication to replica node(s)
// ============================================================================
// Primary node pushes TickMessages into the replicator's queue.
// A background thread batches them and sends via TcpRpcClient::replicate_wal().
//
// Usage:
//   WalReplicator repl;
//   repl.add_replica("10.0.0.2", 19801);
//   repl.start();
//   repl.enqueue(tick);          // called from ingest hot path
//   ...
//   repl.stop();                 // flushes remaining queue
// ============================================================================

#include "apex/cluster/tcp_rpc.h"
#include "apex/ingestion/tick_plant.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apex::cluster {

struct ReplicatorConfig {
    size_t   batch_size     = 256;    // max ticks per WAL_REPLICATE message
    uint32_t flush_interval_ms = 10;  // max delay before sending partial batch
    size_t   queue_capacity = 1 << 20; // 1M entries ring buffer
    bool     sync_mode      = false;  // if true, enqueue blocks until replica ACK
    uint32_t sync_timeout_ms = 1000;  // max wait for sync ACK
};

struct ReplicatorStats {
    std::atomic<uint64_t> enqueued{0};
    std::atomic<uint64_t> replicated{0};
    std::atomic<uint64_t> dropped{0};     // queue full
    std::atomic<uint64_t> send_errors{0};
    std::atomic<uint64_t> sync_acked{0};  // batches confirmed by replica
};

class WalReplicator {
public:
    explicit WalReplicator(ReplicatorConfig cfg = {}) : config_(cfg) {
        queue_.reserve(cfg.queue_capacity);
    }
    ~WalReplicator() { stop(); }

    WalReplicator(const WalReplicator&)            = delete;
    WalReplicator& operator=(const WalReplicator&) = delete;

    /// Add a replica endpoint. Call before start().
    void add_replica(const std::string& host, uint16_t port);

    void start();
    void stop();

    /// Enqueue a tick for replication. Lock-free-ish (mutex on vector swap).
    /// Returns false if queue is full (tick dropped).
    bool enqueue(const apex::ingestion::TickMessage& msg);

    bool is_running() const { return running_.load(); }
    const ReplicatorStats& stats() const { return stats_; }

    /// Number of replica endpoints.
    size_t replica_count() const { return replicas_.size(); }

private:
    void send_loop();
    void flush_batch(std::vector<apex::ingestion::TickMessage>& batch);

    ReplicatorConfig config_;
    ReplicatorStats  stats_;

    std::vector<std::unique_ptr<TcpRpcClient>> replicas_;

    // Double-buffer queue: producers write to queue_, sender swaps out
    std::mutex                                   mu_;
    std::condition_variable                      cv_;
    std::vector<apex::ingestion::TickMessage>    queue_;

    // Sync replication: producers wait on this after enqueue
    std::mutex                                   sync_mu_;
    std::condition_variable                      sync_cv_;
    std::atomic<uint64_t>                        flush_epoch_{0};

    std::atomic<bool> running_{false};
    std::thread       send_thread_;
};

} // namespace apex::cluster
