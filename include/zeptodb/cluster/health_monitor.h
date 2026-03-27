#pragma once
// ============================================================================
// Phase C-2: HealthMonitor — UDP Heartbeat 기반 노드 상태 관리
// ============================================================================
// 상태 전이:
//   JOINING → ACTIVE (heartbeat 수신)
//   ACTIVE → SUSPECT (suspect_timeout_ms 무응답, consecutive miss 확인)
//   SUSPECT → ACTIVE (heartbeat 재개)
//   SUSPECT → DEAD (dead_timeout_ms 추가 무응답 + TCP probe 실패)
//   DEAD → REJOINING (heartbeat 재수신 — rejoin protocol)
//   REJOINING → ACTIVE (재동기화 완료)
//   DEAD → (failover 트리거)
//
// P8-High 개선:
//   1. DEAD 복구: DEAD 노드가 heartbeat 재개 시 REJOINING 상태를 거쳐
//      데이터 재동기화 후 ACTIVE로 복귀. on_rejoin() 콜백으로 제어.
//   2. UDP 내결함성:
//      - consecutive_misses_for_suspect (기본 3) 연속 miss 후에만 SUSPECT
//      - bind 실패 시 fatal error (fatal_on_bind_failure)
//      - SUSPECT→DEAD 전이 전 보조 TCP heartbeat probe
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
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// NodeState: 노드 상태 열거형
// ============================================================================
enum class NodeState : uint8_t {
    UNKNOWN   = 0,  // 아직 등록 안 됨
    JOINING   = 1,  // 클러스터 참가 중
    ACTIVE    = 2,  // 정상 동작
    SUSPECT   = 3,  // 무응답 (잠정 장애)
    DEAD      = 4,  // 장애 확정 → failover
    LEAVING   = 5,  // 정상 이탈
    REJOINING = 6,  // DEAD에서 복구 중 (데이터 재동기화 대기)
};

inline const char* node_state_str(NodeState s) {
    switch (s) {
        case NodeState::UNKNOWN:   return "UNKNOWN";
        case NodeState::JOINING:   return "JOINING";
        case NodeState::ACTIVE:    return "ACTIVE";
        case NodeState::SUSPECT:   return "SUSPECT";
        case NodeState::DEAD:      return "DEAD";
        case NodeState::LEAVING:   return "LEAVING";
        case NodeState::REJOINING: return "REJOINING";
        default:                   return "???";
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
    uint16_t tcp_heartbeat_port    = 9101;  // TCP heartbeat 보조 포트

    // UDP 내결함성: SUSPECT 전이 전 연속 miss 횟수 요구
    uint32_t consecutive_misses_for_suspect = 3;

    // DEAD 복구: rejoin 활성화
    bool     enable_dead_rejoin = true;

    // bind 실패 시 fatal error (true = 예외 발생)
    bool     fatal_on_bind_failure = true;
};

// ============================================================================
// HeartbeatPacket: UDP 패킷 포맷 (최소화)
// ============================================================================
#pragma pack(push, 1)
struct HeartbeatPacket {
    uint32_t magic    = 0x41504558;  // 'ZEPTO' (legacy wire compat: 0x41504558)
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
    using StateCallback  = std::function<void(NodeId, NodeState /*old*/, NodeState /*new*/)>;
    using RejoinCallback = std::function<bool(NodeId)>;  // true = 재동기화 성공
    using Clock          = std::chrono::steady_clock;
    using TimePoint      = Clock::time_point;

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
            consecutive_misses_[self.id] = 0;
        }

        // UDP 소켓 생성
        setup_udp_socket();

        // TCP heartbeat 소켓 생성
        setup_tcp_socket();

        running_.store(true);

        send_thread_     = std::thread([this]() { send_loop(); });
        recv_thread_     = std::thread([this]() { recv_loop(); });
        check_thread_    = std::thread([this]() { check_loop(); });
        tcp_recv_thread_ = std::thread([this]() { tcp_recv_loop(); });
    }

    /// 모니터링 중지
    void stop() {
        if (!running_.exchange(false)) return;

        if (send_thread_.joinable())     send_thread_.join();
        if (recv_thread_.joinable())     recv_thread_.join();
        if (check_thread_.joinable())    check_thread_.join();
        if (tcp_recv_thread_.joinable()) tcp_recv_thread_.join();

        if (udp_sock_ >= 0) { close(udp_sock_); udp_sock_ = -1; }
        if (tcp_sock_ >= 0) { close(tcp_sock_); tcp_sock_ = -1; }
    }

    // ----------------------------------------------------------------
    // 노드 관리
    // ----------------------------------------------------------------

    void add_peer(const NodeAddress& peer) {
        std::unique_lock lock(state_mutex_);
        add_node_locked(peer);
    }

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
            if (state == NodeState::ACTIVE) result.push_back(id);
        }
        return result;
    }

    std::vector<NodeId> get_all_nodes() const {
        std::shared_lock lock(state_mutex_);
        std::vector<NodeId> result;
        for (auto& [id, _] : node_states_) result.push_back(id);
        return result;
    }

    // ----------------------------------------------------------------
    // 콜백 등록
    // ----------------------------------------------------------------

    void on_state_change(StateCallback cb) {
        std::unique_lock lock(callback_mutex_);
        callbacks_.push_back(std::move(cb));
    }

    /// DEAD → REJOINING 전이 시 호출. true 반환 → REJOINING → ACTIVE.
    void on_rejoin(RejoinCallback cb) {
        std::unique_lock lock(callback_mutex_);
        rejoin_callback_ = std::move(cb);
    }

    // ----------------------------------------------------------------
    // 테스트용
    // ----------------------------------------------------------------

    void inject_heartbeat(NodeId node) {
        std::unique_lock lock(state_mutex_);
        last_heartbeat_[node] = Clock::now();
        consecutive_misses_[node] = 0;

        auto it = node_states_.find(node);
        if (it != node_states_.end()) {
            NodeState cur = it->second;
            if (cur == NodeState::SUSPECT) {
                lock.unlock();
                transition_state(node, NodeState::ACTIVE);
            } else if (cur == NodeState::DEAD) {
                if (config_.enable_dead_rejoin) {
                    lock.unlock();
                    handle_dead_rejoin(node);
                }
            } else if (cur == NodeState::REJOINING) {
                // 이미 rejoin 진행 중 — heartbeat 타임스탬프만 갱신 (위에서 완료)
            } else if (cur == NodeState::JOINING || cur == NodeState::UNKNOWN) {
                lock.unlock();
                transition_state(node, NodeState::ACTIVE);
            }
        } else {
            node_states_[node] = NodeState::ACTIVE;
            consecutive_misses_[node] = 0;
        }
    }

    void simulate_timeout(NodeId node, uint32_t age_ms) {
        std::unique_lock lock(state_mutex_);
        last_heartbeat_[node] = Clock::now() - std::chrono::milliseconds(age_ms);
    }

    /// 테스트용: 연속 miss 카운트 직접 설정
    void simulate_consecutive_misses(NodeId node, uint32_t count) {
        std::unique_lock lock(state_mutex_);
        consecutive_misses_[node] = count;
    }

    void check_states_now() {
        check_timeouts();
    }

    uint32_t get_consecutive_misses(NodeId node) const {
        std::shared_lock lock(state_mutex_);
        auto it = consecutive_misses_.find(node);
        return (it != consecutive_misses_.end()) ? it->second : 0;
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
            consecutive_misses_[peer.id] = 0;
        }
    }

    void setup_udp_socket() {
        udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock_ < 0) {
            if (config_.fatal_on_bind_failure)
                throw std::runtime_error("HealthMonitor: failed to create UDP socket");
            return;
        }

        int opt = 1;
        setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct timeval tv{0, 100000};
        setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(config_.heartbeat_port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udp_sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(udp_sock_);
            udp_sock_ = -1;
            if (config_.fatal_on_bind_failure)
                throw std::runtime_error(
                    "HealthMonitor: UDP bind failed on port " +
                    std::to_string(config_.heartbeat_port));
        }
    }

    void setup_tcp_socket() {
        tcp_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_sock_ < 0) return;

        int opt = 1;
        setsockopt(tcp_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(config_.tcp_heartbeat_port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(tcp_sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(tcp_sock_);
            tcp_sock_ = -1;
            return;
        }
        listen(tcp_sock_, 16);

        struct timeval tv{0, 200000};
        setsockopt(tcp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
        if (udp_sock_ < 0) return;

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

            sendto(udp_sock_, &pkt, sizeof(pkt), 0,
                   reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
        }
    }

    void recv_loop() {
        if (udp_sock_ < 0) {
            while (running_.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }

        HeartbeatPacket pkt;
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);

        while (running_.load()) {
            ssize_t n = recvfrom(udp_sock_, &pkt, sizeof(pkt), 0,
                                 reinterpret_cast<struct sockaddr*>(&src), &src_len);
            if (n == sizeof(pkt) && pkt.magic == 0x41504558) {
                inject_heartbeat(pkt.node_id);
            }
        }
    }

    /// TCP heartbeat 수신 루프 — UDP 실패 시 보조 경로
    void tcp_recv_loop() {
        if (tcp_sock_ < 0) {
            while (running_.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }

        while (running_.load()) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(tcp_sock_,
                reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) continue;

            HeartbeatPacket pkt;
            struct timeval tv{0, 500000};
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            ssize_t n = recv(client_fd, &pkt, sizeof(pkt), MSG_WAITALL);
            close(client_fd);

            if (n == sizeof(pkt) && pkt.magic == 0x41504558) {
                inject_heartbeat(pkt.node_id);
            }
        }
    }

    /// TCP probe: SUSPECT→DEAD 전이 전 보조 확인
    bool tcp_probe(const NodeAddress& target) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        struct timeval tv{0, 500000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(config_.tcp_heartbeat_port);
        inet_pton(AF_INET, target.host.c_str(), &addr.sin_addr);

        bool ok = (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                           sizeof(addr)) == 0);
        close(fd);
        return ok;
    }

    void check_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            check_timeouts();
        }
    }

    void check_timeouts() {
        auto now = Clock::now();

        struct StateChange { NodeId id; NodeState from; NodeState to; };
        std::vector<StateChange> changes;
        std::vector<std::pair<NodeId, NodeAddress>> tcp_probe_targets;

        {
            std::unique_lock lock(state_mutex_);

            // 1. 연속 miss 카운트 업데이트 (상태 판정 전에 수행)
            //    check_interval: heartbeat 간격 또는 suspect 판정에 필요한 간격 중 작은 값
            uint32_t check_interval = std::min(
                config_.heartbeat_interval_ms,
                config_.consecutive_misses_for_suspect > 0
                    ? config_.suspect_timeout_ms / config_.consecutive_misses_for_suspect
                    : config_.heartbeat_interval_ms);
            if (check_interval == 0) check_interval = 1;

            for (auto& [id, state] : node_states_) {
                if (id == self_node_.id) continue;
                if (state == NodeState::DEAD || state == NodeState::LEAVING ||
                    state == NodeState::REJOINING) continue;

                auto hb_it = last_heartbeat_.find(id);
                if (hb_it == last_heartbeat_.end()) continue;

                auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - hb_it->second).count();

                if (age_ms >= static_cast<int64_t>(check_interval)) {
                    uint32_t expected = static_cast<uint32_t>(age_ms / check_interval);
                    if (consecutive_misses_[id] < expected)
                        consecutive_misses_[id] = expected;
                }
            }

            // 2. 상태 전이 판정
            for (auto& [id, state] : node_states_) {
                if (id == self_node_.id) continue;
                if (state == NodeState::DEAD || state == NodeState::LEAVING ||
                    state == NodeState::REJOINING) continue;

                auto hb_it = last_heartbeat_.find(id);
                if (hb_it == last_heartbeat_.end()) continue;

                auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - hb_it->second).count();

                if (state == NodeState::ACTIVE || state == NodeState::JOINING) {
                    if (age_ms >= static_cast<int64_t>(config_.suspect_timeout_ms) &&
                        consecutive_misses_[id] >= config_.consecutive_misses_for_suspect) {
                        changes.push_back({id, state, NodeState::SUSPECT});
                    }
                } else if (state == NodeState::SUSPECT) {
                    if (age_ms >= static_cast<int64_t>(config_.dead_timeout_ms)) {
                        auto addr_it = node_addresses_.find(id);
                        if (addr_it != node_addresses_.end()) {
                            tcp_probe_targets.push_back({id, addr_it->second});
                        } else {
                            changes.push_back({id, state, NodeState::DEAD});
                        }
                    }
                }
            }
        }

        // TCP probe: SUSPECT → DEAD 전이 전 보조 확인
        for (auto& [id, addr] : tcp_probe_targets) {
            if (!tcp_probe(addr)) {
                changes.push_back({id, NodeState::SUSPECT, NodeState::DEAD});
            }
        }

        for (auto& c : changes) {
            transition_state(c.id, c.to);
        }
    }

    /// DEAD 노드가 heartbeat를 재개 → REJOINING → (재동기화) → ACTIVE
    void handle_dead_rejoin(NodeId node) {
        transition_state(node, NodeState::REJOINING);

        bool sync_ok = true;
        {
            std::shared_lock lock(callback_mutex_);
            if (rejoin_callback_) sync_ok = rejoin_callback_(node);
        }

        if (sync_ok) {
            transition_state(node, NodeState::ACTIVE);
        }
        // 실패 시 REJOINING 유지 — 다음 heartbeat에서 재시도
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

            if (new_state == NodeState::ACTIVE)
                consecutive_misses_[node] = 0;
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
    std::unordered_map<NodeId, uint32_t>            consecutive_misses_;

    mutable std::shared_mutex                       callback_mutex_;
    std::vector<StateCallback>                      callbacks_;
    RejoinCallback                                  rejoin_callback_;

    std::thread                                     send_thread_;
    std::thread                                     recv_thread_;
    std::thread                                     check_thread_;
    std::thread                                     tcp_recv_thread_;

    int                                             udp_sock_ = -1;
    int                                             tcp_sock_ = -1;
};

} // namespace zeptodb::cluster
