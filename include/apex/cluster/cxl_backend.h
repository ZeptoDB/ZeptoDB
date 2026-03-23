#pragma once
// ============================================================================
// CXL 3.0 Backend — NUMA 기반 시뮬레이션
// ============================================================================
// CXL 3.0 특성: load/store 시맨틱, 캐시 코히런시, ~150-300ns 레이턴시
// 시뮬레이션: mmap 풀 + 선택적 레이턴시 주입

#include "apex/cluster/transport.h"
#include "apex/common/logger.h"

#include <sys/mman.h>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <chrono>

namespace apex::cluster {

struct CXLConfig {
    size_t pool_size = 256ULL * 1024 * 1024;
    bool inject_latency = true;
    uint32_t latency_ns = 200;
};

class CXLBackend : public TransportBackend<CXLBackend> {
public:
    explicit CXLBackend(CXLConfig config = {}) : config_(config) {}
    ~CXLBackend() { do_shutdown(); }

    void do_init(const NodeAddress& self) {
        self_ = self;
        cxl_pool_ = ::mmap(nullptr, config_.pool_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (cxl_pool_ == MAP_FAILED) {
            throw std::runtime_error("CXLBackend: mmap failed");
        }
        std::memset(cxl_pool_, 0, config_.pool_size);
        pool_offset_.store(0);
        APEX_INFO("CXL Backend: pool={}MB latency={}ns inject={}",
                  config_.pool_size/(1024*1024), config_.latency_ns, config_.inject_latency);
    }

    void do_shutdown() {
        if (cxl_pool_ && cxl_pool_ != MAP_FAILED) {
            ::munmap(cxl_pool_, config_.pool_size);
            cxl_pool_ = nullptr;
        }
    }

    ConnectionId do_connect(const NodeAddress& peer) {
        ConnectionId c = next_conn_++;
        connections_[c] = peer;
        return c;
    }

    void do_disconnect(ConnectionId conn) { connections_.erase(conn); }

    RemoteRegion do_register_memory(void* addr, size_t size) {
        size_t off = pool_offset_.fetch_add(size);
        if (off + size > config_.pool_size) throw std::runtime_error("CXL pool full");
        void* dst = static_cast<char*>(cxl_pool_) + off;
        if (addr) std::memcpy(dst, addr, size);
        RemoteRegion r{};
        r.remote_addr = reinterpret_cast<uint64_t>(dst);
        r.rkey = 0;
        r.size = size;
        return r;
    }

    void do_deregister_memory(RemoteRegion& r) { r.remote_addr = 0; r.size = 0; }

    void do_remote_write(const void* local_src, const RemoteRegion& remote,
                         size_t offset, size_t bytes) {
        if (config_.inject_latency) nanospin(config_.latency_ns);
        void* dst = reinterpret_cast<void*>(remote.remote_addr + offset);
        std::memcpy(dst, local_src, bytes);
    }

    void do_remote_read(const RemoteRegion& remote, size_t offset,
                        void* local_dst, size_t bytes) {
        if (config_.inject_latency) nanospin(config_.latency_ns);
        const void* src = reinterpret_cast<const void*>(remote.remote_addr + offset);
        std::memcpy(local_dst, src, bytes);
    }

    void do_fence() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // CXL 고유: Direct Pointer Access (RDMA에 없는 기능)
    template <typename T>
    T* direct_access(const RemoteRegion& region, size_t offset = 0) {
        return reinterpret_cast<T*>(region.remote_addr + offset);
    }
    template <typename T>
    const T* direct_access(const RemoteRegion& region, size_t offset = 0) const {
        return reinterpret_cast<const T*>(region.remote_addr + offset);
    }

    [[nodiscard]] size_t pool_used() const { return pool_offset_.load(); }
    [[nodiscard]] size_t pool_capacity() const { return config_.pool_size; }
    [[nodiscard]] const CXLConfig& config() const { return config_; }

private:
    static void nanospin(uint32_t ns) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::nanoseconds(ns)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
    }

    CXLConfig config_;
    NodeAddress self_;
    void* cxl_pool_ = nullptr;
    std::atomic<size_t> pool_offset_{0};
    uint32_t next_conn_ = 1;
    std::unordered_map<ConnectionId, NodeAddress> connections_;
};

} // namespace apex::cluster
