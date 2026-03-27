#pragma once
// ============================================================================
// RingConsensus — PartitionRouter 분산 동기화 인터페이스
// ============================================================================
// 구현체:
//   EpochBroadcastConsensus — coordinator가 epoch 기반 broadcast (기본)
//   (향후) RaftConsensus    — Raft 기반 strong consistency
//
// Coordinator 측: propose_add / propose_remove → 합의 후 모든 노드에 적용
// Follower 측:    on_ring_update 콜백으로 새 ring 수신
// ============================================================================

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/k8s_lease.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/cluster/rpc_protocol.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// RingSnapshot — 직렬화 가능한 ring 상태
// ============================================================================
struct RingSnapshot {
    uint64_t              epoch = 0;
    std::vector<NodeId>   nodes;   // 물리 노드 목록

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf;
        buf.reserve(8 + 4 + nodes.size() * 4);
        // epoch (8 bytes)
        uint64_t e = epoch;
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<uint8_t>(e & 0xFF));
            e >>= 8;
        }
        // node count (4 bytes)
        uint32_t n = static_cast<uint32_t>(nodes.size());
        for (int i = 0; i < 4; ++i) {
            buf.push_back(static_cast<uint8_t>(n & 0xFF));
            n >>= 8;
        }
        // node ids (4 bytes each)
        for (auto id : nodes) {
            uint32_t v = static_cast<uint32_t>(id);
            for (int i = 0; i < 4; ++i) {
                buf.push_back(static_cast<uint8_t>(v & 0xFF));
                v >>= 8;
            }
        }
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, RingSnapshot& out) {
        if (len < 12) return false;
        out.epoch = 0;
        for (int i = 7; i >= 0; --i)
            out.epoch = (out.epoch << 8) | data[i];

        uint32_t count = 0;
        for (int i = 3; i >= 0; --i)
            count = (count << 8) | data[8 + i];

        if (len < 12 + count * 4) return false;
        out.nodes.resize(count);
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t v = 0;
            size_t off = 12 + j * 4;
            for (int i = 3; i >= 0; --i)
                v = (v << 8) | data[off + i];
            out.nodes[j] = static_cast<NodeId>(v);
        }
        return true;
    }
};

// ============================================================================
// RingConsensus — 추상 인터페이스
// ============================================================================
class RingConsensus {
public:
    virtual ~RingConsensus() = default;

    /// Coordinator: 노드 추가 제안 → 합의 후 전체 적용
    virtual bool propose_add(NodeId node) = 0;

    /// Coordinator: 노드 제거 제안 → 합의 후 전체 적용
    virtual bool propose_remove(NodeId node) = 0;

    /// Follower: 수신한 ring update 적용
    virtual bool apply_update(const uint8_t* data, size_t len) = 0;

    /// 현재 ring epoch
    virtual uint64_t current_epoch() const = 0;
};

// ============================================================================
// EpochBroadcastConsensus — coordinator broadcast 구현
// ============================================================================
// - Coordinator가 ring 변경 시 epoch bump + 전체 노드에 RING_UPDATE 전송
// - Follower는 epoch >= last_seen일 때만 적용 (stale 거부)
// - FencingToken 재사용으로 split-brain 방지
// ============================================================================
class EpochBroadcastConsensus : public RingConsensus {
public:
    /// @param router       이 노드의 PartitionRouter (외부 소유)
    /// @param router_mu    router 보호용 shared_mutex (외부 소유)
    /// @param token        FencingToken (coordinator의 epoch 관리)
    EpochBroadcastConsensus(PartitionRouter& router,
                            std::shared_mutex& router_mu,
                            FencingToken& token)
        : router_(router), router_mu_(router_mu), token_(token) {}

    // ----------------------------------------------------------------
    // Peer 관리 (broadcast 대상)
    // ----------------------------------------------------------------

    void add_peer(NodeId id, const std::string& host, uint16_t rpc_port) {
        std::lock_guard lock(peers_mu_);
        peers_[id] = std::make_unique<TcpRpcClient>(host, rpc_port, 2000);
    }

    void remove_peer(NodeId id) {
        std::lock_guard lock(peers_mu_);
        peers_.erase(id);
    }

    // ----------------------------------------------------------------
    // Coordinator: propose changes
    // ----------------------------------------------------------------

    bool propose_add(NodeId node) override {
        {
            std::unique_lock lock(router_mu_);
            router_.add_node(node);
        }
        return broadcast_ring();
    }

    bool propose_remove(NodeId node) override {
        {
            std::unique_lock lock(router_mu_);
            router_.remove_node(node);
        }
        return broadcast_ring();
    }

    // ----------------------------------------------------------------
    // Follower: apply received update
    // ----------------------------------------------------------------

    bool apply_update(const uint8_t* data, size_t len) override {
        RingSnapshot snap;
        if (!RingSnapshot::deserialize(data, len, snap)) return false;

        // Stale epoch 거부
        if (!token_.validate(snap.epoch)) return false;

        // Router를 snapshot 기준으로 재구성
        std::unique_lock lock(router_mu_);
        auto current_nodes = router_.all_nodes();

        // 제거할 노드
        for (auto id : current_nodes) {
            bool found = false;
            for (auto sid : snap.nodes) {
                if (sid == id) { found = true; break; }
            }
            if (!found) router_.remove_node(id);
        }
        // 추가할 노드
        for (auto id : snap.nodes) {
            router_.add_node(id);  // 이미 있으면 내부에서 무시
        }

        return true;
    }

    uint64_t current_epoch() const override {
        return token_.current();
    }

private:
    bool broadcast_ring() {
        // Snapshot 생성
        RingSnapshot snap;
        snap.epoch = token_.advance();
        {
            std::shared_lock lock(router_mu_);
            snap.nodes = router_.all_nodes();
        }
        auto payload = snap.serialize();

        // 모든 peer에 전송 (best-effort, 실패 카운트 반환)
        size_t failures = 0;
        std::lock_guard lock(peers_mu_);
        for (auto& [id, client] : peers_) {
            if (!send_ring_update(*client, payload)) {
                ++failures;
            }
        }
        return failures == 0;
    }

    static bool send_ring_update(TcpRpcClient& client,
                                 const std::vector<uint8_t>& payload) {
        // RING_UPDATE는 기존 RPC 인프라의 execute_sql 경로를 빌려서 전송하지 않고,
        // 별도 low-level send를 사용. TcpRpcClient에 send_raw()이 없으므로
        // 간단한 TCP 전송으로 구현.
        int fd = -1;
        try {
            // connect
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            auto port_str = std::to_string(client.port());
            if (::getaddrinfo(client.host().c_str(), port_str.c_str(),
                              &hints, &res) != 0)
                return false;

            fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd < 0) { ::freeaddrinfo(res); return false; }

            if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
                ::freeaddrinfo(res); ::close(fd); return false;
            }
            ::freeaddrinfo(res);

            // Send RpcHeader + payload
            RpcHeader hdr{};
            hdr.type        = static_cast<uint32_t>(RpcType::RING_UPDATE);
            hdr.payload_len = static_cast<uint32_t>(payload.size());

            auto send_all = [&](const void* buf, size_t n) -> bool {
                const uint8_t* p = static_cast<const uint8_t*>(buf);
                size_t sent = 0;
                while (sent < n) {
                    auto r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
                    if (r <= 0) return false;
                    sent += static_cast<size_t>(r);
                }
                return true;
            };

            bool ok = send_all(&hdr, sizeof(hdr)) &&
                      send_all(payload.data(), payload.size());

            // Wait for RING_ACK (1 byte: 0x01 = applied)
            if (ok) {
                RpcHeader ack{};
                auto r = ::recv(fd, &ack, sizeof(ack), MSG_WAITALL);
                ok = (r == sizeof(ack) &&
                      ack.type == static_cast<uint32_t>(RpcType::RING_ACK));
            }

            ::close(fd);
            return ok;
        } catch (...) {
            if (fd >= 0) ::close(fd);
            return false;
        }
    }

    PartitionRouter&   router_;
    std::shared_mutex& router_mu_;
    FencingToken&      token_;

    std::mutex peers_mu_;
    std::unordered_map<NodeId, std::unique_ptr<TcpRpcClient>> peers_;
};

} // namespace zeptodb::cluster
