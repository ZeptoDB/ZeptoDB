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

#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zeptodb::cluster {

struct ReplicatorConfig {
    size_t   batch_size     = 256;    // max ticks per WAL_REPLICATE message
    uint32_t flush_interval_ms = 10;  // max delay before sending partial batch
    size_t   queue_capacity = 1 << 20; // 1M entries ring buffer
    bool     sync_mode      = false;  // if true, enqueue blocks until replica ACK
    uint32_t sync_timeout_ms = 1000;  // max wait for sync ACK

    // Quorum write: 최소 W개 replica ACK 필요 (0 = best-effort, 모든 replica 시도)
    size_t   quorum_w       = 0;

    // 재시도: send 실패 시 retry_queue에 보관 후 재시도
    size_t   retry_queue_capacity = 1 << 16;  // 64K batches
    uint32_t retry_interval_ms    = 100;       // 재시도 간격
    uint32_t max_retries          = 3;         // 최대 재시도 횟수

    // 백프레셔: queue 가득 차면 producer block (drop 대신)
    bool     backpressure    = false;
    uint32_t backpressure_timeout_ms = 500;  // block 최대 대기 시간
};

struct ReplicatorStats {
    std::atomic<uint64_t> enqueued{0};
    std::atomic<uint64_t> replicated{0};
    std::atomic<uint64_t> dropped{0};     // queue full (backpressure=false only)
    std::atomic<uint64_t> send_errors{0};
    std::atomic<uint64_t> sync_acked{0};  // batches confirmed by replica
    std::atomic<uint64_t> retried{0};     // batches retried
    std::atomic<uint64_t> retry_exhausted{0}; // batches dropped after max retries
    std::atomic<uint64_t> backpressured{0};   // enqueue calls that blocked
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
    bool enqueue(const zeptodb::ingestion::TickMessage& msg);

    bool is_running() const { return running_.load(); }
    const ReplicatorStats& stats() const { return stats_; }

    /// Number of replica endpoints.
    size_t replica_count() const { return replicas_.size(); }

private:
    void send_loop();
    void flush_batch(std::vector<zeptodb::ingestion::TickMessage>& batch);
    void process_retry_queue();

    struct RetryEntry {
        std::vector<zeptodb::ingestion::TickMessage> batch;
        uint32_t attempts = 0;
    };

    ReplicatorConfig config_;
    ReplicatorStats  stats_;

    std::vector<std::unique_ptr<TcpRpcClient>> replicas_;

    // Double-buffer queue: producers write to queue_, sender swaps out
    std::mutex                                   mu_;
    std::condition_variable                      cv_;
    std::vector<zeptodb::ingestion::TickMessage>    queue_;

    // Sync replication: producers wait on this after enqueue
    std::mutex                                   sync_mu_;
    std::condition_variable                      sync_cv_;
    std::atomic<uint64_t>                        flush_epoch_{0};

    // Retry queue: failed batches awaiting retry
    std::mutex                                   retry_mu_;
    std::vector<RetryEntry>                      retry_queue_;

    std::atomic<bool> running_{false};
    std::thread       send_thread_;
};

} // namespace zeptodb::cluster
