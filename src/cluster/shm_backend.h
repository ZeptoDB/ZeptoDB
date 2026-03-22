#pragma once
// ============================================================================
// Phase C-1: SharedMemBackend — POSIX 공유 메모리 기반 트랜스포트
// ============================================================================
// 목적: RDMA 하드웨어 없이 단일/다중 프로세스에서 테스트
// 구현: shm_open + mmap으로 공유 메모리 생성
//       remote_write/read = memcpy to/from shared mapping
//
// 두 프로세스가 같은 shm 이름으로 접근하면 물리 메모리를 공유
// ============================================================================

#include "apex/cluster/transport.h"
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <stdexcept>
#include <cerrno>
#include <cstdio>

namespace apex::cluster {

// ============================================================================
// SharedMemRegionMeta: 공유 메모리 슬롯 메타데이터
// ============================================================================
struct SharedMemRegionMeta {
    uint64_t  addr_offset;  // shm 내 오프셋 (rkey로 인코딩)
    size_t    size;
    char      shm_name[64]; // /apex_shm_XXXXXXXX
    int       shm_fd;
    void*     local_ptr;    // 로컬 mmap 주소
};

// ============================================================================
// SharedMemBackend: POSIX shm 기반 트랜스포트 구현
// ============================================================================
class SharedMemBackend : public TransportBackend<SharedMemBackend> {
public:
    SharedMemBackend() = default;
    ~SharedMemBackend() { do_shutdown(); }

    // ------------------------------------------------------------------
    // TransportBackend CRTP 구현
    // ------------------------------------------------------------------

    void do_init(const NodeAddress& self) {
        self_node_ = self;
        initialized_ = true;
    }

    void do_shutdown() {
        if (!initialized_) return;
        // 모든 등록된 shm 해제
        for (auto& [key, meta] : registered_regions_) {
            if (meta.shm_ptr && meta.shm_ptr != MAP_FAILED) {
                munmap(meta.shm_ptr, meta.size);
            }
            if (meta.shm_fd >= 0) {
                close(meta.shm_fd);
            }
            // shm_unlink는 소유자(creator)만 호출
            if (meta.is_owner) {
                shm_unlink(meta.shm_name);
            }
        }
        registered_regions_.clear();
        initialized_ = false;
    }

    ConnectionId do_connect(const NodeAddress& peer) {
        // SharedMem에서는 연결 개념이 단순: peer ID를 ConnectionId로 사용
        ConnectionId conn = static_cast<ConnectionId>(peer.id);
        peer_map_[conn] = peer;
        return conn;
    }

    void do_disconnect(ConnectionId conn) {
        peer_map_.erase(conn);
    }

    /// 로컬 메모리 블록을 공유 메모리에 노출
    RemoteRegion do_register_memory(void* addr, size_t size) {
        // shm 이름: /apex_<node_id>_<ptr>
        char shm_name[64];
        snprintf(shm_name, sizeof(shm_name), "/apex_%u_%llx",
                 self_node_.id, (unsigned long long)(uintptr_t)addr);

        // POSIX shm 생성
        int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            throw std::runtime_error(std::string("shm_open failed: ") + strerror(errno));
        }

        if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
            close(fd);
            shm_unlink(shm_name);
            throw std::runtime_error(std::string("ftruncate failed: ") + strerror(errno));
        }

        // mmap으로 공유 메모리에 매핑
        void* shm_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if (shm_ptr == MAP_FAILED) {
            close(fd);
            shm_unlink(shm_name);
            throw std::runtime_error(std::string("mmap failed: ") + strerror(errno));
        }

        // Copy initial data if provided; nullptr means allocate empty region
        if (addr) std::memcpy(shm_ptr, addr, size);

        // 메타데이터 저장
        RegionMeta meta;
        meta.size     = size;
        meta.shm_fd   = fd;
        meta.shm_ptr  = shm_ptr;
        meta.is_owner = true;
        std::memcpy(meta.shm_name, shm_name, sizeof(meta.shm_name));

        uint32_t rkey = next_rkey_++;
        registered_regions_[rkey] = meta;

        RemoteRegion region;
        region.remote_addr  = reinterpret_cast<uint64_t>(shm_ptr);
        region.rkey         = rkey;
        region.conn         = static_cast<ConnectionId>(self_node_.id);
        region.size         = size;
        region.local_mapped = shm_ptr;

        return region;
    }

    void do_deregister_memory(RemoteRegion& region) {
        auto it = registered_regions_.find(region.rkey);
        if (it == registered_regions_.end()) return;

        auto& meta = it->second;
        if (meta.shm_ptr && meta.shm_ptr != MAP_FAILED) {
            munmap(meta.shm_ptr, meta.size);
        }
        if (meta.shm_fd >= 0) {
            close(meta.shm_fd);
        }
        if (meta.is_owner) {
            shm_unlink(meta.shm_name);
        }
        registered_regions_.erase(it);
        region = RemoteRegion{};
    }

    /// remote_write: local_src → shm[offset .. offset+bytes)
    void do_remote_write(const void* local_src, const RemoteRegion& remote,
                         size_t offset, size_t bytes) {
        void* dst = reinterpret_cast<void*>(remote.remote_addr + offset);
        std::memcpy(dst, local_src, bytes);
        // shm은 공유 매핑이므로 memcpy만으로 충분
    }

    /// remote_read: shm[offset .. offset+bytes) → local_dst
    void do_remote_read(const RemoteRegion& remote, size_t offset,
                        void* local_dst, size_t bytes) {
        const void* src = reinterpret_cast<const void*>(remote.remote_addr + offset);
        std::memcpy(local_dst, src, bytes);
    }

    /// 펜스: shm에서는 std::atomic_thread_fence로 충분
    void do_fence() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // ------------------------------------------------------------------
    // SharedMem 전용: 다른 프로세스의 shm에 attach (원격 접근용)
    // ------------------------------------------------------------------

    /// 원격 노드가 공개한 shm에 attach
    /// shm_name을 알고 있어야 함 (NodeId + ptr 기반)
    RemoteRegion attach_remote(const char* shm_name, size_t size) {
        int fd = shm_open(shm_name, O_RDWR, 0666);
        if (fd < 0) {
            throw std::runtime_error(std::string("attach shm_open failed: ") + strerror(errno));
        }

        void* shm_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if (shm_ptr == MAP_FAILED) {
            close(fd);
            throw std::runtime_error(std::string("attach mmap failed: ") + strerror(errno));
        }

        RegionMeta meta;
        meta.size     = size;
        meta.shm_fd   = fd;
        meta.shm_ptr  = shm_ptr;
        meta.is_owner = false;
        std::memcpy(meta.shm_name, shm_name, sizeof(meta.shm_name));

        uint32_t rkey = next_rkey_++;
        registered_regions_[rkey] = meta;

        RemoteRegion region;
        region.remote_addr  = reinterpret_cast<uint64_t>(shm_ptr);
        region.rkey         = rkey;
        region.conn         = INVALID_CONN_ID;
        region.size         = size;
        region.local_mapped = shm_ptr;

        return region;
    }

    /// 이 노드가 등록한 shm 이름 조회 (다른 프로세스가 attach할 때 사용)
    std::string get_shm_name(uint32_t rkey) const {
        auto it = registered_regions_.find(rkey);
        if (it == registered_regions_.end()) return "";
        return std::string(it->second.shm_name);
    }

private:
    struct RegionMeta {
        size_t size;
        int    shm_fd;
        void*  shm_ptr;
        bool   is_owner;
        char   shm_name[64];
    };

    NodeAddress                              self_node_;
    bool                                     initialized_ = false;
    std::unordered_map<ConnectionId, NodeAddress> peer_map_;
    std::unordered_map<uint32_t, RegionMeta> registered_regions_;
    std::unordered_map<ConnectionId, NodeAddress> remote_nodes_;
    uint32_t                                 next_rkey_ = 1;
};

// 타입 별칭
using ShmTransport = TransportBackend<SharedMemBackend>;

} // namespace apex::cluster
