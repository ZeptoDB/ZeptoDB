#pragma once
// ============================================================================
// Phase C-2: ClusterNode — 분산 클러스터 노드 (컴파일 타임 Transport 선택)
// ============================================================================
// Transport 파라미터로 컴파일 타임 다형성:
//   ClusterNode<ShmTransport>   → 로컬 테스트
//   ClusterNode<UcxTransport>   → RDMA 프로덕션
//
// 책임:
//   1. 클러스터 참가/이탈 (HealthMonitor + PartitionRouter 연동)
//   2. 분산 ingest: PartitionRouter로 담당 노드 결정 → local or remote
//   3. 분산 쿼리: 담당 노드에서 로컬 쿼리 실행 (scatter-gather 기초)
//   4. 로컬 파이프라인 (ZeptoPipeline) 관리
// ============================================================================

#include "zeptodb/cluster/transport.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/health_monitor.h"
#include "zeptodb/cluster/query_coordinator.h"
#include "zeptodb/cluster/ring_consensus.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/ptp_clock_detector.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/server/metrics_collector.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/common/logger.h"
#include "zeptodb/auth/license_validator.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

using zeptodb::core::ZeptoPipeline;
using zeptodb::core::QueryResult;
using zeptodb::core::PipelineConfig;
using zeptodb::ingestion::TickMessage;

// ============================================================================
// ClusterConfig: 클러스터 노드 설정
// ============================================================================
struct ClusterConfig {
    NodeAddress       self;              // 이 노드의 주소/ID
    HealthConfig      health;            // heartbeat 설정
    PipelineConfig    pipeline;          // 로컬 파이프라인 설정
    bool              enable_remote_ingest = true;  // 원격 인제스트 활성화
    uint16_t          rpc_port = 0;      // TCP RPC port (0 = self.port + 100)
    bool              is_coordinator = false;  // true = ring 변경 권한 보유
    RpcSecurityConfig rpc_security;      // 내부 RPC 보안 (설정 시 자동 전파)
    PtpConfig         ptp;               // PTP clock sync detection config
};

// ============================================================================
// ClusterNode<Transport>: 분산 노드 메인 클래스
// ============================================================================
template <typename Transport>
class ClusterNode {
public:
    explicit ClusterNode(const ClusterConfig& cfg = {})
        : config_(cfg)
        , health_(cfg.health)
        , local_pipeline_(cfg.pipeline)
        , ptp_detector_(cfg.ptp)
    {}

    ~ClusterNode() { leave_cluster(); }

    // Non-copyable
    ClusterNode(const ClusterNode&) = delete;
    ClusterNode& operator=(const ClusterNode&) = delete;

    // ----------------------------------------------------------------
    // 클러스터 참가/이탈
    // ----------------------------------------------------------------

    /// 클러스터 참가
    /// seeds: 이미 클러스터에 있는 노드 주소 목록
    void join_cluster(const std::vector<NodeAddress>& seeds = {}) {
        if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::CLUSTER)) {
            throw std::runtime_error("Multi-node cluster requires Enterprise license");
        }
        if (joined_.load()) return;

        // 1. Transport 초기화
        transport_.init(config_.self);

        // 2. PartitionRouter에 자신 등록
        {
            std::unique_lock lock(router_mutex_);
            router_.add_node(config_.self.id);
            for (auto& seed : seeds) {
                router_.add_node(seed.id);
            }
        }

        // 3. Seed 노드에 연결 + RPC client 생성
        size_t seed_connected = 0;
        for (auto& seed : seeds) {
            try {
                ConnectionId conn = transport_.connect(seed);
                peer_connections_[seed.id] = conn;
                peer_addresses_[seed.id]   = seed;

                uint16_t peer_rpc_port = static_cast<uint16_t>(seed.port + 100);
                auto rpc = std::make_unique<TcpRpcClient>(
                    seed.host, peer_rpc_port, 1000);
                if (config_.rpc_security.enabled) {
                    rpc->set_security(config_.rpc_security);
                }
                {
                    std::unique_lock lock(peer_rpc_mutex_);
                    peer_rpc_clients_[seed.id] = std::move(rpc);
                }
                ++seed_connected;
            } catch (...) {
                // 연결 실패 시 무시 (나중에 재시도)
            }
        }

        // Require at least 1 seed connection (unless no seeds given = bootstrap)
        if (!seeds.empty() && seed_connected == 0) {
            for (auto& seed : seeds)
                router_.remove_node(seed.id);
            peer_addresses_.clear();
            peer_connections_.clear();
            transport_.shutdown();
            throw std::runtime_error(
                "ClusterNode: failed to connect to any seed node ("
                + std::to_string(seeds.size()) + " seeds tried)");
        }

        // 4. Health Monitor 시작
        health_.start(config_.self, seeds);
        health_.on_state_change([this](NodeId id, NodeState old_s, NodeState new_s) {
            on_node_state_change(id, old_s, new_s);
        });

        // 5. 로컬 파이프라인 시작
        local_pipeline_.start();

        // 6. TCP RPC 서버 시작 (원격 SQL 쿼리 + Tick ingest 수신)
        uint16_t rport = (config_.rpc_port > 0)
                       ? config_.rpc_port
                       : static_cast<uint16_t>(config_.self.port + 100);

        // RPC 보안 자동 적용
        if (config_.rpc_security.enabled) {
            rpc_server_.set_security(config_.rpc_security);
        }

        rpc_server_.start(
            rport,
            [this](const std::string& sql) { return execute_sql_local(sql); },
            [this](const zeptodb::ingestion::TickMessage& msg) {
                return local_pipeline_.ingest_tick(msg);
            });

        // Register stats/metrics callbacks for remote collection
        rpc_server_.set_stats_callback([this]() {
            const auto& s = local_pipeline_.stats();
            return std::string("{\"node_id\":") + std::to_string(config_.self.id)
                + ",\"ticks_ingested\":" + std::to_string(s.ticks_ingested.load())
                + ",\"ticks_stored\":" + std::to_string(s.ticks_stored.load())
                + ",\"state\":\"ACTIVE\"}";
        });

        rpc_server_.set_metrics_callback(
            [this](int64_t since_ms, uint32_t limit) -> std::string {
                // If this node has a MetricsCollector, use it; otherwise build
                // a single-snapshot response from current PipelineStats.
                if (metrics_collector_) {
                    auto snaps = metrics_collector_->get_history(since_ms,
                                     limit > 0 ? limit : 0);
                    return zeptodb::server::MetricsCollector::to_json(snaps);
                }
                // Fallback: single snapshot from live stats
                const auto& s = local_pipeline_.stats();
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                return std::string("[{\"timestamp_ms\":") + std::to_string(now)
                    + ",\"node_id\":" + std::to_string(config_.self.id)
                    + ",\"ticks_ingested\":" + std::to_string(s.ticks_ingested.load())
                    + ",\"ticks_stored\":" + std::to_string(s.ticks_stored.load())
                    + ",\"ticks_dropped\":" + std::to_string(s.ticks_dropped.load())
                    + ",\"queries_executed\":" + std::to_string(s.queries_executed.load())
                    + ",\"total_rows_scanned\":" + std::to_string(s.total_rows_scanned.load())
                    + ",\"partitions_created\":" + std::to_string(s.partitions_created.load())
                    + ",\"last_ingest_latency_ns\":" + std::to_string(s.last_ingest_latency_ns.load())
                    + "}]";
            });

        rpc_port_ = rport;

        // 7. RingConsensus 초기화 (외부에서 set_consensus()로 주입하지 않은 경우 기본 EpochBroadcast)
        if (!consensus_) {
            consensus_ = std::make_unique<EpochBroadcastConsensus>(
                router_, router_mutex_, fencing_token_);
        }

        // Follower: RPC 서버에 ring update 콜백 등록
        rpc_server_.set_ring_update_callback(
            [this](const uint8_t* data, size_t len) {
                return consensus_->apply_update(data, len);
            });

        // Coordinator: peer RPC 주소를 consensus에 등록
        if (config_.is_coordinator) {
            auto* ebc = dynamic_cast<EpochBroadcastConsensus*>(consensus_.get());
            if (ebc) {
                for (auto& [id, addr] : peer_addresses_) {
                    uint16_t peer_rpc = static_cast<uint16_t>(addr.port + 100);
                    ebc->add_peer(id, addr.host, peer_rpc);
                }
            }
        }

        joined_.store(true);
    }

    /// 클러스터 이탈 (graceful shutdown)
    void leave_cluster() {
        if (!joined_.exchange(false)) return;

        // TCP RPC 서버 중지
        rpc_server_.stop();

        // Health Monitor에 이탈 알림
        health_.mark_leaving(config_.self.id);
        health_.stop();

        // 로컬 파이프라인 중지
        local_pipeline_.stop();

        // Transport 종료
        for (auto& [id, conn] : peer_connections_) {
            transport_.disconnect(conn);
        }
        peer_connections_.clear();
        {
            std::unique_lock lock(peer_rpc_mutex_);
            peer_rpc_clients_.clear();
        }
        transport_.shutdown();
    }

    // ----------------------------------------------------------------
    // 분산 Ingest
    // ----------------------------------------------------------------

    /// 틱 데이터 수신 → 담당 노드로 라우팅
    /// During partition migration, dual-writes to both source and destination
    /// nodes to prevent data loss.
    /// @return true if ingested (local or remote), false if failed
    bool ingest_tick(TickMessage msg) {
        // Check migration_target FIRST (uses cache_mutex_, not router_mutex_)
        auto mig = router_.migration_target(msg.symbol_id);
        if (mig) {
            // Dual-write: send to both from and to nodes
            auto [from, to] = *mig;
            bool ok_from = send_to_node(from, msg);
            bool ok_to   = send_to_node(to, msg);
            if (ok_from != ok_to) {
                ZEPTO_WARN("Dual-write partial failure: symbol={}, from_ok={}, to_ok={}",
                           msg.symbol_id, ok_from, ok_to);
            }
            return ok_from || ok_to;
        }

        // Normal path: single route
        NodeId owner = route(msg.symbol_id);

        if (owner == config_.self.id) {
            return local_pipeline_.ingest_tick(msg);
        } else if (config_.enable_remote_ingest) {
            return remote_ingest(owner, msg);
        }
        return false;
    }

    /// 로컬 파이프라인에 직접 ingest (라우팅 없이)
    bool ingest_local(TickMessage msg) {
        return local_pipeline_.ingest_tick(msg);
    }

    // ----------------------------------------------------------------
    // 분산 쿼리
    // ----------------------------------------------------------------

    /// VWAP 쿼리 → 담당 노드에서 실행
    QueryResult query_vwap(SymbolId symbol,
                           Timestamp from = 0,
                           Timestamp to = INT64_MAX) {
        NodeId owner = route(symbol);

        if (owner == config_.self.id) {
            // 로컬 쿼리
            return local_pipeline_.query_vwap(symbol, from, to);
        } else {
            // 원격 쿼리 (현재는 에러 반환, 실제 구현에서 gRPC 추가)
            return remote_query_vwap(owner, symbol, from, to);
        }
    }

    /// 로컬 파이프라인 직접 쿼리
    QueryResult query_local_vwap(SymbolId symbol,
                                  Timestamp from = 0,
                                  Timestamp to = INT64_MAX) {
        return local_pipeline_.query_vwap(symbol, from, to);
    }

    // ----------------------------------------------------------------
    // 상태 조회
    // ----------------------------------------------------------------

    NodeId self_id() const { return config_.self.id; }

    bool     is_joined() const { return joined_.load(); }
    uint16_t rpc_port()  const { return rpc_port_; }

    // ----------------------------------------------------------------
    // SQL 실행 (로컬 파이프라인)
    // ----------------------------------------------------------------

    /// Execute a SQL query against the local pipeline (in-process).
    zeptodb::sql::QueryResultSet execute_sql_local(const std::string& sql) {
        zeptodb::sql::QueryExecutor ex(local_pipeline_);
        return ex.execute(sql);
    }

    NodeId route(SymbolId symbol) const {
        std::shared_lock lock(router_mutex_);
        return router_.route(symbol);
    }

    PartitionRouter& router() { return router_; }
    const PartitionRouter& router() const { return router_; }

    /// Share this node's PartitionRouter with a QueryCoordinator so both
    /// use the same routing table.  Call before join_cluster().
    void connect_coordinator(QueryCoordinator& coord) {
        coord.set_shared_router(&router_, &router_mutex_);
    }

    HealthMonitor& health() { return health_; }
    ZeptoPipeline& pipeline() { return local_pipeline_; }
    PtpClockDetector& ptp_detector() { return ptp_detector_; }
    const PtpClockDetector& ptp_detector() const { return ptp_detector_; }

    /// Access the RingConsensus (nullptr if not initialized)
    RingConsensus* consensus() { return consensus_.get(); }

    /// Set a custom RingConsensus implementation (e.g. future RaftConsensus).
    /// Must be called before join_cluster().
    void set_consensus(std::unique_ptr<RingConsensus> c) { consensus_ = std::move(c); }

    // ----------------------------------------------------------------
    // 원격 메모리 노출 (RDMA one-sided)
    // ----------------------------------------------------------------

    /// 로컬 메모리 영역을 클러스터에 노출 (다른 노드가 직접 읽기/쓰기 가능)
    RemoteRegion expose_memory(void* addr, size_t size) {
        return transport_.register_memory(addr, size);
    }

    void unexpose_memory(RemoteRegion& region) {
        transport_.deregister_memory(region);
    }

    /// 원격 노드의 메모리에 직접 쓰기
    void write_to_remote(NodeId target, const RemoteRegion& region,
                         const void* data, size_t offset, size_t size) {
        (void)target;
        transport_.remote_write(data, region, offset, size);
    }

    /// 원격 노드의 메모리에서 직접 읽기
    void read_from_remote(NodeId target, const RemoteRegion& region,
                          size_t offset, void* dst, size_t size) {
        (void)target;
        transport_.remote_read(region, offset, dst, size);
    }

    Transport& transport() { return transport_; }

private:
    // ----------------------------------------------------------------
    // 내부 구현
    // ----------------------------------------------------------------

    /// Send tick to a specific node (local or remote).
    bool send_to_node(NodeId target, const TickMessage& msg) {
        if (target == config_.self.id)
            return local_pipeline_.ingest_tick(msg);
        if (config_.enable_remote_ingest)
            return remote_ingest(target, msg);
        return false;
    }

    /// 원격 노드로 틱 전송 — TCP RPC TICK_INGEST
    bool remote_ingest(NodeId target, const TickMessage& msg) {
        TcpRpcClient* client = nullptr;
        {
            std::shared_lock lock(peer_rpc_mutex_);
            auto it = peer_rpc_clients_.find(target);
            if (it != peer_rpc_clients_.end()) {
                client = it->second.get();
            }
        }

        if (!client) {
            // Peer not yet known; try to create client lazily
            auto addr_it = peer_addresses_.find(target);
            if (addr_it == peer_addresses_.end()) return false;

            uint16_t peer_rpc_port = static_cast<uint16_t>(addr_it->second.port + 100);
            auto rpc = std::make_unique<TcpRpcClient>(
                addr_it->second.host, peer_rpc_port, 2000);
            if (config_.rpc_security.enabled) {
                rpc->set_security(config_.rpc_security);
            }
            client = rpc.get();
            {
                std::unique_lock lock(peer_rpc_mutex_);
                auto [ins, inserted] = peer_rpc_clients_.emplace(target, std::move(rpc));
                if (!inserted) client = ins->second.get();  // another thread beat us
            }
        }
        return client->ingest_tick(msg);
    }

    /// 원격 VWAP 쿼리 (stub — 실제는 gRPC)
    QueryResult remote_query_vwap(NodeId /*target*/, SymbolId /*symbol*/,
                                   Timestamp /*from*/, Timestamp /*to*/) {
        QueryResult r;
        r.type  = QueryResult::Type::ERROR;
        r.value = 0.0;
        return r;
    }

    /// 노드 상태 변경 콜백
    void on_node_state_change(NodeId id, NodeState old_s, NodeState new_s) {
        if (new_s == NodeState::DEAD) {
            if (config_.is_coordinator && consensus_) {
                consensus_->propose_remove(id);
            } else {
                std::unique_lock lock(router_mutex_);
                router_.remove_node(id);
            }
        } else if (new_s == NodeState::ACTIVE &&
                   (old_s == NodeState::JOINING || old_s == NodeState::REJOINING)) {
            if (config_.is_coordinator && consensus_) {
                // Register peer in consensus for future broadcasts
                auto* ebc = dynamic_cast<EpochBroadcastConsensus*>(consensus_.get());
                if (ebc) {
                    auto addr_it = peer_addresses_.find(id);
                    if (addr_it != peer_addresses_.end()) {
                        uint16_t peer_rpc = static_cast<uint16_t>(addr_it->second.port + 100);
                        ebc->add_peer(id, addr_it->second.host, peer_rpc);
                    }
                }
                consensus_->propose_add(id);
            } else {
                std::unique_lock lock(router_mutex_);
                router_.add_node(id);
            }
        }
    }

    // ----------------------------------------------------------------
    // 데이터 멤버
    // ----------------------------------------------------------------

    ClusterConfig                              config_;
    Transport                                  transport_;
    mutable std::shared_mutex                  router_mutex_;
    PartitionRouter                            router_;
    HealthMonitor                              health_;
    ZeptoPipeline                               local_pipeline_;
    PtpClockDetector                           ptp_detector_;
    TcpRpcServer                               rpc_server_;
    uint16_t                                   rpc_port_ = 0;
    FencingToken                               fencing_token_;
    std::unique_ptr<RingConsensus>             consensus_;

    std::atomic<bool>                          joined_{false};

    // 피어 연결 정보
    std::unordered_map<NodeId, ConnectionId>              peer_connections_;
    std::unordered_map<NodeId, NodeAddress>               peer_addresses_;

    // TCP RPC clients for remote tick ingest (pre-created per seed in join_cluster)
    mutable std::shared_mutex peer_rpc_mutex_;
    std::unordered_map<NodeId, std::unique_ptr<TcpRpcClient>> peer_rpc_clients_;

    // 원격 ingest용 RDMA 영역 (target NodeId → RemoteRegion, future UCX path)
    std::unordered_map<NodeId, RemoteRegion>              remote_ingest_regions_;

    // Optional: MetricsCollector for this node (set externally)
    zeptodb::server::MetricsCollector*                    metrics_collector_ = nullptr;

public:
    /// Attach a MetricsCollector so METRICS_REQUEST returns history.
    void set_metrics_collector(zeptodb::server::MetricsCollector* mc) {
        metrics_collector_ = mc;
    }
};

// ----------------------------------------------------------------
// 편의 타입 별칭
// ----------------------------------------------------------------

// SharedMem 기반 (테스트용)
#include "zeptodb/cluster/transport.h"

} // namespace zeptodb::cluster
