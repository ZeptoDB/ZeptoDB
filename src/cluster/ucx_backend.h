#pragma once
// ============================================================================
// Phase C-1: UCXBackend — UCX 기반 RDMA/EFA 트랜스포트
// ============================================================================
// 프로덕션 백엔드: UCX UCP 레이어 사용
//   - ucp_put_nbi: Non-blocking one-sided write (RDMA PUT)
//   - ucp_get_nbi: Non-blocking one-sided read (RDMA GET)
//   - ucp_mem_map: 메모리 등록
//   - ucp_worker_fence: 메모리 순서 보장
//
// 컴파일 조건: APEX_HAS_UCX 매크로 (CMake에서 UCX 발견 시 정의)
// UCX 없으면 stub 구현으로 컴파일 (빌드 실패 방지)
// ============================================================================

#include "apex/cluster/transport.h"
#include <stdexcept>

#ifdef APEX_HAS_UCX
#include <ucp/api/ucp.h>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <atomic>
#endif

namespace apex::cluster {

#ifdef APEX_HAS_UCX

// ============================================================================
// UCX 연결 정보
// ============================================================================
struct UcxConnection {
    ucp_ep_h         ep;          // UCX endpoint
    NodeAddress      peer_addr;
    ConnectionId     id;
};

// ============================================================================
// UCXBackend: UCX 기반 RDMA 트랜스포트
// ============================================================================
class UCXBackend : public TransportBackend<UCXBackend> {
public:
    UCXBackend() = default;
    ~UCXBackend() { do_shutdown(); }

    void do_init(const NodeAddress& self) {
        self_node_ = self;

        // UCP context 파라미터 설정
        ucp_params_t params{};
        params.field_mask = UCP_PARAM_FIELD_FEATURES |
                            UCP_PARAM_FIELD_REQUEST_SIZE |
                            UCP_PARAM_FIELD_REQUEST_INIT;
        // RMA (원격 메모리 접근) + Wakeup 기능 활성화
        params.features   = UCP_FEATURE_RMA | UCP_FEATURE_WAKEUP;
        params.request_size = sizeof(ucx_request_t);
        params.request_init = request_init;

        ucs_status_t status = ucp_init(&params, nullptr, &context_);
        if (status != UCS_OK) {
            throw std::runtime_error(std::string("ucp_init failed: ") +
                                     ucs_status_string(status));
        }

        // Worker 생성 (단일 스레드 모드)
        ucp_worker_params_t wparams{};
        wparams.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
        wparams.thread_mode = UCS_THREAD_MODE_SINGLE;

        status = ucp_worker_create(context_, &wparams, &worker_);
        if (status != UCS_OK) {
            ucp_cleanup(context_);
            throw std::runtime_error(std::string("ucp_worker_create failed: ") +
                                     ucs_status_string(status));
        }

        initialized_ = true;
    }

    void do_shutdown() {
        if (!initialized_) return;

        // 모든 메모리 등록 해제
        for (auto& [rkey, info] : mem_map_) {
            if (info.mem_h != nullptr) {
                ucp_mem_unmap(context_, info.mem_h);
            }
            if (info.rkey_buf != nullptr) {
                ucp_rkey_buffer_release(info.rkey_buf);
            }
        }
        mem_map_.clear();

        // 모든 endpoint 닫기
        for (auto& [conn_id, conn] : connections_) {
            ucp_ep_destroy(conn.ep);
        }
        connections_.clear();

        if (worker_)  { ucp_worker_destroy(worker_); worker_ = nullptr; }
        if (context_) { ucp_cleanup(context_); context_ = nullptr; }

        initialized_ = false;
    }

    ConnectionId do_connect(const NodeAddress& peer) {
        // Worker 주소 교환은 외부에서 (OOB 채널, 예: TCP)
        // 실제 구현에서는 peer.host:peer.port로 TCP 연결 후
        // worker address 교환 → ucp_ep_create
        //
        // 테스트 환경에서는 같은 프로세스 내 worker 간 연결
        ucp_ep_params_t ep_params{};
        ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS;
        ep_params.flags      = UCP_EP_PARAMS_FLAGS_NO_LOOPBACK;

        // 실제로는 peer worker address를 가져와야 함
        // 여기서는 간단히 loopback 연결 (동일 worker)
        ucp_address_t* worker_addr;
        size_t         addr_len;
        ucs_status_t   status = ucp_worker_get_address(worker_, &worker_addr, &addr_len);
        if (status != UCS_OK) {
            throw std::runtime_error("ucp_worker_get_address failed");
        }

        ep_params.field_mask |= UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        ep_params.address     = worker_addr;

        ucp_ep_h ep;
        status = ucp_ep_create(worker_, &ep_params, &ep);
        ucp_worker_release_address(worker_, worker_addr);

        if (status != UCS_OK) {
            throw std::runtime_error(std::string("ucp_ep_create failed: ") +
                                     ucs_status_string(status));
        }

        ConnectionId conn_id = next_conn_id_++;
        connections_[conn_id] = UcxConnection{ep, peer, conn_id};
        return conn_id;
    }

    void do_disconnect(ConnectionId conn) {
        auto it = connections_.find(conn);
        if (it == connections_.end()) return;
        // Graceful close
        ucp_request_param_t close_params{};
        close_params.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
        close_params.flags        = UCP_EP_CLOSE_FLAG_FORCE;
        void* req = ucp_ep_close_nbx(it->second.ep, &close_params);
        if (req != nullptr && UCS_PTR_IS_PTR(req)) {
            ucs_status_t status;
            do {
                ucp_worker_progress(worker_);
                status = ucp_request_check_status(req);
            } while (status == UCS_INPROGRESS);
            ucp_request_free(req);
        }
        connections_.erase(it);
    }

    RemoteRegion do_register_memory(void* addr, size_t size) {
        ucp_mem_map_params_t map_params{};
        map_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                                UCP_MEM_MAP_PARAM_FIELD_LENGTH;
        map_params.address    = addr;
        map_params.length     = size;

        ucp_mem_h mem_h;
        ucs_status_t status = ucp_mem_map(context_, &map_params, &mem_h);
        if (status != UCS_OK) {
            throw std::runtime_error(std::string("ucp_mem_map failed: ") +
                                     ucs_status_string(status));
        }

        // rkey 패킹 (원격 노드에 전달할 직렬화된 키)
        void*  rkey_buf;
        size_t rkey_size;
        status = ucp_rkey_pack(context_, mem_h, &rkey_buf, &rkey_size);
        if (status != UCS_OK) {
            ucp_mem_unmap(context_, mem_h);
            throw std::runtime_error(std::string("ucp_rkey_pack failed: ") +
                                     ucs_status_string(status));
        }

        uint32_t rkey_id = next_rkey_++;
        mem_map_[rkey_id] = MemInfo{mem_h, rkey_buf, rkey_size};

        RemoteRegion region;
        region.remote_addr = reinterpret_cast<uint64_t>(addr);
        region.rkey        = rkey_id;
        region.conn        = static_cast<ConnectionId>(self_node_.id);
        region.size        = size;
        return region;
    }

    void do_deregister_memory(RemoteRegion& region) {
        auto it = mem_map_.find(region.rkey);
        if (it == mem_map_.end()) return;

        ucp_rkey_buffer_release(it->second.rkey_buf);
        ucp_mem_unmap(context_, it->second.mem_h);
        mem_map_.erase(it);
        region = RemoteRegion{};
    }

    /// RDMA PUT (non-blocking)
    void do_remote_write(const void* local_src, const RemoteRegion& remote,
                         size_t offset, size_t bytes) {
        auto conn_it = connections_.find(remote.conn);
        if (conn_it == connections_.end()) {
            throw std::runtime_error("Invalid connection for remote_write");
        }

        // rkey 해제 (원격 메모리 접근 키 획득)
        // 실제로는 원격 노드의 rkey_buf를 받아서 ucp_ep_rkey_unpack으로 생성
        // 여기서는 로컬 메모리 맵에서 rkey 정보 사용
        auto rkey_it = mem_map_.find(remote.rkey);
        if (rkey_it == mem_map_.end()) {
            throw std::runtime_error("Invalid rkey for remote_write");
        }

        ucp_rkey_h rkey_h;
        ucs_status_t status = ucp_ep_rkey_unpack(
            conn_it->second.ep,
            rkey_it->second.rkey_buf,
            &rkey_h);
        if (status != UCS_OK) {
            throw std::runtime_error("ucp_ep_rkey_unpack failed");
        }

        ucp_request_param_t req_params{};
        req_params.op_attr_mask = 0;

        void* req = ucp_put_nbx(
            conn_it->second.ep,
            local_src,
            bytes,
            remote.remote_addr + offset,
            rkey_h,
            &req_params);

        // 완료 대기 (non-blocking → progress loop)
        wait_completion(req);
        ucp_rkey_destroy(rkey_h);
    }

    /// RDMA GET (non-blocking)
    void do_remote_read(const RemoteRegion& remote, size_t offset,
                        void* local_dst, size_t bytes) {
        auto conn_it = connections_.find(remote.conn);
        if (conn_it == connections_.end()) {
            throw std::runtime_error("Invalid connection for remote_read");
        }

        auto rkey_it = mem_map_.find(remote.rkey);
        if (rkey_it == mem_map_.end()) {
            throw std::runtime_error("Invalid rkey for remote_read");
        }

        ucp_rkey_h rkey_h;
        ucs_status_t status = ucp_ep_rkey_unpack(
            conn_it->second.ep,
            rkey_it->second.rkey_buf,
            &rkey_h);
        if (status != UCS_OK) {
            throw std::runtime_error("ucp_ep_rkey_unpack failed");
        }

        ucp_request_param_t req_params{};
        req_params.op_attr_mask = 0;

        void* req = ucp_get_nbx(
            conn_it->second.ep,
            local_dst,
            bytes,
            remote.remote_addr + offset,
            rkey_h,
            &req_params);

        wait_completion(req);
        ucp_rkey_destroy(rkey_h);
    }

    void do_fence() {
        if (!initialized_) return;
        ucp_worker_fence(worker_);
    }

    /// Worker progress pump (외부에서 주기적으로 호출)
    void progress() {
        if (worker_) ucp_worker_progress(worker_);
    }

private:
    struct ucx_request_t {
        int completed;
    };

    struct MemInfo {
        ucp_mem_h  mem_h;
        void*      rkey_buf;
        size_t     rkey_size;
    };

    static void request_init(void* req) {
        static_cast<ucx_request_t*>(req)->completed = 0;
    }

    static void request_callback(void* req, ucs_status_t /*status*/, void* /*user_data*/) {
        static_cast<ucx_request_t*>(req)->completed = 1;
    }

    void wait_completion(void* req) {
        if (req == nullptr) return;
        if (UCS_PTR_IS_ERR(req)) {
            throw std::runtime_error(std::string("UCX operation failed: ") +
                                     ucs_status_string(UCS_PTR_STATUS(req)));
        }
        if (UCS_PTR_IS_PTR(req)) {
            while (!static_cast<ucx_request_t*>(req)->completed) {
                ucp_worker_progress(worker_);
            }
            ucp_request_free(req);
        }
    }

    NodeAddress  self_node_;
    bool         initialized_ = false;

    ucp_context_h context_ = nullptr;
    ucp_worker_h  worker_  = nullptr;

    std::unordered_map<ConnectionId, UcxConnection> connections_;
    std::unordered_map<uint32_t, MemInfo>            mem_map_;

    ConnectionId next_conn_id_ = 1;
    uint32_t     next_rkey_    = 1;
};

#else // APEX_HAS_UCX 없을 때 — stub 구현

// ============================================================================
// UCXBackend Stub: UCX 없을 때 컴파일 에러 방지
// ============================================================================
class UCXBackend : public TransportBackend<UCXBackend> {
public:
    void do_init(const NodeAddress&) {
        throw std::runtime_error("UCX not available — use SharedMemBackend for testing");
    }
    void do_shutdown() {}
    ConnectionId do_connect(const NodeAddress&) { return INVALID_CONN_ID; }
    void do_disconnect(ConnectionId) {}
    RemoteRegion do_register_memory(void*, size_t) { return {}; }
    void do_deregister_memory(RemoteRegion&) {}
    void do_remote_write(const void*, const RemoteRegion&, size_t, size_t) {}
    void do_remote_read(const RemoteRegion&, size_t, void*, size_t) {}
    void do_fence() {}
};

#endif // APEX_HAS_UCX

// 기본 프로덕션 트랜스포트 타입 별칭
using UcxTransport = TransportBackend<UCXBackend>;

} // namespace apex::cluster
