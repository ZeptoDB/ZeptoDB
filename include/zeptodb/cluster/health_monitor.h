#pragma once
// ============================================================================
// Phase C-2: HealthMonitor — UDP Heartbeat 기반 노드 상태 관리
// ============================================================================
// 상태 전이:
//   JOINING → ACTIVE (heartbeat 수신)
//   ACTIVE → SUSPECT (suspect_timeout_ms 무응답)
//   SUSPECT → ACTIVE (heartbeat 재개)
//   SUSPECT → DEAD (dead_timeout_ms 추가 무응답)
//   DEAD → (failover 트리거)
//
// UDP heartbeat: 각 노드가 1초마다 브로드캐스트
// 구현: POSIX socket (sendto/recvfrom)
// ============================================================================

#include "zeptodb/cluster/transport.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <shared_mutex>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// NodeState: 노드 상태 열거형
// ============================================================================
enum class NodeState : uint8_t {
    UNKNOWN  = 0,  // 아직 등록 안 됨
    JOINING  = 1,  // 클러스터 참가 중
    ACTIVE   = 2,  // 정상 동작
    SUSPECT  = 3,  // 무응답 (잠정 장애)
    DEAD     = 4,  // 장애 확정 → failover
    LEAVING  = 5,  // 정상 이탈
};

inline const char* node_state_str(NodeState s) {
    switch (s) {
        case NodeState::UNKNOWN:  return "UNKNOWN";
        case NodeState::JOINING:  return "JOINING";
        case NodeState::ACTIVE:   return "ACTIVE";
        case NodeState::SUSPECT:  return "SUSPECT";
        case NodeState::DEAD:     return "DEAD";
        case NodeState::LEAVING:  return "LEAVING";
        default:                  return "???";
    }
}

// ============================================================================
// HealthConfig: 타임아웃 설정
// ============================================================================
struct HealthConfig {
    uint32_t heartbeat_interval_ms = 1000;  // 1초마다 heartbeat 전송
    uint32_t suspect_timeout_ms    = 3000;  // 3초 무응답 → SUSPECT
    uint32_t dead_timeout_ms       = 10000; // 10초 → DEAD
    uint16_t heartbeat_port        = 9100;  // UDP heartbeat 포트
};

// ============================================================================
// HeartbeatPacket: UDP 패킷 포맷 (최소화)
// ============================================================================
#pragma pack(push, 1)
struct HeartbeatPacket {
    uint32_t magic    = 0x41504558;  // 'APEX'
    NodeId   node_id;
    uint64_t seq_num;
    uint64_t timestamp_ns;  // 발신 시각 (nanosecond)
};
#pragma pack(pop)

// ============================================================================
// HealthMonitor: 노드 상태 추적 및 heartbeat 관리
// ============================================================================
class HealthMonitor {
public:
    using StateCallback = std::function<void(NodeId, NodeState /*old*/, NodeState /*new*/)>;
    using Clock         = std::chrono::steady_clock;
    using TimePoint     = Clock::time_point;

    explicit HealthMonitor(const HealthConfig& cfg = {}) : config_(cfg) {}
    ~HealthMonitor() { stop(); }

    // ----------------------------------------------------------------
    // 생명주기
    // ----------------------------------------------------------------

    /// 모니터링 시작 (self: 이 노드 자신, peers: 감시할 노드들)
    void start(const NodeAddress& self, const std::vector<NodeAddress>& peers = {}) {
        if (running_.load()) return;

        self_node_ = self;

        // 감시 노드 등록
        {
            std::unique_lock lock(state_mutex_);
            for (auto& p : peers) {
                add_node_locked(p);
            }
            // 자기 자신은 ACTIVE
            node_states_[self.id] = NodeState::ACTIVE;
            last_heartbeat_[self.id] = Clock::now();
        }

        // UDP 소켓 생성
        setup_socket();

        running_.store(true);

        // heartbeat 송신 스레드
        send_thread_ = std::thread([this]() { send_loop(); });

        // heartbeat 수신 및 상태 체크 스레드
        recv_thread_ = std::thread([this]() { recv_loop(); });
        check_thread_ = std::thread([this]() { check_loop(); });
    }

    /// 모니터링 중지
    void stop() {
        if (!running_.exchange(false)) return;

        send_thread_.join();
        recv_thread_.join();
        check_thread_.join();

        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
    }

    // ----------------------------------------------------------------
    // 노드 관리
    // ----------------------------------------------------------------

    /// 감시 노드 추가 (start 이후에도 동적으로 추가 가능)
    void add_peer(const NodeAddress& peer) {
        std::unique_lock lock(state_mutex_);
        add_node_locked(peer);
    }

    /// 노드 제거 (LEAVING 처리)
    void mark_leaving(NodeId node) {
        transition_state(node, NodeState::LEAVING);
    }

    // ----------------------------------------------------------------
    // 상태 조회
    // ----------------------------------------------------------------

    NodeState get_state(NodeId node) const {
        std::shared_lock lock(state_mutex_);
        auto it = node_states_.find(node);
        return (it != node_states_.end()) ? it->second : NodeState::UNKNOWN;
    }

    std::vector<NodeId> get_active_nodes() const {
        std::shared_lock lock(state_mutex_);
        std::vector<NodeId> result;
        for (auto& [id, state] : node_states_) {
            if (state == NodeState::ACTIVE) {
                result.push_back(id);
            }
        }
        return result;
    }

    std::vector<NodeId> get_all_nodes() const {
        std::shared_lock lock(state_mutex_);
        std::vector<NodeId> result;
        for (auto& [id, _] : node_states_) {
            result.push_back(id);
        }
        return result;
    }

    // ----------------------------------------------------------------
    // 콜백 등록
    // ----------------------------------------------------------------

    void on_state_change(StateCallback cb) {
        std::unique_lock lock(callback_mutex_);
        callbacks_.push_back(std::move(cb));
    }

    // ----------------------------------------------------------------
    // 테스트용: 외부에서 heartbeat 시뮬레이션
    // ----------------------------------------------------------------

    /// 테스트에서 네트워크 없이 heartbeat 주입
    void inject_heartbeat(NodeId node) {
        std::unique_lock lock(state_mutex_);
        last_heartbeat_[node] = Clock::now();

        auto it = node_states_.find(node);
        if (it != node_states_.end()) {
            if (it->second == NodeState::SUSPECT) {
                lock.unlock();
                transition_state(node, NodeState::ACTIVE);
            } else if (it->second == NodeState::JOINING ||
                       it->second == NodeState::UNKNOWN) {
                lock.unlock();
                transition_state(node, NodeState::ACTIVE);
            }
        } else {
            node_states_[node] = NodeState::ACTIVE;
        }
    }

    /// 테스트용: 타임아웃 직접 트리거 (마지막 heartbeat를 과거로 설정)
    void simulate_timeout(NodeId node, uint32_t age_ms) {
        std::unique_lock lock(state_mutex_);
        last_heartbeat_[node] = Clock::now() - std::chrono::milliseconds(age_ms);
    }

    /// 테스트용: 수동으로 상태 체크 트리거 (스레드 없이 사용)
    void check_states_now() {
        check_timeouts();
    }

private:
    // ----------------------------------------------------------------
    // 내부 구현
    // ----------------------------------------------------------------

    void add_node_locked(const NodeAddress& peer) {
        node_addresses_[peer.id] = peer;
        if (!node_states_.count(peer.id)) {
            node_states_[peer.id] = NodeState::JOINING;
            last_heartbeat_[peer.id] = Clock::now();
        }
    }

    void setup_socket() {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) return;

        // SO_REUSEADDR
        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Non-blocking recv (100ms timeout)
        struct timeval tv{0, 100000};
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(config_.heartbeat_port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            // 포트 충돌 — 소켓 닫고 null로 표시 (수신 불가, 송신만 시도)
            close(sock_);
            sock_ = -1;
        }
    }

    void send_loop() {
        uint64_t seq = 0;
        while (running_.load()) {
            send_heartbeat(seq++);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.heartbeat_interval_ms));
        }
    }

    void send_heartbeat(uint64_t seq) {
        if (sock_ < 0) return;  // UDP 소켓 없으면 스킵

        HeartbeatPacket pkt;
        pkt.node_id      = self_node_.id;
        pkt.seq_num      = seq;
        pkt.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        std::shared_lock lock(state_mutex_);
        for (auto& [id, addr] : node_addresses_) {
            if (id == self_node_.id) continue;

            struct sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(config_.heartbeat_port);
            inet_pton(AF_INET, addr.host.c_str(), &dest.sin_addr);

            sendto(sock_, &pkt, sizeof(pkt), 0,
                   reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
        }
    }

    void recv_loop() {
        if (sock_ < 0) {
            // UDP 소켓 없음 — 수신 불가, 루프만 돌며 대기
            while (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return;
        }

        HeartbeatPacket pkt;
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);

        while (running_.load()) {
            ssize_t n = recvfrom(sock_, &pkt, sizeof(pkt), 0,
                                 reinterpret_cast<struct sockaddr*>(&src), &src_len);
            if (n == sizeof(pkt) && pkt.magic == 0x41504558) {
                inject_heartbeat(pkt.node_id);
            }
        }
    }

    void check_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            check_timeouts();
        }
    }

    void check_timeouts() {
        auto now = Clock::now();

        // 상태를 변경할 노드 목록 먼저 수집 (lock 보유 중 콜백 호출 방지)
        struct StateChange { NodeId id; NodeState from; NodeState to; };
        std::vector<StateChange> changes;

        {
            std::shared_lock lock(state_mutex_);
            for (auto& [id, state] : node_states_) {
                if (id == self_node_.id) continue;
                if (state == NodeState::DEAD || state == NodeState::LEAVING) continue;

                auto hb_it = last_heartbeat_.find(id);
                if (hb_it == last_heartbeat_.end()) continue;

                auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - hb_it->second).count();

                if (state == NodeState::ACTIVE || state == NodeState::JOINING) {
                    if (age_ms >= config_.suspect_timeout_ms) {
                        changes.push_back({id, state, NodeState::SUSPECT});
                    }
                } else if (state == NodeState::SUSPECT) {
                    if (age_ms >= config_.dead_timeout_ms) {
                        changes.push_back({id, state, NodeState::DEAD});
                    }
                }
            }
        }

        for (auto& c : changes) {
            transition_state(c.id, c.to);
        }
    }

    void transition_state(NodeId node, NodeState new_state) {
        NodeState old_state;
        {
            std::unique_lock lock(state_mutex_);
            auto it = node_states_.find(node);
            if (it == node_states_.end()) return;
            old_state = it->second;
            if (old_state == new_state) return;
            it->second = new_state;
        }

        // 콜백 호출 (lock 없이)
        std::shared_lock cb_lock(callback_mutex_);
        for (auto& cb : callbacks_) {
            cb(node, old_state, new_state);
        }
    }

    // ----------------------------------------------------------------
    // 데이터 멤버
    // ----------------------------------------------------------------

    HealthConfig                                    config_;
    NodeAddress                                     self_node_;
    std::atomic<bool>                               running_{false};

    mutable std::shared_mutex                       state_mutex_;
    std::unordered_map<NodeId, NodeState>           node_states_;
    std::unordered_map<NodeId, TimePoint>           last_heartbeat_;
    std::unordered_map<NodeId, NodeAddress>         node_addresses_;

    mutable std::shared_mutex                       callback_mutex_;
    std::vector<StateCallback>                      callbacks_;

    std::thread                                     send_thread_;
    std::thread                                     recv_thread_;
    std::thread                                     check_thread_;

    int                                             sock_ = -1;
};

} // namespace zeptodb::cluster
