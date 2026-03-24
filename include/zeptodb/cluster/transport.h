#pragma once
// ============================================================================
// Phase C-1: Transport 추상화 레이어
// ============================================================================
// 컴파일 타임 디스패치 (CRTP) — virtual call 오버헤드 제로
// hot path에 간접 호출 없음: 모든 메서드는 인라인 CRTP로 impl()에 위임
//
// 백엔드 구현체:
//   - SharedMemBackend: POSIX shm 기반, 개발/테스트용
//   - UCXBackend: RDMA/AWS EFA 프로덕션
// ============================================================================

#include "zeptodb/common/types.h"
#include <cstdint>
#include <string>

namespace zeptodb::cluster {

// ============================================================================
// 기본 타입 정의
// ============================================================================
using NodeId       = uint32_t;
using ConnectionId = uint64_t;

constexpr NodeId       INVALID_NODE_ID = UINT32_MAX;
constexpr ConnectionId INVALID_CONN_ID = UINT64_MAX;

// ============================================================================
// NodeAddress: 원격 노드 주소
// ============================================================================
struct NodeAddress {
    std::string host;   // IP 또는 hostname
    uint16_t    port;   // 포트 번호
    NodeId      id;     // 클러스터 내 노드 ID

    bool operator==(const NodeAddress& o) const {
        return id == o.id && port == o.port && host == o.host;
    }
};

// ============================================================================
// RemoteRegion: 원격 메모리 영역 핸들
// ============================================================================
struct RemoteRegion {
    uint64_t     remote_addr;  // 원격 가상 주소
    uint32_t     rkey;         // Remote key (RDMA), shm에선 shmid
    ConnectionId conn;         // 연결 ID
    size_t       size;         // 영역 크기 (bytes)

    // 로컬 포인터 (shm backend에서 실제 mmap 주소)
    void* local_mapped = nullptr;

    bool is_valid() const { return remote_addr != 0 && size > 0; }
};

// ============================================================================
// TransportBackend<Impl>: CRTP 기반 컴파일 타임 디스패치
// ============================================================================
// 실제 구현은 Impl(SharedMemBackend, UCXBackend 등)에 위임
// 핫 패스: remote_write/read는 virtual call 없이 직접 인라인
// ============================================================================
template <typename Impl>
class TransportBackend {
public:
    // ------------------------------------------------------------------
    // 생명주기
    // ------------------------------------------------------------------

    /// 트랜스포트 초기화 (로컬 노드 주소 바인딩)
    void init(const NodeAddress& self) {
        impl().do_init(self);
    }

    /// 트랜스포트 종료 (모든 연결/등록 해제)
    void shutdown() {
        impl().do_shutdown();
    }

    // ------------------------------------------------------------------
    // 연결 관리
    // ------------------------------------------------------------------

    /// 원격 노드에 연결
    ConnectionId connect(const NodeAddress& peer) {
        return impl().do_connect(peer);
    }

    /// 연결 해제
    void disconnect(ConnectionId conn) {
        impl().do_disconnect(conn);
    }

    // ------------------------------------------------------------------
    // 메모리 등록
    // ------------------------------------------------------------------

    /// 로컬 메모리를 트랜스포트에 등록 (원격 접근 허용)
    /// @return RemoteRegion: 원격 노드가 이 영역에 접근할 때 사용하는 핸들
    RemoteRegion register_memory(void* addr, size_t size) {
        return impl().do_register_memory(addr, size);
    }

    /// 메모리 등록 해제
    void deregister_memory(RemoteRegion& region) {
        impl().do_deregister_memory(region);
    }

    // ------------------------------------------------------------------
    // One-Sided 연산 (원격 노드 CPU 개입 없음)
    // ------------------------------------------------------------------

    /// 원격 쓰기 (RDMA PUT)
    /// local_src → remote[offset .. offset+bytes)
    void remote_write(const void* local_src, const RemoteRegion& remote,
                      size_t offset, size_t bytes) {
        impl().do_remote_write(local_src, remote, offset, bytes);
    }

    /// 원격 읽기 (RDMA GET)
    /// remote[offset .. offset+bytes) → local_dst
    void remote_read(const RemoteRegion& remote, size_t offset,
                     void* local_dst, size_t bytes) {
        impl().do_remote_read(remote, offset, local_dst, bytes);
    }

    /// 메모리 펜스: 이전 모든 원격 연산이 완료됨을 보장
    void fence() {
        impl().do_fence();
    }

    // ------------------------------------------------------------------
    // CRTP 접근자
    // ------------------------------------------------------------------
    Impl&       impl()       { return static_cast<Impl&>(*this); }
    const Impl& impl() const { return static_cast<const Impl&>(*this); }

    // CRTP 기반 클래스이지만 공개 생성자 허용
    // (Impl이 상속하므로 직접 인스턴스화 할 경우 CRTP 패턴이 깨짐에 주의)
    TransportBackend() = default;
};

} // namespace zeptodb::cluster
